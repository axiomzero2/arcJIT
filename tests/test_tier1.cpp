// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "tier1/tier1.h"

using namespace arcjit;

TEST(Tier1Test, DemoAdd3Shape) {
    Tier1Function fn = make_demo_add3();
    EXPECT_EQ(fn.vreg_count, 5u);
    // 3 LoadConstImm + 2 Add + 1 Return = 6 instructions
    EXPECT_EQ(fn.insts.size(), 6u);
    EXPECT_EQ(fn.insts[0].op, Tier1Op::LoadConstImm);
    EXPECT_EQ(fn.insts[3].op, Tier1Op::Add);
    EXPECT_EQ(fn.insts[5].op, Tier1Op::Return);
}

TEST(Tier1Test, LinearScan) {
    Tier1Function fn = make_demo_add3();
    RegAllocResult ra = linear_scan(fn);
    EXPECT_EQ(ra.intervals.size(), 5u);  // v1..v5
    // All five vregs should have either a physical reg or a stack slot.
    int assigned = 0, spilled = 0;
    for (uint32_t v = 1; v <= fn.vreg_count; ++v) {
        if (ra.vreg_to_phys[v] >= 0)      assigned++;
        else if (ra.vreg_to_stack[v] >= 0) spilled++;
    }
    EXPECT_EQ(assigned + spilled, 5);
}

TEST(Tier1Test, CompileAndRun) {
    Tier1Function fn = make_demo_add3();
    Tier1Compiler compiler;
    auto maybe_entry = compiler.compile(fn);
    ASSERT_TRUE(maybe_entry.has_value()) << maybe_entry.error();
    auto f = *maybe_entry;
    EXPECT_EQ(f(nullptr), 6);  // 1 + 2 + 3 = 6
}

// Test the chunk lowering: synthesize a Chunk that pushes 1, 2, 3 and adds
// them, then lower to Tier1Function and verify the IR shape.
TEST(Tier1Test, LowerChunkSimple) {
    Chunk c;
    c.set_max_locals(0);

    // We need Number constants 1, 2, 3.
    static Number ones[3];
    ones[0].base.type = ObjType::NumberInt;
    ones[0].base.ref_count = 1;
    ones[0].base.is_static = true;
    ones[0].as.i = 1;
    ones[1].base.type = ObjType::NumberInt;
    ones[1].base.ref_count = 1;
    ones[1].base.is_static = true;
    ones[1].as.i = 2;
    ones[2].base.type = ObjType::NumberInt;
    ones[2].base.ref_count = 1;
    ones[2].base.is_static = true;
    ones[2].as.i = 3;
    c.add_const(reinterpret_cast<Object*>(&ones[0]));
    c.add_const(reinterpret_cast<Object*>(&ones[1]));
    c.add_const(reinterpret_cast<Object*>(&ones[2]));

    // LoadConst 0 (push 1); LoadConst 1 (push 2); Add; LoadConst 2 (push 3); Add; Return.
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(2);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);

    auto maybe_fn = lower_chunk_to_tier1(c, "test_add3");
    ASSERT_TRUE(maybe_fn.has_value()) << maybe_fn.error();
    const Tier1Function& fn = *maybe_fn;

    // Expected shape:
    //   LoadConstImm v1, 1
    //   LoadConstImm v2, 2
    //   Add v3, v1, v2
    //   LoadConstImm v4, 3
    //   Add v5, v3, v4
    //   Return v5
    EXPECT_EQ(fn.vreg_count, 5u);
    EXPECT_GE(fn.insts.size(), 6u);
    EXPECT_EQ(fn.insts[0].op, Tier1Op::LoadConstImm);
    EXPECT_EQ(fn.insts[0].payload, 1u);
    EXPECT_EQ(fn.insts[1].op, Tier1Op::LoadConstImm);
    EXPECT_EQ(fn.insts[1].payload, 2u);
    EXPECT_EQ(fn.insts[2].op, Tier1Op::Add);
    EXPECT_EQ(fn.insts[2].src1, 1u);
    EXPECT_EQ(fn.insts[2].src2, 2u);
    EXPECT_EQ(fn.insts[3].op, Tier1Op::LoadConstImm);
    EXPECT_EQ(fn.insts[3].payload, 3u);
    EXPECT_EQ(fn.insts[4].op, Tier1Op::Add);
    EXPECT_EQ(fn.insts[4].src1, 3u);
    EXPECT_EQ(fn.insts[4].src2, 4u);
    EXPECT_EQ(fn.insts[5].op, Tier1Op::Return);
    EXPECT_EQ(fn.insts[5].src1, 5u);

    // Compile and run.
    Tier1Compiler compiler;
    auto maybe_entry = compiler.compile(fn);
    ASSERT_TRUE(maybe_entry.has_value()) << maybe_entry.error();
    auto f = *maybe_entry;
    EXPECT_EQ(f(nullptr), 6);
}

// Test lowering of a chunk with a conditional branch.
TEST(Tier1Test, LowerChunkBranch) {
    Chunk c;
    c.set_max_locals(0);

    static Number ten; ten.base.type = ObjType::NumberInt; ten.base.ref_count = 1;
    ten.base.is_static = true; ten.as.i = 10;
    static Number five; five.base.type = ObjType::NumberInt; five.base.ref_count = 1;
    five.base.is_static = true; five.as.i = 5;
    static Number zero; zero.base.type = ObjType::NumberInt; zero.base.ref_count = 1;
    zero.base.is_static = true; zero.as.i = 0;
    c.add_const(reinterpret_cast<Object*>(&ten));
    c.add_const(reinterpret_cast<Object*>(&five));
    c.add_const(reinterpret_cast<Object*>(&zero));

    // if (10 > 5) return 10; else return 5
    // Bytecode:
    //   LoadConst 0   ; push 10
    //   LoadConst 1   ; push 5
    //   Gt            ; → 1
    //   JumpIfFalse +N ; if false, skip to else
    //   LoadConst 0   ; push 10
    //   Return        ; return 10
    //   <else>:
    //   LoadConst 1   ; push 5
    //   Return        ; return 5
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Gt);
    // JumpIfFalse skips over the LoadConst+Return (4 bytes: opcode + 2-byte offset = 3, but
    // LoadConst is opcode+3 = 4, Return is 1, total 5). Actually offset is computed
    // from after the short, so let's compute: ip after short is at byte 14 (0-indexed:
    // LC0(4) + LC1(4) + Gt(1) + JIF(3) = 12, plus 2 bytes for short = 14).
    // We want to land at the second LoadConst. Let me just emit and patch.
    size_t jif_pos = c.code_size();
    c.emit_op(OpCode::JumpIfFalse); c.emit_short(0);  // placeholder
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);
    // Patch the JumpIfFalse offset.
    size_t else_pos = c.code_size();
    int16_t jif_off = static_cast<int16_t>(else_pos - (jif_pos + 3));
    // The 2 offset bytes are at jif_pos+1, jif_pos+2 (after the opcode byte).
    c.patch_short(jif_pos + 1, jif_off);

    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Return);

    auto maybe_fn = lower_chunk_to_tier1(c, "test_branch");
    ASSERT_TRUE(maybe_fn.has_value()) << maybe_fn.error();
    const Tier1Function& fn = *maybe_fn;

    // We expect at least one BranchIfFalse and at least one Label.
    bool has_branch = false, has_label = false;
    for (const auto& i : fn.insts) {
        if (i.op == Tier1Op::BranchIfFalse) has_branch = true;
        if (i.op == Tier1Op::Label) has_label = true;
    }
    EXPECT_TRUE(has_branch);
    EXPECT_TRUE(has_label);
}
