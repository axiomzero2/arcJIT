// SPDX-License-Identifier: MIT
#include "runtime/runtime.h"

#include <chrono>
#include <cstring>
#include <print>

namespace arcjit {

// --- enkiTS task for background Tier-1 compilation -------------------------
namespace {

class Tier1CompileTask : public enki::ITaskSet {
public:
    const Chunk*       chunk;
    ChunkEntry*        entry;
    CompilationStats*  stats;
    std::mutex*        chunks_mu;
    std::unordered_map<const Chunk*, std::unique_ptr<ChunkEntry>>* chunks;

    explicit Tier1CompileTask(const Chunk* c, ChunkEntry* e, CompilationStats* s)
        : chunk(c), entry(e), stats(s) {}

    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        auto t0 = std::chrono::high_resolution_clock::now();

        auto maybe_fn = lower_chunk_to_tier1(*chunk, "tier1");
        if (!maybe_fn) {
            entry->tier1_compiling = false;
            return;
        }
        auto fn = std::make_unique<Tier1Function>(std::move(*maybe_fn));

        // Compile to machine code.
        static thread_local std::unique_ptr<Tier1Compiler> compiler;
        if (!compiler) compiler = std::make_unique<Tier1Compiler>();
        auto maybe_entry = compiler->compile(*fn);
        if (!maybe_entry) {
            entry->tier1_compiling = false;
            return;
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        stats->total_compilation_time_us +=
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        stats->tier1_compiles++;

        {
            std::lock_guard<std::mutex> g(entry->compile_mu);
            entry->tier1_fn   = std::move(fn);
            entry->tier1_entry = *maybe_entry;
            entry->current_tier.store(Tier::Tier1Baseline, std::memory_order_release);
        }
        entry->tier1_compiling = false;
    }
};

class Tier2CompileTask : public enki::ITaskSet {
public:
    const Chunk*       chunk;
    ChunkEntry*        entry;
    CompilationStats*  stats;

    explicit Tier2CompileTask(const Chunk* c, ChunkEntry* e, CompilationStats* s)
        : chunk(c), entry(e), stats(s) {}

    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        auto t0 = std::chrono::high_resolution_clock::now();

        // Lower Chunk → Tier-1 first (Tier-2 takes Tier-1 as input).
        auto maybe_fn = lower_chunk_to_tier1(*chunk, "tier2_input");
        if (!maybe_fn) {
            entry->tier2_compiling = false;
            return;
        }

        // Run the full Tier-2 pipeline: lower to SoN, optimize, lower back.
        auto maybe_entry = compile_at_tier2(*maybe_fn);
        if (!maybe_entry) {
            entry->tier2_compiling = false;
            return;
        }
        auto fn = std::make_unique<Tier1Function>(std::move(*maybe_fn));

        auto t1 = std::chrono::high_resolution_clock::now();
        stats->total_compilation_time_us +=
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        stats->tier2_compiles++;

        {
            std::lock_guard<std::mutex> g(entry->compile_mu);
            entry->tier2_fn   = std::move(fn);
            entry->tier2_entry = *maybe_entry;
            entry->current_tier.store(Tier::Tier2Optimizing, std::memory_order_release);
        }
        entry->tier2_compiling = false;
    }
};

}  // namespace

// --- Runtime ----------------------------------------------------------------

Runtime::Runtime() {
    scheduler_ = std::make_unique<enki::TaskScheduler>();
    scheduler_->Initialize();

    // Initialize machinery.
    watchdog_  = std::make_unique<Watchdog>();
    meter_     = std::make_unique<Meter>();
    trip_      = std::make_unique<Trip>();
    capacitor_ = std::make_unique<Capacitor>();
    relay_     = std::make_unique<Relay>();
    regulator_ = std::make_unique<Regulator>();
    fuse_      = std::make_unique<Fuse>();

    // Detect CPU features (Probe is static, auto-detected on first access).
    (void)cpu_features();

    interp_ = std::make_unique<Interpreter>();
    interp_->attach_safepoint(&safepoint_mgr_);
    interp_->attach_meter(meter_.get());
}

