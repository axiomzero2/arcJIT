// SPDX-License-Identifier: MIT
// Tests for the type-safe bitmask utilities.
#include <gtest/gtest.h>

#include "core/bitmask.h"
#include "core/node.h"

using namespace arcjit;

// === Flags<E> ===

enum class TestFlags : uint32_t {
    A = 1u << 0,
    B = 1u << 1,
    C = 1u << 2,
    D = 1u << 3,
};
using TestFlagSet = Flags<TestFlags>;

TEST(FlagsTest, default_construct_is_empty) {
    TestFlagSet f;
    EXPECT_TRUE(f.none());
    EXPECT_FALSE(f);
}

TEST(FlagsTest, construct_from_single_flag) {
    TestFlagSet f = TestFlags::A;
    EXPECT_TRUE(f.has(TestFlags::A));
    EXPECT_FALSE(f.has(TestFlags::B));
}

TEST(FlagsTest, or_combines_flags) {
    TestFlagSet f = TestFlags::A | TestFlags::B;
    EXPECT_TRUE(f.has(TestFlags::A));
    EXPECT_TRUE(f.has(TestFlags::B));
    EXPECT_FALSE(f.has(TestFlags::C));
}

TEST(FlagsTest, any_checks_any_flag_in_mask) {
    TestFlagSet f = TestFlags::A | TestFlags::C;
    EXPECT_TRUE(f.any(TestFlags::A | TestFlags::B));   // A is set
    EXPECT_TRUE(f.any(TestFlags::B | TestFlags::C));   // C is set
    EXPECT_FALSE(f.any(TestFlags::B | TestFlags::D));   // neither set
}

TEST(FlagsTest, all_checks_all_flags_in_mask) {
    TestFlagSet f = TestFlags::A | TestFlags::B | TestFlags::C;
    EXPECT_TRUE(f.all(TestFlags::A | TestFlags::B));
    EXPECT_FALSE(f.all(TestFlags::A | TestFlags::D));
}

TEST(FlagsTest, or_equals_adds_flags) {
    TestFlagSet f = TestFlags::A;
    f |= TestFlags::B;
    EXPECT_TRUE(f.has(TestFlags::A));
    EXPECT_TRUE(f.has(TestFlags::B));
}

TEST(FlagsTest, and_equals_removes_flags) {
    TestFlagSet f = TestFlags::A | TestFlags::B | TestFlags::C;
    f &= (TestFlags::A | TestFlags::B);
    EXPECT_TRUE(f.has(TestFlags::A));
    EXPECT_TRUE(f.has(TestFlags::B));
    EXPECT_FALSE(f.has(TestFlags::C));
}

TEST(FlagsTest, xor_toggles_flags) {
    TestFlagSet f = TestFlags::A | TestFlags::B;
    f ^= TestFlags::B;
    EXPECT_TRUE(f.has(TestFlags::A));
    EXPECT_FALSE(f.has(TestFlags::B));
}

TEST(FlagsTest, complement) {
    TestFlagSet f = TestFlags::A;
    TestFlagSet not_a = ~f;
    EXPECT_FALSE(not_a.has(TestFlags::A));
    EXPECT_TRUE(not_a.has(TestFlags::B));
}

TEST(FlagsTest, raw_access) {
    TestFlagSet f = TestFlags::A | TestFlags::C;
    EXPECT_EQ(f.raw(), 5u);  // 0b0101
}

TEST(FlagsTest, equality) {
    TestFlagSet a = TestFlags::A | TestFlags::B;
    TestFlagSet b = TestFlags::B | TestFlags::A;
    EXPECT_EQ(a, b);
    TestFlagSet c = TestFlags::A;
    EXPECT_NE(a, c);
}

TEST(FlagsTest, explicit_bool) {
    TestFlagSet empty;
    TestFlagSet nonempty = TestFlags::A;
    EXPECT_FALSE(static_cast<bool>(empty));
    EXPECT_TRUE(static_cast<bool>(nonempty));
}

// === NodeBitSet ===

TEST(NodeBitSetTest, set_and_test) {
    NodeBitSet bs;
    bs.set(5);
    EXPECT_TRUE(bs.test(5));
    EXPECT_FALSE(bs.test(4));
    EXPECT_FALSE(bs.test(6));
}

TEST(NodeBitSetTest, clear) {
    NodeBitSet bs;
    bs.set(10);
    EXPECT_TRUE(bs.test(10));
    bs.clear(10);
    EXPECT_FALSE(bs.test(10));
}

TEST(NodeBitSetTest, clear_all) {
    NodeBitSet bs;
    bs.set(1);
    bs.set(2);
    bs.set(3);
    bs.clear_all();
    EXPECT_FALSE(bs.test(1));
    EXPECT_FALSE(bs.test(2));
    EXPECT_FALSE(bs.test(3));
}

