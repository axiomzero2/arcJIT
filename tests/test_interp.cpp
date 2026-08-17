// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "interp/interpreter.h"

using namespace arcjit;

// Build a chunk that computes 2 + 3 and returns the result.
static Chunk make_simple_chunk() {
    Chunk c;
    c.set_max_locals(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);  // push 2
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);  // push 3
    c.emit_op(OpCode::Add);                              // → 5
    c.emit_op(OpCode::Return);
    return c;
}

TEST(InterpreterTest, SimpleAdd) {
    Chunk c = make_simple_chunk();

    // Build two constants: Number(2) and Number(3).
    // For the scaffold, we synthesize them as static Arc-compatible objects.
    // We use the Object/Number structs from object.h directly.
    static Number two;
    two.base.type       = ObjType::NumberInt;
    two.base.ref_count  = 1;
    two.base.is_static = true;
    two.as.i            = 2;

    static Number three;
    three.base.type       = ObjType::NumberInt;
    three.base.ref_count  = 1;
    three.base.is_static  = true;
    three.as.i            = 3;

    // The Chunk's constant pool is a vector<Object*>. We need to inject
    // our two Numbers. The Chunk API exposes constants() as read-only; we
    // reach in via the test helper `add_const`.
    uint32_t id0 = c.add_const(reinterpret_cast<Object*>(&two));
    uint32_t id1 = c.add_const(reinterpret_cast<Object*>(&three));
    EXPECT_EQ(id0, 0u);
    EXPECT_EQ(id1, 1u);

    Interpreter interp;
    auto result = interp.run(c);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->is_int());
    EXPECT_EQ(result->as_int(), 5);
}

TEST(InterpreterTest, LocalsAndArith) {
    // Compute: local 0 = 10; local 1 = 20; return (local 0) * (local 1) + 5
    Chunk c;
    c.set_max_locals(2);

    static Number ten;
    ten.base.type = ObjType::NumberInt; ten.base.ref_count = 1; ten.base.is_static = true; ten.as.i = 10;
    static Number twenty;
    twenty.base.type = ObjType::NumberInt; twenty.base.ref_count = 1; twenty.base.is_static = true; twenty.as.i = 20;
    static Number five;
    five.base.type = ObjType::NumberInt; five.base.ref_count = 1; five.base.is_static = true; five.as.i = 5;

    c.add_const(reinterpret_cast<Object*>(&ten));
    c.add_const(reinterpret_cast<Object*>(&twenty));
    c.add_const(reinterpret_cast<Object*>(&five));

    // local 0 = 10
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(0);
    // local 1 = 20
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::StoreLocal); c.emit_byte(1);
    // local 0 * local 1
    c.emit_op(OpCode::LoadLocal); c.emit_byte(0);
    c.emit_op(OpCode::LoadLocal); c.emit_byte(1);
    c.emit_op(OpCode::Mul);
    // + 5
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(2);
    c.emit_op(OpCode::Add);
    // return
    c.emit_op(OpCode::Return);

    Interpreter interp;
    auto result = interp.run(c);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->as_int(), 205);  // 10 * 20 + 5 = 205
}

TEST(InterpreterTest, DivByZero) {
    Chunk c;
    c.set_max_locals(0);

    static Number ten;
    ten.base.type = ObjType::NumberInt; ten.base.ref_count = 1; ten.base.is_static = true; ten.as.i = 10;

    c.add_const(reinterpret_cast<Object*>(&ten));

    // Stack effect:
    //   push 10 (dividend)         → [10]
    //   push 10                    → [10, 10]
    //   push 10                    → [10, 10, 10]
    //   Sub  (pops 2, pushes diff) → [10, 0]
    //   Div  (pops b=0, a=10)      → 10/0 → div-by-zero error
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Sub);
    c.emit_op(OpCode::Div);

    Interpreter interp;
    auto result = interp.run(c);
    ASSERT_FALSE(result.has_value()) << "Expected DivByZero error, got value";
    EXPECT_NE(result.error().find("Division by zero"), std::string::npos);
}
