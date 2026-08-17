// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "passman/pass.h"

using namespace arcjit;

TEST(PassTest, ConstantFolding) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});
    std::pair<NodeId, EdgeKind> inputs[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
    NodeId add = g.add_node(NodeKind::Add,
                            NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                            TypeId::Int, 0, inputs);

    ConstantFoldingPass p;
    PassResult r = p.run(g);
    EXPECT_TRUE(r.changed);
    EXPECT_TRUE(has_flag(g.at(add).flags, NodeFlags::IsDead));

    // The new ConstInt node should have payload = 3.
    bool found = false;
    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        const Node& n = g.at(id);
        if (n.kind == NodeKind::ConstInt && n.payload == 3) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(PassTest, GVN) {
    Graph g;
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});
    EXPECT_NE(c1, c2);

    GVNPass p;
    PassResult r = p.run(g);
    EXPECT_TRUE(r.changed);
    EXPECT_TRUE(has_flag(g.at(c2).flags, NodeFlags::IsDead));
    EXPECT_FALSE(has_flag(g.at(c1).flags, NodeFlags::IsDead));
}

TEST(PassTest, PipelineFixpoint) {
    Graph g;
    // Build (1+2) + (1+2) — should reduce to 6 via GVN + ConstFold.
    NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});

    std::pair<NodeId, EdgeKind> add1_in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
    NodeId add1 = g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                              TypeId::Int, 0, add1_in);

    // Same inputs as add1 — GVN should dedupe.
    NodeId add2 = g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                              TypeId::Int, 0, add1_in);

    std::pair<NodeId, EdgeKind> add3_in[] = {{add1, EdgeKind::Data}, {add2, EdgeKind::Data}};
    NodeId add3 = g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                              TypeId::Int, 0, add3_in);

    PassPipeline pipe;
    pipe.add(std::make_unique<GVNPass>());
    pipe.add(std::make_unique<ConstantFoldingPass>());
    pipe.add(std::make_unique<DeadCodeElimPass>());

    PassResult r = pipe.run_to_fixpoint(g, 8);
    EXPECT_TRUE(r.changed);
    // GVN should have deduped add2 → add1.
    // ConstFold should have folded add1 → ConstInt(3).
    // Then add3 (3 + 3) → ConstInt(6).
    bool found_six = false;
    for (size_t i = 1; i < g.size(); ++i) {
        const Node& n = g.at(NodeId{static_cast<uint32_t>(i)});
        if (n.kind == NodeKind::ConstInt && n.payload == 6) {
            found_six = true;
        }
    }
    EXPECT_TRUE(found_six);
}
