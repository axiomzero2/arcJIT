// SPDX-License-Identifier: MIT
// Tests for the deopt validator (Rule 39).
#include <gtest/gtest.h>

#include "bytecode/chunk.h"
#include "bytecode/heap.h"
#include "runtime/deopt_validator.h"

using namespace arcjit;

static Object* make_num_deopt(int64_t v) {
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

TEST(DeoptValidatorTest, validates_correct_value) {
    Chunk c;
    c.set_max_locals(0);
    c.add_const(make_num_deopt(10));
    c.add_const(make_num_deopt(20));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);

    DeoptFrameState state;
    state.chunk = &c;
    state.bytecode_ip = 0;
    state.reason = "test_deopt";

    // The interpreter should produce 30.
    auto r = validate_deopt(state, Value::Int(30));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->as_int(), 30);

    // The log should have one entry, matched=true.
    auto& log = global_deopt_log();
    ASSERT_EQ(log.count(), 1u);
    EXPECT_TRUE(log.entries()[0].matched);
    log.clear();
}

TEST(DeoptValidatorTest, detects_mismatch) {
    Chunk c;
    c.set_max_locals(0);
    c.add_const(make_num_deopt(5));
    c.add_const(make_num_deopt(7));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);

    DeoptFrameState state;
    state.chunk = &c;
    state.bytecode_ip = 0;
    state.reason = "test_mismatch";

    // Expect 100, but interpreter produces 12.
    auto r = validate_deopt(state, Value::Int(100));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int(), 12);

    auto& log = global_deopt_log();
    ASSERT_EQ(log.count(), 1u);
    EXPECT_FALSE(log.entries()[0].matched);
    log.clear();
}

TEST(DeoptValidatorTest, null_chunk_returns_error) {
    DeoptFrameState state;
    state.chunk = nullptr;
    auto r = validate_deopt(state);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("chunk is null"), std::string::npos);
}

TEST(DeoptValidatorTest, log_clear_resets) {
    auto& log = global_deopt_log();
    log.record({"test", "chunk", 0, 0, Value::Int(0), std::nullopt, true});
    EXPECT_EQ(log.count(), 1u);
    log.clear();
    EXPECT_EQ(log.count(), 0u);
}

TEST(DeoptValidatorTest, log_tracks_mismatches) {
    auto& log = global_deopt_log();
    log.clear();
    log.record({"a", "c1", 0, 0, Value::Int(5), Value::Int(5), true});
    log.record({"b", "c2", 0, 0, Value::Int(3), Value::Int(4), false});
    log.record({"c", "c3", 0, 0, Value::Int(7), Value::Int(7), true});
    log.record({"d", "c4", 0, 0, Value::Int(1), Value::Int(2), false});
    EXPECT_EQ(log.count(), 4u);
    EXPECT_EQ(log.mismatch_count(), 2u);
    log.clear();
}

TEST(DeoptValidatorTest, no_expected_value_always_matched) {
    Chunk c;
    c.set_max_locals(0);
    c.add_const(make_num_deopt(42));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);

    DeoptFrameState state;
    state.chunk = &c;
    state.bytecode_ip = 0;
    state.reason = "no_expected";

    auto r = validate_deopt(state);  // no expected value
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->as_int(), 42);

    auto& log = global_deopt_log();
    ASSERT_EQ(log.count(), 1u);
    // When there's no expected value, matched should be false (no comparison).
    EXPECT_FALSE(log.entries()[0].matched);
    log.clear();
}
