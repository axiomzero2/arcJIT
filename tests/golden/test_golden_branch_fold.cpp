// SPDX-License-Identifier: MIT
// Golden tests for the BranchFolding pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

// Helper: build an If with a constant condition.
static Graph make_if_with_const_cond(int64_t cond_val) {
    Graph g;
    NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
    g.set_start(start);
    NodeId cond = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable,
                              TypeId::Int, static_cast<uint32_t>(cond_val), {});
    std::pair<NodeId, EdgeKind> if_in[] = {{cond, EdgeKind::Data}, {start, EdgeKind::Control}};
    NodeId if_node = g.add_node(NodeKind::If, NodeFlags::IsControl | NodeFlags::NoDeopt,
                                 TypeId::Bottom, 0, if_in);
    std::pair<NodeId, EdgeKind> t_in[] = {{if_node, EdgeKind::Control}};
    g.add_node(NodeKind::IfTrue, NodeFlags::IsControl | NodeFlags::NoDeopt, TypeId::Bottom, 0, t_in);
    std::pair<NodeId, EdgeKind> f_in[] = {{if_node, EdgeKind::Control}};
    g.add_node(NodeKind::IfFalse, NodeFlags::IsControl | NodeFlags::NoDeopt, TypeId::Bottom, 0, f_in);
    return g;
}

