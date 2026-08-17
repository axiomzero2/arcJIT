// SPDX-License-Identifier: MIT
// Golden tests for the ComparisonFolding pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

static NodeId add_const_cf(Graph& g, int64_t v) {
    return g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int,
                       static_cast<uint32_t>(v), {});
}

static NodeId add_cmp(Graph& g, NodeKind k, NodeId a, NodeId b) {
    std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}, {b, EdgeKind::Data}};
    return g.add_node(k, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                       TypeId::Bool, 0, in);
}

// 1. x == x → true
TEST(CompareFoldGoldenTest, eq_self_returns_true) {
    EXPECT_TRUE(check_golden("CompareFold", "eq_self_returns_true",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_cmp(g, NodeKind::Eq, x, x);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 2. x != x → false
TEST(CompareFoldGoldenTest, ne_self_returns_false) {
    EXPECT_TRUE(check_golden("CompareFold", "ne_self_returns_false",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_cmp(g, NodeKind::Ne, x, x);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 3. !(x < y) → x >= y
TEST(CompareFoldGoldenTest, not_lt_becomes_gte) {
    EXPECT_TRUE(check_golden("CompareFold", "not_lt_becomes_gte",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            NodeId lt = add_cmp(g, NodeKind::Lt, x, y);
            std::pair<NodeId, EdgeKind> in[] = {{lt, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Bool, 0, in);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 4. !(x > y) → x <= y
TEST(CompareFoldGoldenTest, not_gt_becomes_lte) {
    EXPECT_TRUE(check_golden("CompareFold", "not_gt_becomes_lte",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            NodeId gt = add_cmp(g, NodeKind::Gt, x, y);
            std::pair<NodeId, EdgeKind> in[] = {{gt, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Bool, 0, in);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 5. !(x == y) → x != y
TEST(CompareFoldGoldenTest, not_eq_becomes_ne) {
    EXPECT_TRUE(check_golden("CompareFold", "not_eq_becomes_ne",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            NodeId eq = add_cmp(g, NodeKind::Eq, x, y);
            std::pair<NodeId, EdgeKind> in[] = {{eq, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Bool, 0, in);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 6. !(x != y) → x == y
TEST(CompareFoldGoldenTest, not_ne_becomes_eq) {
    EXPECT_TRUE(check_golden("CompareFold", "not_ne_becomes_eq",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            NodeId ne = add_cmp(g, NodeKind::Ne, x, y);
            std::pair<NodeId, EdgeKind> in[] = {{ne, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Bool, 0, in);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 7. No fold when operands differ.
TEST(CompareFoldGoldenTest, no_fold_different_operands) {
    EXPECT_TRUE(check_golden("CompareFold", "no_fold_different_operands",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            add_cmp(g, NodeKind::Eq, x, y);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 8. Not of a non-comparison → no fold.
TEST(CompareFoldGoldenTest, no_fold_not_non_comparison) {
    EXPECT_TRUE(check_golden("CompareFold", "no_fold_not_non_comparison",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            NodeId add = add_cmp(g, NodeKind::Add, x, y);  // not a comparison
            std::pair<NodeId, EdgeKind> in[] = {{add, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Bool, 0, in);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 9. Empty graph — no-op.
TEST(CompareFoldGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("CompareFold", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 10. x <= x → true (same as eq_self but with Lte)
TEST(CompareFoldGoldenTest, lte_self_returns_true) {
    EXPECT_TRUE(check_golden("CompareFold", "lte_self_returns_true",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_cmp(g, NodeKind::Lte, x, x);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 11. !(x <= y) → x > y
TEST(CompareFoldGoldenTest, not_lte_becomes_gt) {
    EXPECT_TRUE(check_golden("CompareFold", "not_lte_becomes_gt",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            NodeId lte = add_cmp(g, NodeKind::Lte, x, y);
            std::pair<NodeId, EdgeKind> in[] = {{lte, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Bool, 0, in);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}

// 12. !(x >= y) → x < y
TEST(CompareFoldGoldenTest, not_gte_becomes_lt) {
    EXPECT_TRUE(check_golden("CompareFold", "not_gte_becomes_lt",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            NodeId gte = add_cmp(g, NodeKind::Gte, x, y);
            std::pair<NodeId, EdgeKind> in[] = {{gte, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Bool, 0, in);
            return g;
        },
        [](Graph& g) { ComparisonFoldingPass p; p.run(g); }));
}
