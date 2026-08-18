// SPDX-License-Identifier: MIT
// arcJIT — Proper benchmark that separates compile time from execution time.
//
// The existing --bench command calls run_at_tier() which recompiles every
// time. This benchmark compiles once, then runs the compiled code N times
// to measure pure execution speed.
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

using namespace arcjit;

// Build a chunk that computes a non-trivial expression.
static Chunk make_bench_chunk() {
    Chunk c;
    c.set_max_locals(3);
    static Number nums[5];
    nums[0].base.type = ObjType::NumberInt; nums[0].base.ref_count = 1;
    nums[0].base.is_static = true; nums[0].as.i = 10;
    nums[1].base.type = ObjType::NumberInt; nums[1].base.ref_count = 1;
    nums[1].base.is_static = true; nums[1].as.i = 20;
    nums[2].base.type = ObjType::NumberInt; nums[2].base.ref_count = 1;
    nums[2].base.is_static = true; nums[2].as.i = 3;
    nums[3].base.type = ObjType::NumberInt; nums[3].base.ref_count = 1;
    nums[3].base.is_static = true; nums[3].as.i = 7;
    nums[4].base.type = ObjType::NumberInt; nums[4].base.ref_count = 1;
    nums[4].base.is_static = true; nums[4].as.i = 42;

    c.add_const(reinterpret_cast<Object*>(&nums[0]));
    c.add_const(reinterpret_cast<Object*>(&nums[1]));
    c.add_const(reinterpret_cast<Object*>(&nums[2]));
    c.add_const(reinterpret_cast<Object*>(&nums[3]));
    c.add_const(reinterpret_cast<Object*>(&nums[4]));

    // local 0 = 10, local 1 = 20, local 2 = 3
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(2);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(2);

    // (local0 + local1) * local2 + 7 - 42
    // = (10 + 20) * 3 + 7 - 42 = 90 + 7 - 42 = 55
    c.emit_op(OpCode::LoadLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(2);
    c.emit_op(OpCode::Mul);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(3);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(4);
    c.emit_op(OpCode::Sub);
    c.emit_op(OpCode::Return);
    return c;
}

int main(int argc, char** argv) {
    size_t N = argc > 1 ? strtoull(argv[1], nullptr, 10) : 100000;
    Chunk c = make_bench_chunk();

    std::println("=== arcJIT Benchmark ===");
    std::println("Expression: (10+20)*3 + 7 - 42 = 55");
    std::println("Iterations: {}", N);
    std::println("");

    Runtime rt;
    rt.set_profiling_enabled(false);  // we're not driving a tier-up here

    // --- Spark (interpreter) ---
    {
        // Warmup.
        rt.run_at_tier(c, Tier::Interpreter);

        auto t0 = std::chrono::high_resolution_clock::now();
        int64_t result = 0;
        for (size_t i = 0; i < N; ++i) {
            auto r = rt.run_at_tier(c, Tier::Interpreter);
            result = r->as_int();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        std::println("Spark  (interpreter):  result={}  total={}ms  per_op={}ns",
                     result, ns / 1000000, ns / N);
    }

    // --- Jolt (Tier 1) ---
    // Compile once, then run the compiled entry N times.
    {
        // Compile.
        auto t0c = std::chrono::high_resolution_clock::now();
        auto maybe_entry = rt.compile_tier1(c);
        auto t1c = std::chrono::high_resolution_clock::now();
        if (!maybe_entry) {
            std::println(stderr, "Jolt compile failed: {}", maybe_entry.error());
            return 1;
        }
        auto entry = *maybe_entry;
        auto compile_us = std::chrono::duration_cast<std::chrono::microseconds>(t1c - t0c).count();

        // Prepare locals buffer.
        std::vector<int64_t> locals(std::max(c.max_locals(), 1), 0);

        // Warmup.
        entry(locals.data());

        // Execute N times.
        auto t0 = std::chrono::high_resolution_clock::now();
        int64_t result = 0;
        for (size_t i = 0; i < N; ++i) {
            result = entry(locals.data());
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        std::println("Jolt  (baseline JIT):  result={}  compile={}us  total={}ms  per_op={}ns",
                     result, compile_us, ns / 1000000, ns / N);
    }

    // --- Surge (Tier 2 / Gigavolt) ---
    // Compile once, then run the compiled entry N times.
    {
        // Compile.
        auto t0c = std::chrono::high_resolution_clock::now();
        auto maybe_entry = rt.compile_tier2(c);
        auto t1c = std::chrono::high_resolution_clock::now();
        if (!maybe_entry) {
            std::println(stderr, "Surge compile failed: {}", maybe_entry.error());
            return 1;
        }
        auto entry = *maybe_entry;
        auto compile_us = std::chrono::duration_cast<std::chrono::microseconds>(t1c - t0c).count();

        // Prepare locals buffer.
        std::vector<int64_t> locals(std::max(c.max_locals(), 1), 0);

        // Warmup.
        entry(locals.data());

        // Execute N times.
        auto t0 = std::chrono::high_resolution_clock::now();
        int64_t result = 0;
        for (size_t i = 0; i < N; ++i) {
            result = entry(locals.data());
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        std::println("Surge (Gigavolt JIT):  result={}  compile={}us  total={}ms  per_op={}ns",
                     result, compile_us, ns / 1000000, ns / N);
    }

    std::println("");
    std::println("=== Machinery State ===");
    std::println("{}", rt.dump_machinery());

    return 0;
}
