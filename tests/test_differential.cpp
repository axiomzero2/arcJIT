// SPDX-License-Identifier: MIT
// Differential testing framework tests (Rule 38).
//
// These tests run the same Chunk through all three tiers and assert that
// the results agree. Divergence = bug.
#include <gtest/gtest.h>

#include <memory>

#include "runtime/runtime.h"
#include "test_differential.h"

using namespace arcjit;

// Helper to make a static Number constant (lives for the test duration).
static Object* make_num(int64_t v) {
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

// Helper to build a chunk from a list of (opcode, operand) specs.
// For "1+2+3": push 1, push 2, add, push 3, add, return.
static Chunk make_arith_chunk(std::initializer_list<int64_t> nums,
                              std::initializer_list<char> ops) {
    Chunk c;
    c.set_max_locals(0);
    std::vector<uint32_t> indices;
    for (int64_t n : nums) {
        indices.push_back(c.add_const(make_num(n)));
    }
    auto op_it = ops.begin();
    auto num_it = indices.begin();
    bool first = true;
    for (uint32_t idx : indices) {
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

// 1+2+3 → 6, all tiers agree.
TEST(DifferentialTest, add_three_constants_all_tiers_agree) {
    Runtime rt;
    Chunk c = make_arith_chunk({1, 2, 3}, {'+', '+'});
    auto r = diff_test_chunk(rt, c, 6);
    EXPECT_TRUE(r.passed) << r.failure_message;
}

// (10+20)*3 → 90.
TEST(DifferentialTest, add_then_mul_all_tiers_agree) {
    Runtime rt;
    Chunk c = make_arith_chunk({10, 20, 3}, {'+', '*'});
    auto r = diff_test_chunk(rt, c, 90);
    EXPECT_TRUE(r.passed) << r.failure_message;
}

// 100-50-25 → 25.
TEST(DifferentialTest, sub_chain_all_tiers_agree) {
    Runtime rt;
    Chunk c = make_arith_chunk({100, 50, 25}, {'-', '-'});
    auto r = diff_test_chunk(rt, c, 25);
    EXPECT_TRUE(r.passed) << r.failure_message;
}

// 2*3*4 → 24.
TEST(DifferentialTest, mul_chain_all_tiers_agree) {
    Runtime rt;
    Chunk c = make_arith_chunk({2, 3, 4}, {'*', '*'});
    auto r = diff_test_chunk(rt, c, 24);
    EXPECT_TRUE(r.passed) << r.failure_message;
}

// Single constant: 42 → 42.
TEST(DifferentialTest, single_constant_all_tiers_agree) {
    Runtime rt;
    Chunk c = make_arith_chunk({42}, {});
    auto r = diff_test_chunk(rt, c, 42);
    EXPECT_TRUE(r.passed) << r.failure_message;
}

// Larger expression: 1+2+3+4+5 → 15.
TEST(DifferentialTest, five_constant_add_all_tiers_agree) {
    Runtime rt;
    Chunk c = make_arith_chunk({1, 2, 3, 4, 5}, {'+', '+', '+', '+'});
    auto r = diff_test_chunk(rt, c, 15);
    EXPECT_TRUE(r.passed) << r.failure_message;
}

// Batch test: run many cases, count failures.
TEST(DifferentialTest, batch_arith_no_failures) {
    Runtime rt;
    auto failures = diff_test_batch(rt, {
        {"1+2",      3},
        {"10-5",     5},
        {"4*5",      20},
        {"1+2+3",    6},
        {"2*3*4",   24},
        {"100-50",  50},
    });
    EXPECT_EQ(failures, 0u);
}

// A chunk that uses locals.
TEST(DifferentialTest, locals_all_tiers_agree) {
    Runtime rt;
    Chunk c;
    c.set_max_locals(2);
    c.add_const(make_num(10));
    c.add_const(make_num(20));
    // local 0 = 10; local 1 = 20; return local 0 + local 1
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);

    auto r = diff_test_chunk(rt, c, 30);
    EXPECT_TRUE(r.passed) << r.failure_message;
}

// A chunk with a comparison (no branch).
TEST(DifferentialTest, comparison_all_tiers_agree) {
    Runtime rt;
    Chunk c;
    c.set_max_locals(0);
    c.add_const(make_num(10));
    c.add_const(make_num(5));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Gt);  // 10 > 5 → 1
    c.emit_op(OpCode::Return);

    auto r = diff_test_chunk(rt, c, 1);
    EXPECT_TRUE(r.passed) << r.failure_message;
}
