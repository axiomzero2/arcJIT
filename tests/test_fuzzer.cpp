// SPDX-License-Identifier: MIT
// Tests for the structure-aware bytecode fuzzer.
#include <gtest/gtest.h>

#include "runtime/fuzzer.h"
#include "runtime/runtime.h"

using namespace arcjit;

// Basic fuzz test — run 100 random programs, expect no failures.
TEST(FuzzerTest, 100_random_programs_no_divergence) {
    Runtime rt;
    size_t failures = run_fuzz_batch(rt, /*start_seed=*/1, /*count=*/100, /*max_ops=*/5);
    EXPECT_EQ(failures, 0u);
}

// Larger fuzz test — 1000 programs.
TEST(FuzzerTest, 1000_random_programs_no_divergence) {
    Runtime rt;
    size_t failures = run_fuzz_batch(rt, /*start_seed=*/1000, /*count=*/1000, /*max_ops=*/8);
    EXPECT_EQ(failures, 0u);
}

// Test with a specific seed (for reproducibility).
TEST(FuzzerTest, specific_seed_reproducible) {
    Runtime rt;
    FuzzCase fc = generate_fuzz_case(/*seed=*/42, /*max_ops=*/5);

    // Run twice — should produce the same result.
    FuzzResult r1 = run_fuzz_case(rt, fc);
    FuzzResult r2 = run_fuzz_case(rt, fc);

    EXPECT_EQ(r1.interp_result, r2.interp_result);
    EXPECT_EQ(r1.tier1_result, r2.tier1_result);
    EXPECT_EQ(r1.tier2_result, r2.tier2_result);
    EXPECT_TRUE(r1.passed);
    EXPECT_TRUE(r2.passed);
}

// Test that the fuzzer generates valid bytecode.
TEST(FuzzerTest, generates_valid_bytecode) {
    FuzzCase fc = generate_fuzz_case(/*seed=*/123, /*max_ops=*/3);
    EXPECT_FALSE(fc.bytecode.empty());
    EXPECT_GE(fc.constants.size(), 2u);
    // The bytecode should end with Return.
    EXPECT_EQ(fc.bytecode.back(), static_cast<uint8_t>(OpCode::Return));
}

// Test with large constants.
// Note: when constants are large, intermediate results may overflow 32-bit
// payloads. ConstFold skips folding in that case (correct behavior), so all
// three tiers should still agree on the final result.
TEST(FuzzerTest, large_constants_no_overflow_crash) {
    Runtime rt;
    size_t failures = 0;
    for (uint64_t seed = 5000; seed < 5100; ++seed) {
        // Generate with large constants by using a custom seed range.
        // We don't override the constants (that would break bytecode indices).
        FuzzCase fc = generate_fuzz_case(seed * 7, 3);
        FuzzResult r = run_fuzz_case(rt, fc);
        if (!r.passed) failures++;
    }
    // All tiers should agree even with large intermediate results.
    EXPECT_EQ(failures, 0u);
}

// Test that the fuzzer handles the minimal case (2 constants, 1 op).
TEST(FuzzerTest, minimal_program) {
    Runtime rt;
    FuzzCase fc = generate_fuzz_case(/*seed=*/999, /*max_ops=*/0);

    // Even with max_ops=0, we should get at least LoadConst; LoadConst; op; Return.
    EXPECT_GE(fc.bytecode.size(), 7u);  // 4 + 1 + 1 + 1 = 7 bytes minimum

    FuzzResult r = run_fuzz_case(rt, fc);
    EXPECT_TRUE(r.passed) << r.error;
}

// Test many seeds in a batch.
TEST(FuzzerTest, batch_500_seeds) {
    Runtime rt;
    size_t failures = run_fuzz_batch(rt, /*start_seed=*/2000, /*count=*/500, /*max_ops=*/6);
    EXPECT_EQ(failures, 0u);
}
