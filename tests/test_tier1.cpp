// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "tier1/tier1.h"

using namespace arcjit;

TEST(Tier1Test, DemoAdd3Shape) {
    Tier1Function fn = make_demo_add3();
    EXPECT_EQ(fn.vreg_count, 6u);
    // 3 LoadConst + 2 Add + 1 Return = 6 instructions
    EXPECT_EQ(fn.insts.size(), 6u);
    EXPECT_EQ(fn.insts[0].op, Tier1Op::LoadConst);
    EXPECT_EQ(fn.insts[3].op, Tier1Op::Add);
    EXPECT_EQ(fn.insts[5].op, Tier1Op::Return);
}

TEST(Tier1Test, LinearScan) {
    Tier1Function fn = make_demo_add3();
    RegAllocResult ra = linear_scan(fn);
    EXPECT_EQ(ra.intervals.size(), 5u);  // v1..v5
    // All five vregs should have either a physical reg or a stack slot.
    int assigned = 0, spilled = 0;
    for (uint32_t v = 1; v <= 5; ++v) {
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
    using EntryFn = int64_t (*)();
    auto f = reinterpret_cast<EntryFn>(*maybe_entry);
    EXPECT_EQ(f(), 6);  // 1 + 2 + 3 = 6
}
