// SPDX-License-Identifier: MIT
// arcJIT — Gigavolt pass effectiveness diagnostic.
//
// Runs the benchmark expression through Surge and reports:
//   1. How many nodes each Gigavolt pass actually changed
//   2. The Tier-1 IR before and after the SoN round-trip (diff)
//   3. The final graph size
//   4. asmjit assembly dump for both Jolt and Surge
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <vector>

#include "runtime/runtime.h"
#include "tier1/tier1.h"
#include "tier2/tier2.h"
#include "core/ir_dump.h"
#include "core/bitmask.h"
#include "passman/pass.h"
#include "passman/instrument.h"

using namespace arcjit;

static Object* make_num(int64_t v) {
    static std::vector<std::unique_ptr<Number>> pool;
    auto n = std::make_unique<Number>();
    n->base.type = ObjType::NumberInt; n->base.ref_count = 1;
    n->base.is_static = true; n->as.i = v;
    Object* p = reinterpret_cast<Object*>(n.get());
    pool.push_back(std::move(n));
    return p;
}

// Build the benchmark expression: (10+20)*3 + 7 - 42 = 55
static Chunk make_bench_chunk() {
    Chunk c;
    c.set_max_locals(3);
    c.add_const(make_num(10));
    c.add_const(make_num(20));
    c.add_const(make_num(3));
    c.add_const(make_num(7));
    c.add_const(make_num(42));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(2);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(2);
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

// A pure-arithmetic chunk with no locals: 1+2+3+4+5 = 15
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

// A loop-heavy chunk: sum 0..99 using a while loop
// Since we don't have while in bytecode directly, we simulate:
// local0 = 0 (sum)
// local1 = 0 (i)
// local2 = 100 (limit)
// loop_start:
//   if local1 >= local2 goto loop_end
//   local0 = local0 + local1
//   local1 = local1 + 1
//   goto loop_start
// loop_end:
//   return local0
static Chunk make_loop_chunk() {
    Chunk c;
    c.set_max_locals(3);
    c.add_const(make_num(0));    // 0: initial sum
    c.add_const(make_num(0));    // 1: initial i
    c.add_const(make_num(100));  // 2: limit
    c.add_const(make_num(1));    // 3: increment

    // local0 = 0, local1 = 0, local2 = 100
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(2);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(2);

    // loop_start: (offset 15)
    // if local1 >= local2, jump to loop_end
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);  // push i
    c.emit_op(OpCode::LoadLocal); c.emit_byte(2);  // push limit
    c.emit_op(OpCode::Gte);                          // i >= limit?
    // JumpIfFalse to loop_end — we need to compute the offset.
    // Current IP after the short = offset of JumpIfFalse + 3
    // loop_end is the Return instruction.
    // Let's emit a placeholder and patch.
    size_t jif_pos = c.code_size();
    c.emit_op(OpCode::JumpIfFalse); c.emit_short(0);  // placeholder

    // local0 = local0 + local1
    c.emit_op(OpCode::LoadLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);

    // local1 = local1 + 1
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(3);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);

    // Jump back to loop_start (offset 15)
    // Current IP = jif_pos + 3 + (body instructions)
    // We need to jump to offset 15.
    // Jump offset = target_ip - (current_ip_after_short)
    size_t jump_pos = c.code_size();
    int16_t back_offset = static_cast<int16_t>(15 - (jump_pos + 3));
    c.emit_op(OpCode::Jump); c.emit_short(back_offset);

    // loop_end:
    size_t loop_end_pos = c.code_size();
    // Patch the JumpIfFalse to jump here.
    int16_t jif_offset = static_cast<int16_t>(loop_end_pos - (jif_pos + 3));
    c.patch_short(jif_pos + 1, jif_offset);

    // return local0
    c.emit_op(OpCode::LoadLocal); c.emit_byte(0);
    c.emit_op(OpCode::Return);

    return c;
}

