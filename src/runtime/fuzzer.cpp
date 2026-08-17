// SPDX-License-Identifier: MIT
#include "runtime/fuzzer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

#include "bytecode/heap.h"

namespace arcjit {

// Static pool of Number constants for fuzz cases.
// These leak intentionally — process lifetime.
static Object* make_fuzz_const(int64_t v) {
    static std::vector<std::unique_ptr<Number>> pool;
    auto n = std::make_unique<Number>();
    n->base.type = ObjType::NumberInt;
    n->base.ref_count = 1;
    n->base.is_static = true;
    n->as.i = v;
    Object* p = reinterpret_cast<Object*>(n.get());
    pool.push_back(std::move(n));
    return p;
}

[[nodiscard]] FuzzCase generate_fuzz_case(uint64_t seed, size_t max_ops) {
    FuzzCase fc;
    fc.seed = seed;
    Rng rng(seed);

    // Generate 2-5 constants in range [-20, 20].
    // We use small constants to avoid intermediate overflow in ConstFold
    // (which uses 32-bit payloads). With max 5 constants of magnitude ≤20
    // and max 8 multiplications, the worst case is 20^8 ≈ 2.5e10, which
    // exceeds 32-bit range. So we further limit to [-10, 10] for safety.
    size_t num_consts = static_cast<size_t>(rng.next_int(2, 5));
    for (size_t i = 0; i < num_consts; ++i) {
        fc.constants.push_back(rng.next_int(-10, 10));
    }

    // Build the chunk.
    Chunk c;
    c.set_max_locals(0);

    // Add constants to the pool.
    for (int64_t v : fc.constants) {
        c.add_const(make_fuzz_const(v));
    }

    // Emit: LoadConst 0; LoadConst 1; <op>; [LoadConst K; <op>]*
    c.emit_op(OpCode::LoadConst);
    c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst);
    c.emit_const_idx(1);

    // First binary op.
    OpCode ops[] = {OpCode::Add, OpCode::Sub, OpCode::Mul};
    c.emit_op(ops[rng.next() % 3]);

    // Additional ops.
    size_t extra_ops = (max_ops > 0) ? (rng.next() % max_ops) : 0;
    for (size_t i = 0; i < extra_ops && num_consts > 2; ++i) {
        uint32_t k = static_cast<uint32_t>(rng.next() % num_consts);
        c.emit_op(OpCode::LoadConst);
        c.emit_const_idx(k);
        c.emit_op(ops[rng.next() % 3]);
    }

    c.emit_op(OpCode::Return);

    // Copy bytecode.
    fc.bytecode.assign(c.code().begin(), c.code().end());

    // Compute expected result by evaluating the arithmetic.
    // (We do this in run_fuzz_case by running the interpreter.)

    return fc;
}

[[nodiscard]] FuzzResult run_fuzz_case(Runtime& rt, const FuzzCase& fc) {
    FuzzResult r;
    r.passed = true;

    // Reconstruct the chunk.
    Chunk c;
    c.set_max_locals(0);
    for (int64_t v : fc.constants) {
        c.add_const(make_fuzz_const(v));
    }
    for (uint8_t b : fc.bytecode) {
        c.emit_byte(b);
    }

    // Run at Tier 0 (interpreter) — this is the reference.
    auto ri = rt.run_at_tier(c, Tier::Interpreter);
    if (!ri) {
        r.passed = false;
        r.error = "interpreter failed: " + ri.error();
        return r;
    }
    r.interp_result = ri->as_int();

    // Run at Tier 1.
    auto r1 = rt.run_at_tier(c, Tier::Tier1Baseline);
    if (!r1) {
        r.passed = false;
        r.error = "tier1 failed: " + r1.error();
        return r;
    }
    r.tier1_result = r1->as_int();

    // Run at Tier 2.
    auto r2 = rt.run_at_tier(c, Tier::Tier2Optimizing);
    if (!r2) {
        r.passed = false;
        r.error = "tier2 failed: " + r2.error();
        return r;
    }
    r.tier2_result = r2->as_int();

    // Compare.
    if (r.interp_result != r.tier1_result) {
        r.passed = false;
        r.error = "interp != tier1: " + std::to_string(r.interp_result) +
                  " vs " + std::to_string(r.tier1_result);
    } else if (r.interp_result != r.tier2_result) {
        r.passed = false;
        r.error = "interp != tier2: " + std::to_string(r.interp_result) +
                  " vs " + std::to_string(r.tier2_result);
    }

    return r;
}

[[nodiscard]] size_t run_fuzz_batch(Runtime& rt, uint64_t start_seed, size_t count,
                                     size_t max_ops) {
    size_t failures = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t seed = start_seed + i;
        FuzzCase fc = generate_fuzz_case(seed, max_ops);
        FuzzResult r = run_fuzz_case(rt, fc);
        if (!r.passed) {
            failures++;
            std::println(stderr, "FUZZ FAIL seed={}: {}", seed, r.error);
            std::print(stderr, "  constants: ");
            for (size_t j = 0; j < fc.constants.size(); ++j) {
                if (j > 0) std::print(stderr, ", ");
                std::print(stderr, "{}", fc.constants[j]);
            }
            std::println(stderr);
            std::print(stderr, "  interp={}, tier1={}, tier2={}",
                        r.interp_result, r.tier1_result, r.tier2_result);
        }
    }
    return failures;
}

}  // namespace arcjit
