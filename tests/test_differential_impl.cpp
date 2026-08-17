// SPDX-License-Identifier: MIT
// Differential testing framework implementation.
#include "test_differential.h"

#include <memory>
#include <print>

namespace arcjit {

DiffTestResult diff_test_chunk(Runtime& rt, const Chunk& chunk,
                                std::optional<int64_t> expected) {
    DiffTestResult r;

    // Run at Tier 0 (interpreter).
    auto ri = rt.run_at_tier(chunk, Tier::Interpreter);
    if (ri) {
        r.interp_ok = true;
        r.interp_result = *ri;
    } else {
        r.failure_message += "interpreter failed: " + ri.error() + "\n";
    }

    // Run at Tier 1.
    auto r1 = rt.run_at_tier(chunk, Tier::Tier1Baseline);
    if (r1) {
        r.tier1_ok = true;
        r.tier1_result = *r1;
    } else {
        r.failure_message += "tier1 failed: " + r1.error() + "\n";
    }

    // Run at Tier 2.
    auto r2 = rt.run_at_tier(chunk, Tier::Tier2Optimizing);
    if (r2) {
        r.tier2_ok = true;
        r.tier2_result = *r2;
    } else {
        r.failure_message += "tier2 failed: " + r2.error() + "\n";
    }

    // Compare results.
    if (r.interp_ok && r.tier1_ok) {
        if (r.interp_result.as_int() != r.tier1_result.as_int()) {
            r.passed = false;
            r.failure_message += "interp != tier1: " +
                std::to_string(r.interp_result.as_int()) + " vs " +
                std::to_string(r.tier1_result.as_int()) + "\n";
        }
    }
    if (r.interp_ok && r.tier2_ok) {
        if (r.interp_result.as_int() != r.tier2_result.as_int()) {
            r.passed = false;
            r.failure_message += "interp != tier2: " +
                std::to_string(r.interp_result.as_int()) + " vs " +
                std::to_string(r.tier2_result.as_int()) + "\n";
        }
    }

    // Check expected value.
    if (expected && r.interp_ok) {
        if (r.interp_result.as_int() != *expected) {
            r.passed = false;
            r.failure_message += "interp result != expected: " +
                std::to_string(r.interp_result.as_int()) + " vs " +
                std::to_string(*expected) + "\n";
        }
    }

    return r;
}

// Helper to build a chunk from a simple arithmetic spec.
static Chunk make_arith_chunk_from_spec(std::string_view spec) {
    Chunk c;
    c.set_max_locals(0);

    // Parse numbers and operators from the spec.
    std::vector<int64_t> nums;
    std::vector<char>    ops;
    size_t i = 0;
    while (i < spec.size()) {
        if (spec[i] == ' ') { ++i; continue; }
        if (spec[i] >= '0' && spec[i] <= '9') {
            int64_t n = 0;
            while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
                n = n * 10 + (spec[i] - '0');
                ++i;
            }
            nums.push_back(n);
        } else if (spec[i] == '+' || spec[i] == '-' || spec[i] == '*' || spec[i] == '/') {
            ops.push_back(spec[i]);
            ++i;
        } else {
            ++i;  // skip unknown chars
        }
    }

    // Allocate Number constants (leak intentionally — process lifetime).
    static std::vector<std::unique_ptr<Number>> pool;
    auto op_it = ops.begin();
    bool first = true;
    for (int64_t n : nums) {
        auto num = std::make_unique<Number>();
        num->base.type = ObjType::NumberInt;
        num->base.ref_count = 1;
        num->base.is_static = true;
        num->as.i = n;
        uint32_t idx = c.add_const(reinterpret_cast<Object*>(num.get()));
        pool.push_back(std::move(num));

        c.emit_op(OpCode::LoadConst);
        c.emit_const_idx(idx);

        if (!first && op_it != ops.end()) {
            switch (*op_it) {
                case '+': c.emit_op(OpCode::Add); break;
                case '-': c.emit_op(OpCode::Sub); break;
                case '*': c.emit_op(OpCode::Mul); break;
                case '/': c.emit_op(OpCode::Div); break;
            }
            ++op_it;
        }
        first = false;
    }
    c.emit_op(OpCode::Return);
    return c;
}

DiffTestResult diff_test_arith(Runtime& rt, std::string_view spec, int64_t expected) {
    Chunk c = make_arith_chunk_from_spec(spec);
    return diff_test_chunk(rt, c, expected);
}

size_t diff_test_batch(Runtime& rt, const std::vector<std::pair<std::string_view, int64_t>>& cases) {
    size_t failures = 0;
    for (const auto& [spec, expected] : cases) {
        auto r = diff_test_arith(rt, spec, expected);
        if (!r.passed) {
            failures++;
            std::println(stderr, "DIFF FAIL: '{}' expected {} — {}",
                         spec, expected, r.failure_message);
        }
    }
    return failures;
}

}  // namespace arcjit
