// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "core/graph.h"

using namespace arcjit;

TEST(GraphTest, Empty) {
    Graph g;
    EXPECT_EQ(g.size(), 1u);  // slot 0 = sentinel
}

TEST(GraphTest, AddNode) {
    Graph g;
    NodeId n = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
    EXPECT_EQ(n.value, 1u);
    EXPECT_EQ(g.at(n).kind, NodeKind::ConstInt);
    EXPECT_EQ(g.at(n).payload, 42u);
    EXPECT_EQ(g.at(n).input_count, 0);
}

TEST(GraphTest, AddNodeWithInputs) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});

    std::pair<NodeId, EdgeKind> inputs[] = {
        {c1, EdgeKind::Data}, {c2, EdgeKind::Data},
    };
    NodeId add = g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable,
                            TypeId::Int, 0, inputs);
    EXPECT_EQ(g.at(add).input_count, 2);
    EXPECT_EQ(g.at(c1).use_count, 1);
    EXPECT_EQ(g.at(c2).use_count, 1);
}

TEST(GraphTest, DumpDot) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::pair<NodeId, EdgeKind> inputs[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
    g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, inputs);

    std::string dot = g.dump_dot();
    EXPECT_NE(dot.find("digraph G"), std::string::npos);
    EXPECT_NE(dot.find("ConstInt"), std::string::npos);
    EXPECT_NE(dot.find("Add"), std::string::npos);
}

TEST(GraphTest, ReplaceAllUses) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::pair<NodeId, EdgeKind> inputs[] = {{c1, EdgeKind::Data}};
    NodeId add = g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, inputs);

    EXPECT_EQ(g.at(c1).use_count, 1);
    g.replace_all_uses_with(c1, c2);
    EXPECT_EQ(g.at(c1).use_count, 0);
    EXPECT_EQ(g.at(c2).use_count, 1);

    // The Add node should now have c2 as its first input.
    auto in = g.inputs_of(add);
    ASSERT_EQ(in.size(), 1u);
    EXPECT_EQ(in[0].target, c2);
}
