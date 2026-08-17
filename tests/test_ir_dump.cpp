// SPDX-License-Identifier: MIT
// Tests for the IR dumper.
#include <gtest/gtest.h>

#include "core/graph.h"
#include "core/ir_dump.h"

using namespace arcjit;

TEST(IRDumpTest, dumps_node_kind) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("ConstInt"), std::string::npos);
    EXPECT_NE(s.find("42"), std::string::npos);
}

TEST(IRDumpTest, dumps_node_id) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("n1"), std::string::npos);
    EXPECT_NE(s.find("n2"), std::string::npos);
}

TEST(IRDumpTest, dumps_payload) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 99, {});
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("(99)"), std::string::npos);
}

TEST(IRDumpTest, dumps_zero_payload_omitted) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 0, {});
    std::string s = dump_graph_text(g);
    // Payload 0 is omitted.
    EXPECT_EQ(s.find("(0)"), std::string::npos);
}

TEST(IRDumpTest, dumps_flags) {
    Graph g;
    g.add_node(NodeKind::ConstInt,
               NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::CSEable,
               TypeId::Int, 1, {});
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("pure"), std::string::npos);
    EXPECT_NE(s.find("gvn"), std::string::npos);
    EXPECT_NE(s.find("cse"), std::string::npos);
}

TEST(IRDumpTest, dumps_type) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("type=Int"), std::string::npos);
}

TEST(IRDumpTest, dumps_inputs_with_edge_kinds) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::pair<NodeId, EdgeKind> in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
    g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, in);
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("data:n1"), std::string::npos);
    EXPECT_NE(s.find("data:n2"), std::string::npos);
}

TEST(IRDumpTest, dumps_dead_node_with_prefix) {
    Graph g;
    NodeId c = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    g.mark_dead(c);
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("; dead:"), std::string::npos);
}

TEST(IRDumpTest, dumps_header_with_node_count) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::string s = dump_graph_text(g);
    EXPECT_NE(s.find("// graph:"), std::string::npos);
    EXPECT_NE(s.find("2 nodes"), std::string::npos);
}

TEST(IRDumpTest, dumps_use_count) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::pair<NodeId, EdgeKind> in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
    g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, in);
    std::string s = dump_graph_text(g);
    // c1 and c2 each have 1 use.
    EXPECT_NE(s.find("uses=1"), std::string::npos);
}

TEST(IRDumpTest, dump_single_node) {
    Graph g;
    NodeId c = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
    std::string s = dump_node(g, c);
    EXPECT_NE(s.find("n1"), std::string::npos);
    EXPECT_NE(s.find("ConstInt"), std::string::npos);
    EXPECT_NE(s.find("42"), std::string::npos);
}

TEST(IRDumpTest, dump_with_stats) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::string s = dump_graph_with_stats(g);
    EXPECT_NE(s.find("Graph stats"), std::string::npos);
    EXPECT_NE(s.find("live:"), std::string::npos);
}

TEST(IRDumpTest, type_name_all_types) {
    EXPECT_EQ(type_name(TypeId::Int), "Int");
    EXPECT_EQ(type_name(TypeId::Float), "Float");
    EXPECT_EQ(type_name(TypeId::Bool), "Bool");
    EXPECT_EQ(type_name(TypeId::Null), "Null");
    EXPECT_EQ(type_name(TypeId::Object), "Object");
    EXPECT_EQ(type_name(TypeId::Top), "Top");
    EXPECT_EQ(type_name(TypeId::Bottom), "Bottom");
}

TEST(IRDumpTest, edge_kind_name_all_kinds) {
    EXPECT_EQ(edge_kind_name(EdgeKind::Data), "data");
    EXPECT_EQ(edge_kind_name(EdgeKind::Control), "ctrl");
    EXPECT_EQ(edge_kind_name(EdgeKind::Effect), "effect");
    EXPECT_EQ(edge_kind_name(EdgeKind::FrameState), "fs");
}

TEST(IRDumpTest, stable_output_across_runs) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
    std::pair<NodeId, EdgeKind> in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
    g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, in);

    std::string s1 = dump_graph_text(g);
    std::string s2 = dump_graph_text(g);
    EXPECT_EQ(s1, s2);  // deterministic
}
