// SPDX-License-Identifier: MIT
#include "runtime/runtime.h"

#include <chrono>
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

    interp_ = std::make_unique<Interpreter>();
    interp_->attach_safepoint(&safepoint_mgr_);
}

Runtime::~Runtime() = default;

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

    // Tier 0 → Tier 1
    if (inv >= kHotThreshold
        && entry.current_tier.load(std::memory_order_acquire) == Tier::Interpreter
        && !entry.tier1_compiling.load(std::memory_order_relaxed)
        && entry.tier1_entry == nullptr) {
        bool expected = false;
        if (entry.tier1_compiling.compare_exchange_strong(expected, true)) {
            static thread_local std::unique_ptr<Tier1CompileTask> task;
            if (!task) task = std::make_unique<Tier1CompileTask>(&chunk, &entry, &stats_);
            task->chunk = &chunk;
            task->entry = &entry;
            scheduler_->AddTaskSetToPipe(task.get());
        }
    }

    // Tier 1 → Tier 2
    if (inv >= kTier2Threshold
        && entry.current_tier.load(std::memory_order_acquire) == Tier::Tier1Baseline
        && !entry.tier2_compiling.load(std::memory_order_relaxed)
        && entry.tier2_entry == nullptr) {
        bool expected = false;
        if (entry.tier2_compiling.compare_exchange_strong(expected, true)) {
            static thread_local std::unique_ptr<Tier2CompileTask> task;
            if (!task) task = std::make_unique<Tier2CompileTask>(&chunk, &entry, &stats_);
            task->chunk = &chunk;
            task->entry = &entry;
            scheduler_->AddTaskSetToPipe(task.get());
        }
    }
}

[[nodiscard]] std::expected<Value, std::string> Runtime::run(const Chunk& chunk) {
    ChunkEntry& entry = entry_for_(chunk);
    entry.invocations.fetch_add(1, std::memory_order_relaxed);

    // Try to use the highest available compiled tier.
    Tier current = entry.current_tier.load(std::memory_order_acquire);
    if (current >= Tier::Tier2Optimizing) {
        std::lock_guard<std::mutex> g(entry.compile_mu);
        if (entry.tier2_entry) {
            stats_.tier2_invocations++;
            std::vector<int64_t> locals(std::max(chunk.max_locals(), 1), 0);
            return Value::Int(entry.tier2_entry(locals.data()));
        }
    }
    if (current >= Tier::Tier1Baseline) {
        std::lock_guard<std::mutex> g(entry.compile_mu);
        if (entry.tier1_entry) {
            stats_.tier1_invocations++;
            std::vector<int64_t> locals(std::max(chunk.max_locals(), 1), 0);
            return Value::Int(entry.tier1_entry(locals.data()));
        }
    }

    // Kick off background compilation if thresholds are met.
    maybe_compile_(entry, chunk);

    // Fall back to interpreter.
    stats_.interp_invocations++;
    return interp_->run(chunk);
}

[[nodiscard]] std::expected<Value, std::string> Runtime::run_at_tier(const Chunk& chunk, Tier t) {
    if (t == Tier::Interpreter) {
        stats_.interp_invocations++;
        return interp_->run(chunk);
    }
    if (t == Tier::Tier1Baseline) {
        auto maybe_entry = compile_tier1(chunk);
        if (!maybe_entry) return std::unexpected(maybe_entry.error());
        stats_.tier1_invocations++;
        // Allocate a locals buffer on the stack. The compiled code reads from
        // [r12 + slot*8] — zero-initialized so undef reads give 0.
        std::vector<int64_t> locals(std::max(chunk.max_locals(), 1), 0);
        return Value::Int((*maybe_entry)(locals.data()));
    }
    if (t == Tier::Tier2Optimizing) {
        auto maybe_entry = compile_tier2(chunk);
        if (!maybe_entry) return std::unexpected(maybe_entry.error());
        stats_.tier2_invocations++;
        std::vector<int64_t> locals(std::max(chunk.max_locals(), 1), 0);
        return Value::Int((*maybe_entry)(locals.data()));
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
    out += std::format("Tier-2 demo pipeline finished:\n");
    out += std::format("  changed        : {}\n", r.changed);
    out += std::format("  nodes removed  : {}\n", r.nodes_removed);
    out += std::format("  nodes added    : {}\n", r.nodes_added);
    out += std::format("  time           : {} us\n",
                       std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    out += "\n";
    out += dump_graph_dot(job.graph);
    return out;
}

}  // namespace arcjit
