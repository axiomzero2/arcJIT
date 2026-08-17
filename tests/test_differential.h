// SPDX-License-Identifier: MIT
// arcJIT — Differential testing framework (Rule 38).
//
// Runs the same Chunk through all three tiers and asserts identical results.
// Divergence indicates a miscompilation.
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "bytecode/chunk.h"
#include "bytecode/value.h"
#include "runtime/runtime.h"

namespace arcjit {

struct DiffTestResult {
    bool     passed = true;
    Value    interp_result;
    Value    tier1_result;
    Value    tier2_result;
    std::string failure_message;

    // For each tier, did the run succeed?
    bool interp_ok = false;
    bool tier1_ok  = false;
    bool tier2_ok  = false;
};

// Run a chunk through all three tiers and compare results.
//
// `expected` is the value the chunk should produce (optional — if not set,
// we just check that all three tiers agree).
[[nodiscard]] DiffTestResult diff_test_chunk(Runtime& rt, const Chunk& chunk,
                                              std::optional<int64_t> expected = std::nullopt);

// Helper: build a chunk from a simple arithmetic spec and run diff test.
// E.g. "1+2+3" → LoadConst 1; LoadConst 2; Add; LoadConst 3; Add; Return.
[[nodiscard]] DiffTestResult diff_test_arith(Runtime& rt, std::string_view spec,
                                              int64_t expected);

// Run a batch of differential tests. Returns count of failures.
[[nodiscard]] size_t diff_test_batch(Runtime& rt, const std::vector<std::pair<std::string_view, int64_t>>& cases);

}  // namespace arcjit