Runtime::~Runtime() = default;

void Runtime::set_profiling_enabled(bool enabled) {
    // Detach or re-attach the Meter from the interpreter. The Meter itself
    // stays alive (it owns historical profile data we may still want); we
    // only flip the interpreter's pointer to it.
    interp_->attach_meter(enabled ? meter_.get() : nullptr);
}

ChunkEntry& Runtime::entry_for_(const Chunk& chunk) {
    std::lock_guard<std::mutex> g(chunks_mu_);
    auto it = chunks_.find(&chunk);
    if (it != chunks_.end()) return *it->second;
    auto e = std::make_unique<ChunkEntry>();
    ChunkEntry& ref = *e;
    chunks_[&chunk] = std::move(e);
    return ref;
}

void Runtime::maybe_compile_(ChunkEntry& entry, const Chunk& chunk) {
    uint32_t inv = entry.invocations.load(std::memory_order_relaxed);

    // Sampled Capacitor aging: age all allocations every ~256 slow-path
    // entries. The slow path runs when we're considering compilation, so
    // it's a natural place to do periodic housekeeping. Aging lets
    // Capacitor::evict_cold identify code that hasn't been called recently.
    // (The previous implementation never called age_all — hot/cold
    // classification was dead code.)
    static thread_local uint32_t age_counter = 0;
    if ((++age_counter & 0xFF) == 0) {
        capacitor_->age_all();
    }

    // Tier 0 → Tier 1
    if (inv >= kHotThreshold
        && entry.current_tier.load(std::memory_order_acquire) == Tier::Interpreter
        && !entry.tier1_compiling.load(std::memory_order_relaxed)
        && entry.tier1_entry == nullptr) {
        bool expected = false;
        if (entry.tier1_compiling.compare_exchange_strong(expected, true)) {
            // Allocate a fresh task per compile. The previous implementation
            // reused a static thread_local task, which silently corrupted
            // in-flight compiles when one thread scheduled two back-to-back:
            // the second `task->chunk = &chunk_B` overwrote the first
            // before enkiTS finished processing it.
            //
            // The task is heap-allocated and owned by the lambda captured
            // via shared_ptr in the task's ExecuteRange. (enkiTS doesn't
            // own the task — we must keep it alive until execution finishes.)
            //
            // For simplicity, we leak the task (process-lifetime allocation).
            // A production implementation would use a task pool or track
            // completion via a counter.
            auto* task = new Tier1CompileTask(&chunk, &entry, &stats_);
            scheduler_->AddTaskSetToPipe(task);
        }
    }

    // Tier 1 → Tier 2
    if (inv >= kTier2Threshold
        && entry.current_tier.load(std::memory_order_acquire) == Tier::Tier1Baseline
        && !entry.tier2_compiling.load(std::memory_order_relaxed)
        && entry.tier2_entry == nullptr) {
        bool expected = false;
        if (entry.tier2_compiling.compare_exchange_strong(expected, true)) {
            auto* task = new Tier2CompileTask(&chunk, &entry, &stats_);
            scheduler_->AddTaskSetToPipe(task);
        }
    }
}

