// SPDX-License-Identifier: MIT
// arcJIT — Overhead profiler: measures where time goes in each tier.
//
// Measures:
//   1. Spark: dispatch overhead, stack operations, feedback recording
//   2. Jolt: function call overhead (locals alloc, Tier1Compiler lookup)
//   3. Surge: Gigavolt pipeline overhead (lowering, optimization, lowering back)
//   4. Pure function call overhead (no computation, just call+return)
//   5. Constant folding effectiveness
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <print>
#include <vector>

#include "runtime/runtime.h"
#include "tier1/tier1.h"
#include "tier2/tier2.h"
#include "machinery/meter.h"
#include "machinery/watchdog.h"

using namespace arcjit;

static auto now_ns() {
    return std::chrono::high_resolution_clock::now();
}
static int64_t elapsed_ns(auto t0, auto t1) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

// Helper: make a static Number
static Object* make_num(int64_t v) {
    static std::vector<std::unique_ptr<Number>> pool;
    auto n = std::make_unique<Number>();
    n->base.type = ObjType::NumberInt; n->base.ref_count = 1;
    n->base.is_static = true; n->as.i = v;
    Object* p = reinterpret_cast<Object*>(n.get());
    pool.push_back(std::move(n));
    return p;
}

// Build a minimal chunk: LoadConst 42; Return
static Chunk make_minimal_chunk() {
    Chunk c;
    c.set_max_locals(0);
    c.add_const(make_num(42));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);
    return c;
}

// Build a chunk with locals: local0=10; local1=20; return local0+local1
static Chunk make_locals_chunk() {
    Chunk c;
    c.set_max_locals(2);
    c.add_const(make_num(10));
    c.add_const(make_num(20));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);
    return c;
}

// Build a pure-arithmetic chunk: 1+2+3+4+5
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
    constexpr int N = 1000000;
    Runtime rt;
    rt.set_profiling_enabled(false);  // we measure raw interpreter cost

    std::println("=== arcJIT Overhead Profiler ===");
    std::println("Iterations: {}", N);
    std::println("");

    // --- 1. Pure function call overhead (minimal chunk) ---
    {
        Chunk c = make_minimal_chunk();
        auto r = rt.run_at_tier(c, Tier::Interpreter);
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) rt.run_at_tier(c, Tier::Interpreter);
        auto t1 = now_ns();
        std::println("Spark minimal (LoadConst+Return):     {} ns/op", elapsed_ns(t0,t1)/N);

        auto maybe_e = rt.compile_tier1(c);
        auto entry = *maybe_e;
        auto t2 = now_ns();
        for (int i = 0; i < N; ++i) entry(nullptr);
        auto t3 = now_ns();
        std::println("Jolt  minimal (compiled entry call):  {} ns/op", elapsed_ns(t2,t3)/N);
    }

    // --- 2. run_at_tier overhead vs direct entry call ---
    {
        Chunk c = make_arith_chunk();
        auto maybe_e = rt.compile_tier1(c);
        auto entry = *maybe_e;

        // Direct entry call (no Runtime overhead).
        std::vector<int64_t> locals(1, 0);
        entry(locals.data()); // warmup
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) entry(locals.data());
        auto t1 = now_ns();
        std::println("\nJolt  direct entry call:               {} ns/op", elapsed_ns(t0,t1)/N);

        // Via run_at_tier (includes Runtime::entry_for_, mutex, locals alloc).
        auto t2 = now_ns();
        for (int i = 0; i < N; ++i) rt.run_at_tier(c, Tier::Tier1Baseline);
        auto t3 = now_ns();
        std::println("Jolt  via run_at_tier:                 {} ns/op", elapsed_ns(t2,t3)/N);
        std::println("      Runtime overhead:                {} ns/op", (elapsed_ns(t2,t3) - elapsed_ns(t0,t1))/N);
    }

    // --- 3. Spark overhead breakdown ---
    {
        Chunk c = make_arith_chunk();

        // With Meter attached (profile recording).
        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) rt.run_at_tier(c, Tier::Interpreter);
        auto t1 = now_ns();
        std::println("\nSpark with Meter:                      {} ns/op", elapsed_ns(t0,t1)/N);

        // Meter summary.
        auto s = rt.meter().summarize();
        std::println("  Meter: {} sites, {} samples, {} deopts", s.total_sites, s.total_samples, s.total_deopts);
    }

    // --- 4. Surge compile breakdown ---
    {
        Chunk c = make_arith_chunk();
        auto fn = lower_chunk_to_tier1(c, "bench");

        // Tier-1 → SoN lowering.
        auto t0 = now_ns();
        Tier2Job job;
        job.function_name = "bench";
        lower_tier1_to_son(*fn, job);
        auto t1 = now_ns();
        std::println("\nSurge lowering (Tier1→SoN):            {} us", elapsed_ns(t0,t1)/1000);

        // Gigavolt pipeline.
        auto t2 = now_ns();
        run_tier2_pipeline(job);
        auto t3 = now_ns();
        std::println("Surge Gigavolt pipeline:               {} us", elapsed_ns(t2,t3)/1000);

        // SoN → Tier-1 lowering.
        auto t4 = now_ns();
        auto back = lower_son_to_tier1(job);
        auto t5 = now_ns();
        std::println("Surge lowering (SoN→Tier1):            {} us", elapsed_ns(t4,t5)/1000);

        // asmjit emission.
        static thread_local std::unique_ptr<Tier1Compiler> compiler;
        if (!compiler) compiler = std::make_unique<Tier1Compiler>();
        auto t6 = now_ns();
        compiler->compile(*back);
        auto t7 = now_ns();
        std::println("Surge asmjit emission:                 {} us", elapsed_ns(t6,t7)/1000);

        std::println("Surge total compile:                   {} us", elapsed_ns(t0,t7)/1000);
    }

    // --- 5. Jolt compile breakdown ---
    {
        Chunk c = make_arith_chunk();

        // Chunk → Tier-1 lowering.
        auto t0 = now_ns();
        auto fn = lower_chunk_to_tier1(c, "bench");
        auto t1 = now_ns();
        std::println("\nJolt lowering (Chunk→Tier1):           {} us", elapsed_ns(t0,t1)/1000);

        // asmjit emission.
        static thread_local std::unique_ptr<Tier1Compiler> compiler;
        if (!compiler) compiler = std::make_unique<Tier1Compiler>();
        auto t2 = now_ns();
        compiler->compile(*fn);
        auto t3 = now_ns();
        std::println("Jolt asmjit emission:                  {} us", elapsed_ns(t2,t3)/1000);

        std::println("Jolt total compile:                    {} us", elapsed_ns(t0,t3)/1000);
    }

    // --- 6. Compiled code comparison ---
    {
        Chunk c = make_arith_chunk();
        auto jolt_e = *rt.compile_tier1(c);
        auto surge_e = *rt.compile_tier2(c);

        std::vector<int64_t> locals(1, 0);
        jolt_e(locals.data()); // warmup
        surge_e(locals.data());

        auto t0 = now_ns();
        for (int i = 0; i < N; ++i) jolt_e(locals.data());
        auto t1 = now_ns();
        std::println("\nJolt  execution (arith):               {} ns/op", elapsed_ns(t0,t1)/N);

        auto t2 = now_ns();
        for (int i = 0; i < N; ++i) surge_e(locals.data());
        auto t3 = now_ns();
        std::println("Surge execution (arith):               {} ns/op", elapsed_ns(t2,t3)/N);
    }

    return 0;
}
