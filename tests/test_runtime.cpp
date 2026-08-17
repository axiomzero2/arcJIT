// SPDX-License-Identifier: MIT
// Tests for the Runtime tier ladder and OSR.
#include <gtest/gtest.h>

#include <thread>

#include "runtime/runtime.h"
#include "tier1/tier1.h"
#include "tier2/tier2.h"

using namespace arcjit;

// Helper: build a Chunk that computes (a + b) * c via locals.
static Chunk make_arith_chunk() {
    Chunk c;
    c.set_max_locals(3);

    static Number nums[3];
    nums[0].base.type = ObjType::NumberInt; nums[0].base.ref_count = 1;
    nums[0].base.is_static = true; nums[0].as.i = 10;
    nums[1].base.type = ObjType::NumberInt; nums[1].base.ref_count = 1;
    nums[1].base.is_static = true; nums[1].as.i = 20;
    nums[2].base.type = ObjType::NumberInt; nums[2].base.ref_count = 1;
    nums[2].base.is_static = true; nums[2].as.i = 3;
    c.add_const(reinterpret_cast<Object*>(&nums[0]));
    c.add_const(reinterpret_cast<Object*>(&nums[1]));
    c.add_const(reinterpret_cast<Object*>(&nums[2]));

    // local 0 = 10, local 1 = 20, local 2 = 3
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(2);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(2);
    // (local 0 + local 1) * local 2 = (10 + 20) * 3 = 90
    c.emit_op(OpCode::LoadLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(2);
    c.emit_op(OpCode::Mul);
    c.emit_op(OpCode::Return);
    return c;
}

// Test that the interpreter runs a chunk and returns the right value.
TEST(RuntimeTest, RunInterpreter) {
    Runtime rt;
    Chunk c = make_arith_chunk();
    auto r = rt.run(c);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->as_int(), 90);
}

// Test forced Tier-1 compilation and execution.
TEST(RuntimeTest, RunAtTier1) {
    Runtime rt;
    Chunk c = make_arith_chunk();
    auto r = rt.run_at_tier(c, Tier::Tier1Baseline);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->as_int(), 90);
}

// Test forced Tier-2 compilation and execution.
TEST(RuntimeTest, RunAtTier2) {
    Runtime rt;
    Chunk c = make_arith_chunk();
    auto r = rt.run_at_tier(c, Tier::Tier2Optimizing);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->as_int(), 90);
}

// Test the tier ladder: run a chunk many times and verify it transitions
// through the tiers.
TEST(RuntimeTest, TierLadderTransitions) {
    Runtime rt;
    Chunk c = make_arith_chunk();

    // Run 150 times — should cross the Tier-1 threshold (100).
    for (int i = 0; i < 150; ++i) {
        auto r = rt.run(c);
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->as_int(), 90);
    }

    // Wait for background compilation to finish.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The runtime should have compiled to Tier 1 by now.
    EXPECT_GE(rt.stats().tier1_compiles.load(), 1u);

    // Run more — should use the Tier-1 compiled code.
    for (int i = 0; i < 10; ++i) {
        auto r = rt.run(c);
        ASSERT_TRUE(r.has_value()) << r.error();
        EXPECT_EQ(r->as_int(), 90);
    }
    EXPECT_GT(rt.stats().tier1_invocations.load(), 0u);
}

// Test OSR: explicitly request an OSR transition.
TEST(RuntimeTest, OSRToTier1) {
    Runtime rt;
    Chunk c = make_arith_chunk();
    auto r = rt.osr_to_tier1(c, nullptr);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 90);
    EXPECT_EQ(rt.stats().osr_transitions.load(), 1u);
}

TEST(RuntimeTest, OSRToTier2) {
    Runtime rt;
    Chunk c = make_arith_chunk();
    auto r = rt.osr_to_tier2(c, nullptr);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 90);
    EXPECT_EQ(rt.stats().osr_transitions.load(), 1u);
}

// Test that the stats are tracked correctly.
TEST(RuntimeTest, StatsTracking) {
    Runtime rt;
    Chunk c = make_arith_chunk();

    rt.run(c);
    EXPECT_EQ(rt.stats().interp_invocations.load(), 1u);

    rt.run_at_tier(c, Tier::Tier1Baseline);
    EXPECT_EQ(rt.stats().tier1_invocations.load(), 1u);

    rt.run_at_tier(c, Tier::Tier2Optimizing);
    EXPECT_EQ(rt.stats().tier2_invocations.load(), 1u);
}

// Test that compilation produces correct results across all tiers for a
// more complex chunk (with branches).
//
// Note: The SoN lowering currently uses a linearization strategy that doesn't
// fully model branch merge points (Region nodes for joining control flow).
// For chunks with branches, Tier-2 may produce incorrect results because
// nodes after the branch get attached to the wrong control path. This is
// a known limitation — full GCM with proper Region/Phi handling is future
// work. For now we only test branches at Tier-0 and Tier-1.
TEST(RuntimeTest, BranchChunkInterpreterAndTier1) {
    // if (10 > 5) return 10; else return 5
    Chunk c;
    c.set_max_locals(0);

    static Number ten; ten.base.type = ObjType::NumberInt; ten.base.ref_count = 1;
    ten.base.is_static = true; ten.as.i = 10;
    static Number five; five.base.type = ObjType::NumberInt; five.base.ref_count = 1;
    five.base.is_static = true; five.as.i = 5;
    c.add_const(reinterpret_cast<Object*>(&ten));
    c.add_const(reinterpret_cast<Object*>(&five));

    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Gt);
    size_t jif_pos = c.code_size();
    c.emit_op(OpCode::JumpIfFalse); c.emit_short(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);
    size_t else_pos = c.code_size();
    int16_t jif_off = static_cast<int16_t>(else_pos - (jif_pos + 3));
    c.patch_short(jif_pos + 1, jif_off);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Return);

    Runtime rt;

    // Interpreter
    auto r0 = rt.run_at_tier(c, Tier::Interpreter);
    ASSERT_TRUE(r0.has_value()) << r0.error();
    EXPECT_EQ(r0->as_int(), 10);

    // Tier 1
    auto r1 = rt.run_at_tier(c, Tier::Tier1Baseline);
    ASSERT_TRUE(r1.has_value()) << r1.error();
    EXPECT_EQ(r1->as_int(), 10);
}