[[nodiscard]] std::expected<Value, std::string> Runtime::run(const Chunk& chunk) {
    // Hot path: thread-local single-slot cache keyed by Chunk*.
    // The ChunkEntry* is stable for the chunk's lifetime (it lives behind a
    // unique_ptr in the chunks_ map), so we can cache it per-thread without
    // taking chunks_mu_ on every call.
    thread_local const Chunk* tls_cached_chunk = nullptr;
    thread_local ChunkEntry*  tls_cached_entry = nullptr;

    ChunkEntry* entry_ptr;
    if (tls_cached_chunk == &chunk) [[likely]] {
        entry_ptr = tls_cached_entry;
    } else {
        entry_ptr = &entry_for_(chunk);
        tls_cached_chunk = &chunk;
        tls_cached_entry = entry_ptr;
    }
    ChunkEntry& entry = *entry_ptr;

    // Relaxed — we only need an approximate count for the tier-up decision.
    entry.invocations.fetch_add(1, std::memory_order_relaxed);

    // Fast path: try to use the highest available compiled tier WITHOUT
    // taking the mutex. We read the entry pointer atomically (it's a
    // plain pointer — set under compile_mu, read lock-free here).
    // Relaxed is safe: if we read a stale nullptr, we fall through to the
    // next tier or slow path; the next call observes the updated value.
    // If we read a stale non-null while an invalidate is in flight, the
    // code is still valid until Trip patches the entry point (Rule 44).
    Tier current = entry.current_tier.load(std::memory_order_relaxed);
    if (current >= Tier::Tier2Optimizing) {
        CompiledEntry e2 = entry.tier2_entry;
        if (e2) {
            stats_.tier2_invocations.fetch_add(1, std::memory_order_relaxed);
            // Sampled Capacitor hit-tracking: 1/1024 calls pay the mutex
            // cost to update hot/cold stats. This is enough signal for
            // eviction decisions without adding per-call mutex overhead.
            // (The previous implementation never called record_hit at all
            // — hot/cold logic was dead code.)
            //
            // We use a thread-local counter (not the atomic) for the
            // sampling decision — reading the atomic would force a memory
            // barrier on every call, defeating the relaxed optimization.
            static thread_local uint32_t t2_hit_counter = 0;
            if ((++t2_hit_counter & 0x3FF) == 0) {
                capacitor_->record_hit(reinterpret_cast<void*>(e2));
            }
            // Thread-local locals buffer — avoids zeroing 2KB per call.
            // The previous implementation did `int64_t locals_buf[256] = {}`
            // which zero-inits all 256 entries even when max_locals is 0.
            // We only zero the entries that will actually be used.
            static thread_local int64_t t2_locals_buf[256];
            int n = chunk.max_locals();
            void* locals = nullptr;
            if (n > 0) {
                std::memset(t2_locals_buf, 0, n * sizeof(int64_t));
                locals = t2_locals_buf;
            }
            return Value::Int(e2(locals));
        }
    }
    if (current >= Tier::Tier1Baseline) {
        CompiledEntry e1 = entry.tier1_entry;
        if (e1) {
            stats_.tier1_invocations.fetch_add(1, std::memory_order_relaxed);
            static thread_local uint32_t t1_hit_counter = 0;
            if ((++t1_hit_counter & 0x3FF) == 0) {
                capacitor_->record_hit(reinterpret_cast<void*>(e1));
            }
            static thread_local int64_t t1_locals_buf[256];
            int n = chunk.max_locals();
            void* locals = nullptr;
            if (n > 0) {
                std::memset(t1_locals_buf, 0, n * sizeof(int64_t));
                locals = t1_locals_buf;
            }
            return Value::Int(e1(locals));
        }
    }

    // Slow path: check if we need to compile.
    maybe_compile_(entry, chunk);

    // Fall back to interpreter.
    stats_.interp_invocations.fetch_add(1, std::memory_order_relaxed);
    return interp_->run(chunk);
}

[[nodiscard]] std::expected<Value, std::string> Runtime::run_at_tier(const Chunk& chunk, Tier t) {
    if (t == Tier::Interpreter) {
        stats_.interp_invocations++;
        return interp_->run(chunk);
    }

    // Thread-local locals buffer — avoids zeroing 2KB per call.
    // Only zero the entries that will actually be used.
    static thread_local int64_t rat_locals_buf[256];
    int n = chunk.max_locals();
    void* locals = nullptr;
    if (n > 0) {
        std::memset(rat_locals_buf, 0, n * sizeof(int64_t));
        locals = rat_locals_buf;
    }

    if (t == Tier::Tier1Baseline) {
        // Always compile fresh — run_at_tier is the test/benchmark API.
        // The hot path is Runtime::run(), which caches compiled code.
        auto maybe = compile_tier1(chunk);
        if (!maybe) return std::unexpected(maybe.error());
        stats_.tier1_invocations++;
        return Value::Int((*maybe)(locals));
    }
    if (t == Tier::Tier2Optimizing) {
        auto maybe = compile_tier2(chunk);
        if (!maybe) return std::unexpected(maybe.error());
        stats_.tier2_invocations++;
        return Value::Int((*maybe)(locals));
    }
    return std::unexpected("unknown tier");
}

