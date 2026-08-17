// SPDX-License-Identifier: MIT
// Golden tests for the ConstantFolding pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

static NodeId add_const_int(Graph& g, int64_t v) {
    return g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int,
                       static_cast<uint32_t>(v), {});
}

static NodeId add_binop(Graph& g, NodeKind k, NodeId a, NodeId b) {
    std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}, {b, EdgeKind::Data}};
    return g.add_node(k, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                       TypeId::Int, 0, in);
}

// 1. Fold 1 + 2 → 3.
TEST(ConstFoldGoldenTest, fold_add_two_constants) {
    EXPECT_TRUE(check_golden("ConstFold", "fold_add_two_constants",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 1);
            NodeId c2 = add_const_int(g, 2);
            add_binop(g, NodeKind::Add, c1, c2);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 2. Fold 10 - 3 → 7.
TEST(ConstFoldGoldenTest, fold_sub_two_constants) {
    EXPECT_TRUE(check_golden("ConstFold", "fold_sub_two_constants",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 10);
            NodeId c2 = add_const_int(g, 3);
            add_binop(g, NodeKind::Sub, c1, c2);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 3. Fold 4 * 5 → 20.
TEST(ConstFoldGoldenTest, fold_mul_two_constants) {
    EXPECT_TRUE(check_golden("ConstFold", "fold_mul_two_constants",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 4);
            NodeId c2 = add_const_int(g, 5);
            add_binop(g, NodeKind::Mul, c1, c2);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 4. Do NOT fold when one input is not a constant.
TEST(ConstFoldGoldenTest, no_fold_when_one_input_not_const) {
    EXPECT_TRUE(check_golden("ConstFold", "no_fold_when_one_input_not_const",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 1);
            NodeId p  = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_binop(g, NodeKind::Add, c1, p);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 5. Chain: (1 + 2) + 3 → 6 after two passes.
TEST(ConstFoldGoldenTest, fold_chain_two_passes) {
    EXPECT_TRUE(check_golden("ConstFold", "fold_chain_two_passes",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 1);
            NodeId c2 = add_const_int(g, 2);
            NodeId c3 = add_const_int(g, 3);
            NodeId inner = add_binop(g, NodeKind::Add, c1, c2);
            add_binop(g, NodeKind::Add, inner, c3);
            return g;
        },
        [](Graph& g) {
            ConstantFoldingPass p;
            p.run(g);
            p.run(g);  // second pass folds the chain
        }));
}

// 6. Fold 0 + x → not folded (we only fold when BOTH are const).
TEST(ConstFoldGoldenTest, no_fold_zero_plus_var) {
    EXPECT_TRUE(check_golden("ConstFold", "no_fold_zero_plus_var",
        []() {
            Graph g;
            NodeId c0 = add_const_int(g, 0);
            NodeId p  = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_binop(g, NodeKind::Add, c0, p);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 7. Single Add with no constants → no-op.
TEST(ConstFoldGoldenTest, no_constants_noop) {
    EXPECT_TRUE(check_golden("ConstFold", "no_constants_noop",
        []() {
            Graph g;
            NodeId p1 = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId p2 = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            add_binop(g, NodeKind::Add, p1, p2);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 8. Empty graph → no-op.
TEST(ConstFoldGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("ConstFold", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 9. Fold negative result: 3 - 10 → -7.
TEST(ConstFoldGoldenTest, fold_negative_result) {
    EXPECT_TRUE(check_golden("ConstFold", "fold_negative_result",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 3);
            NodeId c2 = add_const_int(g, 10);
            add_binop(g, NodeKind::Sub, c1, c2);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 10. Fold large multiplication: 1000 * 1000 → 1000000.
TEST(ConstFoldGoldenTest, fold_large_multiplication) {
    EXPECT_TRUE(check_golden("ConstFold", "fold_large_multiplication",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 1000);
            NodeId c2 = add_const_int(g, 1000);
            add_binop(g, NodeKind::Mul, c1, c2);
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 11. Multiple foldable ops in one graph.
TEST(ConstFoldGoldenTest, multiple_foldable_ops) {
    EXPECT_TRUE(check_golden("ConstFold", "multiple_foldable_ops",
        []() {
            Graph g;
            NodeId c1 = add_const_int(g, 1);
            NodeId c2 = add_const_int(g, 2);
            NodeId c3 = add_const_int(g, 3);
            NodeId c4 = add_const_int(g, 4);
            add_binop(g, NodeKind::Add, c1, c2);   // → 3
            add_binop(g, NodeKind::Mul, c3, c4);   // → 12
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}

// 12. Fold with control edges present (data edges are the only ones used).
TEST(ConstFoldGoldenTest, fold_with_control_edges_present) {
    EXPECT_TRUE(check_golden("ConstFold", "fold_with_control_edges_present",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            NodeId c1 = add_const_int(g, 5);
            NodeId c2 = add_const_int(g, 10);
            std::pair<NodeId, EdgeKind> in[] = {
                {c1, EdgeKind::Data}, {c2, EdgeKind::Data}, {start, EdgeKind::Control}
            };
            NodeId add = g.add_node(NodeKind::Add,
                                     NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                                     TypeId::Int, 0, in);
            std::pair<NodeId, EdgeKind> stop_in[] = {{add, EdgeKind::Data}, {start, EdgeKind::Control}};
            g.set_stop(g.add_node(NodeKind::Stop, NodeFlags::IsControl, TypeId::Bottom, 0, stop_in));
            return g;
        },
        [](Graph& g) { ConstantFoldingPass p; p.run(g); }));
}
