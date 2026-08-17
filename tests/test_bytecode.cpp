// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "bytecode/chunk.h"

using namespace arcjit;

TEST(BytecodeTest, OpcodeName) {
    EXPECT_EQ(opcode_name(OpCode::Add), "Add");
    EXPECT_EQ(opcode_name(OpCode::Halt), "Halt");
    EXPECT_EQ(opcode_name(OpCode::LoadConst), "LoadConst");
}

TEST(BytecodeTest, EmitByte) {
    Chunk c;
    c.emit_byte(0xAB);
    c.emit_byte(0xCD);
    auto bytes = c.code();
    ASSERT_EQ(bytes.size(), 2u);
    EXPECT_EQ(bytes[0], 0xAB);
    EXPECT_EQ(bytes[1], 0xCD);
}

TEST(BytecodeTest, EmitOp) {
    Chunk c;
    c.emit_op(OpCode::Add);
    ASSERT_EQ(c.code().size(), 1u);
    EXPECT_EQ(c.code()[0], static_cast<uint8_t>(OpCode::Add));
}

TEST(BytecodeTest, EmitConstIdx) {
    Chunk c;
    c.emit_const_idx(0x123456);
    ASSERT_EQ(c.code().size(), 3u);
    EXPECT_EQ(c.code()[0], 0x12);
    EXPECT_EQ(c.code()[1], 0x34);
    EXPECT_EQ(c.code()[2], 0x56);
}

TEST(BytecodeTest, EmitShort) {
    Chunk c;
    c.emit_short(-100);
    ASSERT_EQ(c.code().size(), 2u);
    // -100 as int16_t is 0xFF9C
    EXPECT_EQ(c.code()[0], 0xFF);
    EXPECT_EQ(c.code()[1], 0x9C);
}

TEST(BytecodeTest, BytecodeReader) {
    Chunk c;
    c.emit_op(OpCode::LoadConst);
    c.emit_const_idx(0x123456);
    c.emit_op(OpCode::Jump);
    c.emit_short(-50);
    c.emit_byte(0x42);  // dummy

    BytecodeReader r{c.code(), 0};
    EXPECT_EQ(r.read_op(), OpCode::LoadConst);
    EXPECT_EQ(r.read_const_idx(), 0x123456u);
    EXPECT_EQ(r.read_op(), OpCode::Jump);
    EXPECT_EQ(r.read_short(), -50);
    EXPECT_EQ(r.read_byte(), 0x42);
    EXPECT_TRUE(r.at_end());
}