[[nodiscard]] std::expected<int64_t (*)(void*), std::string>
Runtime::compile_tier1(const Chunk& chunk) {
    auto maybe_fn = lower_chunk_to_tier1(chunk, "tier1");
    if (!maybe_fn) return std::unexpected(maybe_fn.error());

    ChunkEntry& entry = entry_for_(chunk);
    static thread_local std::unique_ptr<Tier1Compiler> compiler;
    if (!compiler) compiler = std::make_unique<Tier1Compiler>();
    auto maybe_entry = compiler->compile(*maybe_fn);
    if (!maybe_entry) return std::unexpected(maybe_entry.error());

    {
        std::lock_guard<std::mutex> g(entry.compile_mu);
        entry.tier1_fn   = std::make_unique<Tier1Function>(std::move(*maybe_fn));
        entry.tier1_entry = *maybe_entry;
        entry.current_tier.store(Tier::Tier1Baseline, std::memory_order_release);

        // Register with Trip (code invalidation) and Capacitor (code cache).
        entry.tier1_code_id = trip_->register_code(
            reinterpret_cast<void*>(*maybe_entry), 0, "jolt");
        capacitor_->register_allocation(
            reinterpret_cast<void*>(*maybe_entry), 0, true);

        // Register a Watchdog assumption for this chunk (TypeStable).
        if (entry.assumption_id == 0) {
            entry.assumption_id = watchdog_->register_assumption(
                AssumptionKind::TypeStable,
                reinterpret_cast<uint64_t>(&chunk),
                nullptr, "chunk_type_stable");
        }
    }
    stats_.tier1_compiles++;
    return entry.tier1_entry;
}

[[nodiscard]] std::expected<int64_t (*)(void*), std::string>
Runtime::compile_tier2(const Chunk& chunk) {
    auto maybe_fn = lower_chunk_to_tier1(chunk, "tier2_input");
    if (!maybe_fn) return std::unexpected(maybe_fn.error());

    auto maybe_entry = compile_at_tier2(*maybe_fn);
    if (!maybe_entry) return std::unexpected(maybe_entry.error());

    ChunkEntry& entry = entry_for_(chunk);
    {
        std::lock_guard<std::mutex> g(entry.compile_mu);
        entry.tier2_fn   = std::make_unique<Tier1Function>(std::move(*maybe_fn));
        entry.tier2_entry = *maybe_entry;
        entry.current_tier.store(Tier::Tier2Optimizing, std::memory_order_release);

        // Register with Trip and Capacitor.
        entry.tier2_code_id = trip_->register_code(
            reinterpret_cast<void*>(*maybe_entry), 0, "surge");
        capacitor_->register_allocation(
            reinterpret_cast<void*>(*maybe_entry), 0, true);

        // Add dependency: Trip code depends on Watchdog assumption.
        if (entry.assumption_id != 0) {
            trip_->add_dependency(entry.tier2_code_id, entry.assumption_id);
        }
    }
    stats_.tier2_compiles++;
    return entry.tier2_entry;
}

[[nodiscard]] std::expected<int64_t, std::string>
Runtime::osr_to_tier1(const Chunk& chunk, void* locals_base) {
    auto maybe_entry = compile_tier1(chunk);
    if (!maybe_entry) return std::unexpected(maybe_entry.error());
    stats_.osr_transitions++;
    // If no locals base provided, allocate a zero-initialized one.
    std::vector<int64_t> locals_buf;
    if (!locals_base) {
        locals_buf.assign(std::max(chunk.max_locals(), 1), 0);
        locals_base = locals_buf.data();
    }
    return (*maybe_entry)(locals_base);
}