TEST(NodeBitSetTest, popcount) {
    NodeBitSet bs;
    bs.set(0);
    bs.set(63);
    bs.set(64);
    bs.set(127);
    EXPECT_EQ(bs.popcount(), 4u);
}

TEST(NodeBitSetTest, resize) {
    NodeBitSet bs(100);
    bs.set(99);
    EXPECT_TRUE(bs.test(99));
    EXPECT_FALSE(bs.test(100));
}

TEST(NodeBitSetTest, set_union) {
    NodeBitSet a, b;
    a.set(1);
    a.set(3);
    b.set(2);
    b.set(3);
    a |= b;
    EXPECT_TRUE(a.test(1));
    EXPECT_TRUE(a.test(2));
    EXPECT_TRUE(a.test(3));
}

TEST(NodeBitSetTest, set_intersection) {
    NodeBitSet a, b;
    a.set(1);
    a.set(2);
    a.set(3);
    b.set(2);
    b.set(3);
    b.set(4);
    a &= b;
    EXPECT_FALSE(a.test(1));
    EXPECT_TRUE(a.test(2));
    EXPECT_TRUE(a.test(3));
    EXPECT_FALSE(a.test(4));
}

TEST(NodeBitSetTest, empty_check) {
    NodeBitSet bs;
    EXPECT_TRUE(bs.empty());
    bs.set(5);
    EXPECT_FALSE(bs.empty());
    bs.clear(5);
    EXPECT_TRUE(bs.empty());
}

TEST(NodeBitSetTest, large_indices) {
    NodeBitSet bs;
    bs.set(10000);
    EXPECT_TRUE(bs.test(10000));
    EXPECT_FALSE(bs.test(9999));
}

// === AnalysisInvalidSet ===

TEST(AnalysisInvalidSetTest, combine_and_check) {
    AnalysisInvalidSet invalidated = AnalysisKind::DominatorTree | AnalysisKind::LoopTree;
    EXPECT_TRUE(invalidated.has(AnalysisKind::DominatorTree));
    EXPECT_TRUE(invalidated.has(AnalysisKind::LoopTree));
    EXPECT_FALSE(invalidated.has(AnalysisKind::TypeInference));
}

// === CompileOptions ===

TEST(CompileOptionsTest, default_gigavolt_options) {
    EXPECT_TRUE(kDefaultGigavoltOptions.has(CompileOption::EnablePEA));
    EXPECT_TRUE(kDefaultGigavoltOptions.has(CompileOption::EnableInlining));
    EXPECT_TRUE(kDefaultGigavoltOptions.has(CompileOption::VerifyGraph));
    EXPECT_FALSE(kDefaultGigavoltOptions.has(CompileOption::EnableVectorization));
}

TEST(CompileOptionsTest, combine_custom) {
    CompileOptionSet opts = CompileOption::EnableFastMath | CompileOption::EnableVectorization;
    EXPECT_TRUE(opts.has(CompileOption::EnableFastMath));
    EXPECT_TRUE(opts.has(CompileOption::EnableVectorization));
    EXPECT_FALSE(opts.has(CompileOption::EnablePEA));
}

// === Symbolic printing ===

TEST(SymbolicPrintTest, format_node_flags) {
    uint32_t raw = static_cast<uint32_t>(NodeFlags::Pure) |
                   static_cast<uint32_t>(NodeFlags::GVNable) |
                   static_cast<uint32_t>(NodeFlags::Commutative);
    std::string s = format_node_flags(raw);
    EXPECT_NE(s.find("Pure"), std::string::npos);
    EXPECT_NE(s.find("GVNable"), std::string::npos);
    EXPECT_NE(s.find("Commutative"), std::string::npos);
    EXPECT_NE(s.find("|"), std::string::npos);
}

TEST(SymbolicPrintTest, format_node_flags_none) {
    EXPECT_EQ(format_node_flags(0), "(none)");
}

TEST(SymbolicPrintTest, format_analysis_invalid) {
    uint32_t raw = static_cast<uint32_t>(AnalysisKind::DominatorTree) |
                   static_cast<uint32_t>(AnalysisKind::LoopTree);
    std::string s = format_analysis_invalid(raw);
    EXPECT_NE(s.find("DominatorTree"), std::string::npos);
    EXPECT_NE(s.find("LoopTree"), std::string::npos);
}

TEST(SymbolicPrintTest, format_compile_options) {
    uint64_t raw = static_cast<uint64_t>(CompileOption::EnableInlining) |
                   static_cast<uint64_t>(CompileOption::VerifyGraph);
    std::string s = format_compile_options(raw);
    EXPECT_NE(s.find("EnableInlining"), std::string::npos);
    EXPECT_NE(s.find("VerifyGraph"), std::string::npos);
}
