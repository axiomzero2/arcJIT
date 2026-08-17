// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "core/node.h"

using namespace arcjit;

TEST(NodeTest, KindName) {
    EXPECT_EQ(node_kind_name(NodeKind::Add), "Add");
    EXPECT_EQ(node_kind_name(NodeKind::ConstInt), "ConstInt");
    EXPECT_EQ(node_kind_name(NodeKind::Start), "Start");
    EXPECT_EQ(node_kind_name(NodeKind::Phi), "Phi");
}

TEST(NodeTest, Size) {
    EXPECT_EQ(sizeof(Node), 32);
}

TEST(NodeTest, FlagOr) {
    NodeFlags f = NodeFlags::Pure | NodeFlags::GVNable;
    EXPECT_TRUE(has_flag(f, NodeFlags::Pure));
    EXPECT_TRUE(has_flag(f, NodeFlags::GVNable));
    EXPECT_FALSE(has_flag(f, NodeFlags::IsGuard));
}