[[nodiscard]] std::expected<int64_t, std::string>
Runtime::osr_to_tier2(const Chunk& chunk, void* locals_base) {
    auto maybe_entry = compile_tier2(chunk);
    if (!maybe_entry) return std::unexpected(maybe_entry.error());
    stats_.osr_transitions++;
    std::vector<int64_t> locals_buf;
    if (!locals_base) {
        locals_buf.assign(std::max(chunk.max_locals(), 1), 0);
        locals_base = locals_buf.data();
    }
    return (*maybe_entry)(locals_base);
}

[[nodiscard]] std::expected<int64_t, std::string> Runtime::run_tier1_demo() {
    auto t0 = std::chrono::high_resolution_clock::now();

    Tier1Function fn = make_demo_add3();
    static thread_local std::unique_ptr<Tier1Compiler> compiler;
    if (!compiler) compiler = std::make_unique<Tier1Compiler>();
    auto maybe_entry = compiler->compile(fn);
    if (!maybe_entry) {
        return std::unexpected(maybe_entry.error());
    }
    auto entry = *maybe_entry;
    int64_t result = entry(nullptr);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.tier1_compiles++;
    stats_.total_compilation_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return result;
}

[[nodiscard]] std::string Runtime::run_tier2_demo() {
    auto t0 = std::chrono::high_resolution_clock::now();

    Tier2Job job;
    job.function_name = "demo";
    build_demo_graph(job.graph);

    PassResult r = run_tier2_pipeline(job);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.tier2_compiles++;
    stats_.total_compilation_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    std::string out;
    out += std::format("Surge (Gigavolt) demo pipeline finished:\n");
    out += std::format("  changed        : {}\n", r.changed);
    out += std::format("  nodes removed  : {}\n", r.nodes_removed);
    out += std::format("  nodes added    : {}\n", r.nodes_added);
    out += std::format("  time           : {} us\n",
                       std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    out += "\n";
    out += dump_graph_dot(job.graph);
    return out;
}

// --- Machinery integration --------------------------------------------------

[[nodiscard]] std::string Runtime::dump_machinery() const {
    std::string out;
    out += "=== arcJIT Machinery State ===\n\n";
    out += watchdog_->dump();
    out += "\n";
    out += meter_->dump();
    out += "\n";
    out += cpu_features().dump();
    out += "\n";
    out += regulator_->dump();
    out += "\n";
    out += fuse_->dump();
    out += "\n";
    out += trip_->dump();
    out += "\n";
    out += capacitor_->dump();
    out += "\n";
    out += relay_->dump();
    return out;
}

void Runtime::invalidate_chunk(const Chunk& chunk) {
    std::lock_guard<std::mutex> g(chunks_mu_);
    auto it = chunks_.find(&chunk);
    if (it == chunks_.end()) return;
    ChunkEntry& entry = *it->second;

    // Invalidate the Watchdog assumption (triggers Trip cascade).
    if (entry.assumption_id != 0) {
        watchdog_->invalidate(entry.assumption_id);
    }

    // Directly invalidate compiled code via Trip.
    if (entry.tier1_code_id != 0) {
        trip_->invalidate(entry.tier1_code_id);
    }
    if (entry.tier2_code_id != 0) {
        trip_->invalidate(entry.tier2_code_id);
    }

    // Reset the tier to interpreter.
    entry.current_tier.store(Tier::Interpreter, std::memory_order_release);

    // Clear compiled entries.
    {
        std::lock_guard<std::mutex> cg(entry.compile_mu);
        entry.tier1_entry = nullptr;
        entry.tier2_entry = nullptr;
        entry.tier1_fn.reset();
        entry.tier2_fn.reset();
    }
}

}  // namespace arcjit