// 1. if (true) → IfFalse is dead.
TEST(BranchFoldGoldenTest, if_true_kills_false_branch) {
    EXPECT_TRUE(check_golden("BranchFold", "if_true_kills_false_branch",
        []() { return make_if_with_const_cond(1); },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 2. if (false) → IfTrue is dead.
TEST(BranchFoldGoldenTest, if_false_kills_true_branch) {
    EXPECT_TRUE(check_golden("BranchFold", "if_false_kills_true_branch",
        []() { return make_if_with_const_cond(0); },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 3. if (non-constant) → no fold.
TEST(BranchFoldGoldenTest, non_constant_condition_no_fold) {
    EXPECT_TRUE(check_golden("BranchFold", "non_constant_condition_no_fold",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            NodeId cond = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            std::pair<NodeId, EdgeKind> if_in[] = {{cond, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId if_node = g.add_node(NodeKind::If, NodeFlags::IsControl | NodeFlags::NoDeopt,
                                         TypeId::Bottom, 0, if_in);
            std::pair<NodeId, EdgeKind> t_in[] = {{if_node, EdgeKind::Control}};
            g.add_node(NodeKind::IfTrue, NodeFlags::IsControl | NodeFlags::NoDeopt, TypeId::Bottom, 0, t_in);
            std::pair<NodeId, EdgeKind> f_in[] = {{if_node, EdgeKind::Control}};
            g.add_node(NodeKind::IfFalse, NodeFlags::IsControl | NodeFlags::NoDeopt, TypeId::Bottom, 0, f_in);
            return g;
        },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 4. if (42) → IfFalse is dead (non-zero is true).
TEST(BranchFoldGoldenTest, if_nonzero_kills_false_branch) {
    EXPECT_TRUE(check_golden("BranchFold", "if_nonzero_kills_false_branch",
        []() { return make_if_with_const_cond(42); },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 5. Empty graph — no-op.
TEST(BranchFoldGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("BranchFold", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 6. No If nodes — no-op.
TEST(BranchFoldGoldenTest, no_if_nodes_noop) {
    EXPECT_TRUE(check_golden("BranchFold", "no_if_nodes_noop",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            return g;
        },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 7. Multiple If nodes with constant conditions.
TEST(BranchFoldGoldenTest, multiple_constant_ifs) {
    EXPECT_TRUE(check_golden("BranchFold", "multiple_constant_ifs",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            // First if: true
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            std::pair<NodeId, EdgeKind> if1_in[] = {{c1, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId if1 = g.add_node(NodeKind::If, NodeFlags::IsControl, TypeId::Bottom, 0, if1_in);
            std::pair<NodeId, EdgeKind> t1_in[] = {{if1, EdgeKind::Control}};
            g.add_node(NodeKind::IfTrue, NodeFlags::IsControl, TypeId::Bottom, 0, t1_in);
            std::pair<NodeId, EdgeKind> f1_in[] = {{if1, EdgeKind::Control}};
            g.add_node(NodeKind::IfFalse, NodeFlags::IsControl, TypeId::Bottom, 0, f1_in);
            // Second if: false
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 0, {});
            std::pair<NodeId, EdgeKind> if2_in[] = {{c2, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId if2 = g.add_node(NodeKind::If, NodeFlags::IsControl, TypeId::Bottom, 0, if2_in);
            std::pair<NodeId, EdgeKind> t2_in[] = {{if2, EdgeKind::Control}};
            g.add_node(NodeKind::IfTrue, NodeFlags::IsControl, TypeId::Bottom, 0, t2_in);
            std::pair<NodeId, EdgeKind> f2_in[] = {{if2, EdgeKind::Control}};
            g.add_node(NodeKind::IfFalse, NodeFlags::IsControl, TypeId::Bottom, 0, f2_in);
            return g;
        },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 8. If with negative condition (-1 is truthy).
TEST(BranchFoldGoldenTest, if_negative_nonzero_kills_false) {
    EXPECT_TRUE(check_golden("BranchFold", "if_negative_nonzero_kills_false",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            // -1 as uint32 is 0xFFFFFFFF
            NodeId cond = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 0xFFFFFFFF, {});
            std::pair<NodeId, EdgeKind> if_in[] = {{cond, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId if_node = g.add_node(NodeKind::If, NodeFlags::IsControl, TypeId::Bottom, 0, if_in);
            std::pair<NodeId, EdgeKind> t_in[] = {{if_node, EdgeKind::Control}};
            g.add_node(NodeKind::IfTrue, NodeFlags::IsControl, TypeId::Bottom, 0, t_in);
            std::pair<NodeId, EdgeKind> f_in[] = {{if_node, EdgeKind::Control}};
            g.add_node(NodeKind::IfFalse, NodeFlags::IsControl, TypeId::Bottom, 0, f_in);
            return g;
        },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 9. IfTrue without matching IfFalse.
TEST(BranchFoldGoldenTest, if_true_only) {
    EXPECT_TRUE(check_golden("BranchFold", "if_true_only",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            NodeId cond = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            std::pair<NodeId, EdgeKind> if_in[] = {{cond, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId if_node = g.add_node(NodeKind::If, NodeFlags::IsControl, TypeId::Bottom, 0, if_in);
            std::pair<NodeId, EdgeKind> t_in[] = {{if_node, EdgeKind::Control}};
            g.add_node(NodeKind::IfTrue, NodeFlags::IsControl, TypeId::Bottom, 0, t_in);
            // No IfFalse — the pass should handle this gracefully.
            return g;
        },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 10. If with a large truthy constant.
TEST(BranchFoldGoldenTest, if_large_truthy_constant) {
    EXPECT_TRUE(check_golden("BranchFold", "if_large_truthy_constant",
        []() { return make_if_with_const_cond(1000000); },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 11. IfFalse only (no IfTrue).
TEST(BranchFoldGoldenTest, if_false_only) {
    EXPECT_TRUE(check_golden("BranchFold", "if_false_only",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            NodeId cond = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 0, {});
            std::pair<NodeId, EdgeKind> if_in[] = {{cond, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId if_node = g.add_node(NodeKind::If, NodeFlags::IsControl, TypeId::Bottom, 0, if_in);
            std::pair<NodeId, EdgeKind> f_in[] = {{if_node, EdgeKind::Control}};
            g.add_node(NodeKind::IfFalse, NodeFlags::IsControl, TypeId::Bottom, 0, f_in);
            return g;
        },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}

// 12. Already-dead IfTrue/IfFalse are skipped.
TEST(BranchFoldGoldenTest, skips_already_dead) {
    EXPECT_TRUE(check_golden("BranchFold", "skips_already_dead",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            NodeId cond = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            std::pair<NodeId, EdgeKind> if_in[] = {{cond, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId if_node = g.add_node(NodeKind::If, NodeFlags::IsControl, TypeId::Bottom, 0, if_in);
            std::pair<NodeId, EdgeKind> t_in[] = {{if_node, EdgeKind::Control}};
            NodeId if_true = g.add_node(NodeKind::IfTrue, NodeFlags::IsControl, TypeId::Bottom, 0, t_in);
            g.mark_dead(if_true);  // pre-dead
            std::pair<NodeId, EdgeKind> f_in[] = {{if_node, EdgeKind::Control}};
            g.add_node(NodeKind::IfFalse, NodeFlags::IsControl, TypeId::Bottom, 0, f_in);
            return g;
        },
        [](Graph& g) { BranchFoldingPass p; p.run(g); }));
}