int main() {
    std::println("=== Gigavolt Pass Effectiveness Diagnostic ===\n");

    // Test 1: Benchmark expression with locals
    {
        Chunk c = make_bench_chunk();
        auto fn = lower_chunk_to_tier1(c, "bench_locals");
        std::println("--- Test 1: (10+20)*3 + 7 - 42 (with locals) ---");
        std::println("Tier-1 IR ({} insts, {} vregs):", fn->insts.size(), fn->vreg_count);
        for (size_t i = 0; i < fn->insts.size(); ++i) {
            std::println("  [{}] op={} dst={} src1={} src2={} payload={}",
                         i, (int)fn->insts[i].op, fn->insts[i].dst,
                         fn->insts[i].src1, fn->insts[i].src2,
                         (long)fn->insts[i].payload);
        }

        // Lower to SoN
        Tier2Job job;
        job.function_name = "bench_locals";
        lower_tier1_to_son(*fn, job);
        std::println("\nSoN graph (before Gigavolt): {} nodes", job.graph.size() - 1);

        // Run Gigavolt with instrumentation
        auto& instr = global_instrumentation();
        instr.clear();
        instr.set_enabled(true);

        // Build pipeline manually so we can run passes one at a time
        job.pipeline = build_gigavolt_pipeline();
        job.pipeline.run_to_fixpoint(job.graph, 8);

        // Print per-pass stats
        auto stats = instr.summarize();
        std::println("\nGigavolt pass stats:");
        for (const auto& s : stats) {
            std::println("  {:<20} created={:<4} deleted={:<4} changed={}",
                         s.name, s.nodes_created, s.nodes_deleted, s.nodes_changed);
        }

        std::println("\nSoN graph (after Gigavolt): {} nodes", job.graph.size() - 1);

        // Lower back
        auto back = lower_son_to_tier1(job);
        std::println("\nTier-1 IR after round-trip ({} insts, {} vregs):",
                     back->insts.size(), back->vreg_count);
        for (size_t i = 0; i < back->insts.size(); ++i) {
            std::println("  [{}] op={} dst={} src1={} src2={} payload={}",
                         i, (int)back->insts[i].op, back->insts[i].dst,
                         back->insts[i].src1, back->insts[i].src2,
                         (long)back->insts[i].payload);
        }
    }

    // Test 2: Pure arithmetic (no locals)
    {
        Chunk c = make_arith_chunk();
        auto fn = lower_chunk_to_tier1(c, "arith");
        std::println("\n--- Test 2: 1+2+3+4+5 (pure arithmetic, no locals) ---");
        std::println("Tier-1 IR ({} insts, {} vregs)", fn->insts.size(), fn->vreg_count);

        Tier2Job job;
        job.function_name = "arith";
        lower_tier1_to_son(*fn, job);
        std::println("SoN graph (before): {} nodes", job.graph.size() - 1);

        auto& instr = global_instrumentation();
        instr.clear();
        instr.set_enabled(true);
        job.pipeline = build_gigavolt_pipeline();
        job.pipeline.run_to_fixpoint(job.graph, 8);

        auto stats = instr.summarize();
        std::println("\nGigavolt pass stats:");
        for (const auto& s : stats) {
            std::println("  {:<20} created={:<4} deleted={:<4} changed={}",
                         s.name, s.nodes_created, s.nodes_deleted, s.nodes_changed);
        }

        std::println("SoN graph (after): {} nodes", job.graph.size() - 1);

        // Check if the result was constant-folded
        bool found_const = false;
        int64_t folded_value = 0;
        for (uint32_t i = 1; i < job.graph.size(); ++i) {
            const Node& n = job.graph.at(NodeId{i});
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;
            if (n.kind == NodeKind::ConstInt && n.use_count > 0) {
                found_const = true;
                folded_value = static_cast<int64_t>(n.payload);
            }
        }
        if (found_const) {
            std::println("Result: constant-folded to {}", folded_value);
        }

        // Print final graph
        std::println("\nFinal SoN graph:");
        std::println("{}", dump_graph_text(job.graph));
    }

    // Test 3: Loop chunk
    {
        Chunk c = make_loop_chunk();
        auto fn = lower_chunk_to_tier1(c, "loop");
        std::println("\n--- Test 3: sum(0..99) loop ---");
        std::println("Tier-1 IR ({} insts, {} vregs)", fn->insts.size(), fn->vreg_count);

        // Run at interpreter to verify correctness
        Runtime rt;
        auto ri = rt.run_at_tier(c, Tier::Interpreter);
        if (ri) {
            std::println("Interpreter result: {}", ri->as_int());
        }

        Tier2Job job;
        job.function_name = "loop";
        lower_tier1_to_son(*fn, job);
        std::println("SoN graph (before): {} nodes", job.graph.size() - 1);

        auto& instr = global_instrumentation();
        instr.clear();
        instr.set_enabled(true);
        job.pipeline = build_gigavolt_pipeline();
        job.pipeline.run_to_fixpoint(job.graph, 8);

        auto stats = instr.summarize();
        std::println("\nGigavolt pass stats:");
        for (const auto& s : stats) {
            std::println("  {:<20} created={:<4} deleted={:<4} changed={}",
                         s.name, s.nodes_created, s.nodes_deleted, s.nodes_changed);
        }

        std::println("SoN graph (after): {} nodes", job.graph.size() - 1);
    }

    // Test 4: Jolt compile time breakdown
    {
        Chunk c = make_bench_chunk();
        std::println("\n--- Test 4: Jolt compile time breakdown ---");

        // Lowering
        auto t0 = std::chrono::high_resolution_clock::now();
        auto fn = lower_chunk_to_tier1(c, "bench");
        auto t1 = std::chrono::high_resolution_clock::now();
        auto lower_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        // Compile (includes linear scan + asmjit)
        static thread_local std::unique_ptr<Tier1Compiler> compiler;
        if (!compiler) compiler = std::make_unique<Tier1Compiler>();
        auto t2 = std::chrono::high_resolution_clock::now();
        auto entry = compiler->compile(*fn);
        auto t3 = std::chrono::high_resolution_clock::now();
        auto compile_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

        std::println("  Lowering (Chunk→Tier1): {} us", lower_us);
        std::println("  Compile (LinearScan+asmjit): {} us", compile_us);
        std::println("  Total: {} us", lower_us + compile_us);

        if (!entry) {
            std::println(stderr, "  COMPILE FAILED: {}", entry.error());
        }
    }

    // Test 5: Surge compile time breakdown
    {
        Chunk c = make_bench_chunk();
        std::println("\n--- Test 5: Surge compile time breakdown ---");

        auto fn = lower_chunk_to_tier1(c, "bench");

        // Tier-1 → SoN
        auto t0 = std::chrono::high_resolution_clock::now();
        Tier2Job job;
        job.function_name = "bench";
        lower_tier1_to_son(*fn, job);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto lower_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        // Gigavolt
        auto t2 = std::chrono::high_resolution_clock::now();
        run_tier2_pipeline(job);
        auto t3 = std::chrono::high_resolution_clock::now();
        auto pipeline_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

        // SoN → Tier-1
        auto t4 = std::chrono::high_resolution_clock::now();
        auto back = lower_son_to_tier1(job);
        auto t5 = std::chrono::high_resolution_clock::now();
        auto back_us = std::chrono::duration_cast<std::chrono::microseconds>(t5 - t4).count();

        // asmjit
        static thread_local std::unique_ptr<Tier1Compiler> compiler;
        if (!compiler) compiler = std::make_unique<Tier1Compiler>();
        auto t6 = std::chrono::high_resolution_clock::now();
        compiler->compile(*back);
        auto t7 = std::chrono::high_resolution_clock::now();
        auto asmjit_us = std::chrono::duration_cast<std::chrono::microseconds>(t7 - t6).count();

        std::println("  Lowering (Tier1→SoN): {} us", lower_us);
        std::println("  Gigavolt pipeline:    {} us", pipeline_us);
        std::println("  Back-lowering (SoN→Tier1): {} us", back_us);
        std::println("  asmjit emission:      {} us", asmjit_us);
        std::println("  Total:                {} us", lower_us + pipeline_us + back_us + asmjit_us);
    }

    return 0;
}
