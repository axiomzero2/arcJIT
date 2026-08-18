// SPDX-License-Identifier: MIT
// arcJIT — Overhead Breakdown Profiler
//
// Breaks down the 4747 ns/op `run_at_tier` overhead into individual components
// to identify which guards/machinery actually cost time.
//
// Components measured:
//   A. lower_chunk_to_tier1 (bytecode → Tier-1 IR)
//   B. Tier1Compiler::compile (asmjit code emission)
//   C. entry_for_ mutex + hash lookup
//   D. trip_->register_code (mutex + vector push + string copy)
//   E. capacitor_->register_allocation (mutex + unordered_map insert)
//   F. watchdog_->register_assumption (mutex + vector push + map insert)
//   G. int64_t locals_buf[256] = {} (2KB stack zero-init)
//   H. Atomic counter increments (3 of them per call)
//   I. Value::Int + std::expected construction
//   J. Function pointer call itself (baseline)
//
// Then compares:
//   - direct entry call (no runtime)
//   - run_at_tier (full path, recompiles every iter)
//   - run() after warmup (cached entry, real hot path)
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory>
#include <print>
#include <vector>

#include "runtime/runtime.h"
#include "tier1/tier1.h"
#include "machinery/watchdog.h"
#include "machinery/trip.h"
#include "machinery/capacitor.h"

using namespace arcjit;

static auto now_ns() {
    return std::chrono::high_resolution_clock::now();
}
static int64_t elapsed_ns(auto t0, auto t1) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

static Object* make_num(int64_t v) {
    static std::vector<std::unique_ptr<Number>> pool;
    auto n = std::make_unique<Number>();
    n->base.type = ObjType::NumberInt; n->base.ref_count = 1;
    n->base.is_static = true; n->as.i = v;
    Object* p = reinterpret_cast<Object*>(n.get());
    pool.push_back(std::move(n));
    return p;
}

// arith chunk: 1+2+3+4+5
static Chunk make_arith_chunk() {
    Chunk c;
    c.set_max_locals(0);
    for (int i = 1; i <= 5; ++i) c.add_const(make_num(i));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(2);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(3);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(4);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);
    return c;
}

