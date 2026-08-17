// SPDX-License-Identifier: MIT
// Golden tests for the DeadCodeElimPass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

// 1. Dead pure node (no uses) is removed.
TEST(DCEGoldenTest, removes_dead_pure_node) {
    EXPECT_TRUE(check_golden("DCE", "removes_dead_pure_node",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 2. Live pure node (has uses) is NOT removed.
TEST(DCEGoldenTest, keeps_live_pure_node) {
    EXPECT_TRUE(check_golden("DCE", "keeps_live_pure_node",
        []() {
            Graph g;
            NodeId c = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
            std::pair<NodeId, EdgeKind> in[] = {{c, EdgeKind::Data}};
            g.add_node(NodeKind::Stop, NodeFlags::IsControl, TypeId::Bottom, 0, in);
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 3. Effectful node with no uses is NOT removed (has side effects).
TEST(DCEGoldenTest, keeps_effectful_node_without_uses) {
    EXPECT_TRUE(check_golden("DCE", "keeps_effectful_node_without_uses",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            std::pair<NodeId, EdgeKind> in[] = {{start, EdgeKind::Control}, {start, EdgeKind::Effect}};
            g.add_node(NodeKind::StoreLocal, NodeFlags::IsEffect, TypeId::Bottom, 0, in);
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 4. Chain of dead pure nodes.
TEST(DCEGoldenTest, removes_chain_of_dead_pure_nodes) {
    EXPECT_TRUE(check_golden("DCE", "removes_chain_of_dead_pure_nodes",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, in);  // no users
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 5. Empty graph → no-op.
TEST(DCEGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("DCE", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 6. Already-dead node is skipped.
TEST(DCEGoldenTest, skips_already_dead_node) {
    EXPECT_TRUE(check_golden("DCE", "skips_already_dead_node",
        []() {
            Graph g;
            NodeId c = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            g.mark_dead(c);
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 7. Mix of live and dead nodes.
TEST(DCEGoldenTest, mix_live_and_dead) {
    EXPECT_TRUE(check_golden("DCE", "mix_live_and_dead",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});  // dead
            std::pair<NodeId, EdgeKind> stop_in[] = {{c1, EdgeKind::Data}};
            g.set_start(g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {}));
            g.set_stop(g.add_node(NodeKind::Stop, NodeFlags::IsControl, TypeId::Bottom, 0, stop_in));
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 8. Control node with no uses is NOT removed (not pure).
TEST(DCEGoldenTest, keeps_control_node_without_uses) {
    EXPECT_TRUE(check_golden("DCE", "keeps_control_node_without_uses",
        []() {
            Graph g;
            g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 9. Multiple dead constants.
TEST(DCEGoldenTest, removes_multiple_dead_constants) {
    EXPECT_TRUE(check_golden("DCE", "removes_multiple_dead_constants",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 3, {});
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 10. Realistic function: Start + consts + Add + Stop (all live).
TEST(DCEGoldenTest, realistic_function_all_live) {
    EXPECT_TRUE(check_golden("DCE", "realistic_function_all_live",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> add_in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId add = g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, add_in);
            std::pair<NodeId, EdgeKind> stop_in[] = {{add, EdgeKind::Data}, {start, EdgeKind::Control}};
            g.set_stop(g.add_node(NodeKind::Stop, NodeFlags::IsControl, TypeId::Bottom, 0, stop_in));
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 11. Node with only control input (no data uses) is kept if not pure.
TEST(DCEGoldenTest, node_with_only_control_input_kept) {
    EXPECT_TRUE(check_golden("DCE", "node_with_only_control_input_kept",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            std::pair<NodeId, EdgeKind> in[] = {{start, EdgeKind::Control}};
            g.add_node(NodeKind::Region, NodeFlags::IsControl, TypeId::Bottom, 0, in);
            return g;
        },
        [](Graph& g) { DeadCodeElimPass p; p.run(g); }));
}

// 12. Dead node that was the only user of another dead node.
TEST(DCEGoldenTest, dead_chain_cascade) {
    EXPECT_TRUE(check_golden("DCE", "dead_chain_cascade",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> add_in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, add_in);
            // The Add has no users, so it's dead. c1 and c2 have 1 user each (the Add),
            // so they're live until DCE removes the Add.
            return g;
        },
        [](Graph& g) {
            // Run DCE twice to cascade.
            DeadCodeElimPass p;
            p.run(g);
            p.run(g);
        }));
}
