// SPDX-License-Identifier: MIT
// arcJIT — Structure-aware bytecode fuzzer.
//
// Generates syntactically valid Arc bytecode programs and runs them through
// all three tiers. Any divergence in results indicates a bug.
//
// The fuzzer uses a simple PRNG (xorshift64) for reproducibility. Each test
// case is generated from a seed; failing seeds are logged for replay.
//
// Usage:
//   arcjit-fuzz --iterations=10000           # run 10k random tests
//   arcjit-fuzz --seed=12345                 # run a specific seed
//   arcjit-fuzz --max-ops=20                 # limit program size
//
// The fuzzer generates programs of the form:
//   LoadConst N
//   LoadConst M
//   <binary op>
//   [LoadConst K, <binary op>]*
//   Return
//
// It only uses arithmetic ops (Add, Sub, Mul) to avoid division-by-zero
// and type errors. This is sufficient to catch miscompilations in the
// arithmetic fast paths.
#pragma once

#include <cstdint>
#include <vector>

#include "bytecode/chunk.h"
#include "runtime/runtime.h"

namespace arcjit {

// A single fuzz test case.
struct FuzzCase {
    uint64_t                seed;
    std::vector<uint8_t>    bytecode;
    std::vector<int64_t>    constants;
    int64_t                 expected;  // computed by the interpreter
};

// xorshift64 PRNG — fast, deterministic, good enough for fuzzing.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 0xDEADBEEFCAFEBABEULL) {}

    uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    int64_t next_int(int64_t min, int64_t max) {
        if (min >= max) return min;
        return min + static_cast<int64_t>(next() % static_cast<uint64_t>(max - min + 1));
    }

    bool next_bool() { return (next() & 1) != 0; }
};

// Generate a single fuzz test case from a seed.
// The generated program uses only safe arithmetic (Add, Sub, Mul) with
// small integers to avoid overflow surprises.
[[nodiscard]] FuzzCase generate_fuzz_case(uint64_t seed, size_t max_ops = 10);

// Run a single fuzz case through all three tiers.
// Returns true if all tiers agree, false if there's a divergence.
struct FuzzResult {
    bool     passed;
    int64_t  interp_result;
    int64_t  tier1_result;
    int64_t  tier2_result;
    std::string error;
};

[[nodiscard]] FuzzResult run_fuzz_case(Runtime& rt, const FuzzCase& fc);

// Run a batch of fuzz cases. Returns the number of failures.
// Each failure is logged to stderr with its seed for replay.
[[nodiscard]] size_t run_fuzz_batch(Runtime& rt, uint64_t start_seed, size_t count,
                                     size_t max_ops = 10);

}  // namespace arcjit