int main() {
    constexpr int N = 1'000'000;
    Runtime rt;
    rt.set_profiling_enabled(false);
    Chunk c = make_arith_chunk();

    std::println("=== arcJIT Overhead Breakdown ===");
    std::println("Iterations: {}\n", N);

    // ----- Baselines ---------------------------------------------------------
    // Pre-compile once, then call directly.
    auto maybe_e = rt.compile_tier1(c);
    auto entry = *maybe_e;
    int64_t locals_buf[256] = {};

    {
        entry(locals_buf); // warmup
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) entry(locals_buf);
        auto t1 = now_ns();
        std::println("J. direct entry call (baseline):           {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- A. lower_chunk_to_tier1 alone ------------------------------------
    {
        // Warmup
        for (int i = 0; i < 1000; ++i) lower_chunk_to_tier1(c, "bench");
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) lower_chunk_to_tier1(c, "bench");
        auto t1 = now_ns();
        std::println("A. lower_chunk_to_tier1 only:              {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- B. asmjit compile only (lower once, recompile many) --------------
    {
        auto fn = lower_chunk_to_tier1(c, "bench");
        static thread_local std::unique_ptr<Tier1Compiler> compiler;
        if (!compiler) compiler = std::make_unique<Tier1Compiler>();
        // Warmup
        for (int i = 0; i < 100; ++i) compiler->compile(*fn);
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) compiler->compile(*fn);
        auto t1 = now_ns();
        std::println("B. Tier1Compiler::compile (asmjit):        {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- A+B combined (what compile_tier1 does, minus machinery) ---------
    {
        static thread_local std::unique_ptr<Tier1Compiler> compiler;
        if (!compiler) compiler = std::make_unique<Tier1Compiler>();
        for (int i = 0; i < 100; ++i) {
            auto fn = lower_chunk_to_tier1(c, "bench");
            compiler->compile(*fn);
        }
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            auto fn = lower_chunk_to_tier1(c, "bench");
            compiler->compile(*fn);
        }
        auto t1 = now_ns();
        std::println("A+B. lower + asmjit compile:               {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- C. entry_for_ mutex + hash lookup --------------------------------
    // We can't call entry_for_ directly (private), so use compile_tier1
    // and subtract A+B+D+E+F.
    // For an isolated mutex benchmark:
    {
        static std::mutex mu;
        static std::unordered_map<const Chunk*, int> map;
        map[&c] = 1;
        volatile int sink = 0;
        for (int i = 0; i < 1000; ++i) {
            std::lock_guard<std::mutex> g(mu);
            sink += map[&c];
        }
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            std::lock_guard<std::mutex> g(mu);
            sink += map[&c];
        }
        auto t1 = now_ns();
        std::println("C. mutex+unordered_map lookup (sim):       {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- D. Trip::register_code alone ------------------------------------
    {
        Trip trip;
        void* fake_entry = reinterpret_cast<void*>(0x1000);
        for (int i = 0; i < 1000; ++i) trip.register_code(fake_entry, 0, "jolt");
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) trip.register_code(fake_entry, 0, "jolt");
        auto t1 = now_ns();
        std::println("D. Trip::register_code:                    {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- E. Capacitor::register_allocation alone -------------------------
    {
        Capacitor cap;
        void* fake_entry = reinterpret_cast<void*>(0x2000);
        for (int i = 0; i < 1000; ++i) cap.register_allocation(fake_entry, 0, true);
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) cap.register_allocation(fake_entry, 0, true);
        auto t1 = now_ns();
        std::println("E. Capacitor::register_allocation:         {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- F. Watchdog::register_assumption alone --------------------------
    {
        Watchdog wd;
        uint64_t fake_payload = 0xDEADBEEF;
        for (int i = 0; i < 1000; ++i)
            wd.register_assumption(AssumptionKind::TypeStable, fake_payload, nullptr, "x");
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i)
            wd.register_assumption(AssumptionKind::TypeStable, fake_payload, nullptr, "x");
        auto t1 = now_ns();
        std::println("F. Watchdog::register_assumption:          {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- G. int64_t locals_buf[256] = {} (2KB stack zero-init) ----------
    {
        volatile int64_t sink = 0;
        for (int i = 0; i < 1000; ++i) {
            int64_t buf[256] = {};
            sink += buf[0];
        }
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            int64_t buf[256] = {};
            sink += buf[0];
        }
        auto t1 = now_ns();
        std::println("G. int64_t[256]={{}} stack zero-init (2KB): {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- H. Atomic counter increments ------------------------------------
    {
        std::atomic<uint64_t> a{0}, b{0}, cc{0};
        for (int i = 0; i < 1000; ++i) {
            a.fetch_add(1, std::memory_order_relaxed);
            b.fetch_add(1, std::memory_order_relaxed);
            cc.fetch_add(1, std::memory_order_relaxed);
        }
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            a.fetch_add(1, std::memory_order_relaxed);
            b.fetch_add(1, std::memory_order_relaxed);
            cc.fetch_add(1, std::memory_order_relaxed);
        }
        auto t1 = now_ns();
        std::println("H. 3x atomic fetch_add(relaxed):           {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- I. Value::Int + std::expected construction ---------------------
    {
        for (int i = 0; i < 1000; ++i) {
            std::expected<Value, std::string> r = Value::Int(42);
            (void)r;
        }
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) {
            std::expected<Value, std::string> r = Value::Int(42);
            (void)r;
        }
        auto t1 = now_ns();
        std::println("I. Value::Int + std::expected ctor:        {} ns/op",
                     elapsed_ns(t0,t1)/N);
    }

    // ----- Full run_at_tier(Tier1Baseline) ----------------------------------
    {
        rt.run_at_tier(c, Tier::Tier1Baseline); // warmup
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) rt.run_at_tier(c, Tier::Tier1Baseline);
        auto t1 = now_ns();
        std::println("\nFULL run_at_tier(Tier1Baseline):           {} ns/op",
                     elapsed_ns(t0,t1)/N);
        std::println("    minus baseline entry call:             {} ns/op (overhead)",
                     elapsed_ns(t0,t1)/N - 1);
    }

    // ----- Real hot path: Runtime::run() after warmup (cached entry) -------
    // Bump invocations past kHotThreshold so tier1 is compiled & cached.
    {
        // Warmup: trigger background compile and wait for tier1_entry to be set.
        for (int i = 0; i < 5000; ++i) (void)rt.run(c);
        // Wait for tier1 to be ready.
        for (int spin = 0; spin < 1000000; ++spin) {
            // run() will use tier1_entry once it's set.
            auto r = rt.run(c);
            if (r && r->as_int() == 15) break;
        }
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) (void)rt.run(c);
        auto t1 = now_ns();
        std::println("HOT PATH Runtime::run() (cached tier1):    {} ns/op",
                     elapsed_ns(t0,t1)/N);
        std::println("     minus baseline entry call:            {} ns/op (overhead)",
                     elapsed_ns(t0,t1)/N - 1);
    }

    return 0;
}
