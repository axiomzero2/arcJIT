// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "bytecode/value.h"

using namespace arcjit;

TEST(ValueTest, IntConstruction) {
    Value v = Value::Int(42);
    EXPECT_TRUE(v.is_int());
    EXPECT_FALSE(v.is_float());
    EXPECT_EQ(v.as_int(), 42);
}

TEST(ValueTest, FloatConstruction) {
    Value v = Value::Float(3.14);
    EXPECT_TRUE(v.is_float());
    EXPECT_FALSE(v.is_int());
    EXPECT_DOUBLE_EQ(v.as_float(), 3.14);
}

TEST(ValueTest, NullConstruction) {
    Value v = Value::null();
    EXPECT_TRUE(v.is_null());
    EXPECT_FALSE(v.is_truthy());
}

TEST(ValueTest, UndefConstruction) {
    Value v = Value::undef();
    EXPECT_TRUE(v.is_undef());
    EXPECT_FALSE(v.is_truthy());
}

TEST(ValueTest, IntTruthiness) {
    EXPECT_TRUE(Value::Int(1).is_truthy());
    EXPECT_FALSE(Value::Int(0).is_truthy());
    EXPECT_TRUE(Value::Int(-1).is_truthy());
}

TEST(ValueTest, FloatTruthiness) {
    EXPECT_TRUE(Value::Float(1.0).is_truthy());
    EXPECT_FALSE(Value::Float(0.0).is_truthy());
    EXPECT_TRUE(Value::Float(-1.5).is_truthy());
}

TEST(ValueTest, SizeIs16Bytes) {
    EXPECT_EQ(sizeof(Value), 16);
}
