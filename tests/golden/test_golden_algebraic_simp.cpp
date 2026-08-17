// SPDX-License-Identifier: MIT
// Golden tests for the AlgebraicSimplification pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

static NodeId add_const(Graph& g, int64_t v) {
    return g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int,
                       static_cast<uint32_t>(v), {});
}

static NodeId add_binop(Graph& g, NodeKind k, NodeId a, NodeId b, NodeFlags extra = NodeFlags::None) {
    std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}, {b, EdgeKind::Data}};
    return g.add_node(k, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative | extra,
                       TypeId::Int, 0, in);
}

// 1. x + 0 → x
TEST(AlgebraicSimpGoldenTest, add_zero_returns_x) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "add_zero_returns_x",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId zero = add_const(g, 0);
            add_binop(g, NodeKind::Add, x, zero);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 2. 0 + x → x
TEST(AlgebraicSimpGoldenTest, zero_add_returns_x) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "zero_add_returns_x",
        []() {
            Graph g;
            NodeId zero = add_const(g, 0);
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_binop(g, NodeKind::Add, zero, x);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 3. x - 0 → x
TEST(AlgebraicSimpGoldenTest, sub_zero_returns_x) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "sub_zero_returns_x",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId zero = add_const(g, 0);
            add_binop(g, NodeKind::Sub, x, zero);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 4. x * 0 → 0
TEST(AlgebraicSimpGoldenTest, mul_zero_returns_zero) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "mul_zero_returns_zero",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId zero = add_const(g, 0);
            add_binop(g, NodeKind::Mul, x, zero);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 5. x * 1 → x
TEST(AlgebraicSimpGoldenTest, mul_one_returns_x) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "mul_one_returns_x",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId one = add_const(g, 1);
            add_binop(g, NodeKind::Mul, x, one);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 6. 1 * x → x
TEST(AlgebraicSimpGoldenTest, one_mul_returns_x) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "one_mul_returns_x",
        []() {
            Graph g;
            NodeId one = add_const(g, 1);
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_binop(g, NodeKind::Mul, one, x);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 7. x / 1 → x
TEST(AlgebraicSimpGoldenTest, div_one_returns_x) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "div_one_returns_x",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId one = add_const(g, 1);
            add_binop(g, NodeKind::Div, x, one);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 8. x - x → 0
TEST(AlgebraicSimpGoldenTest, sub_self_returns_zero) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "sub_self_returns_zero",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_binop(g, NodeKind::Sub, x, x);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 9. No simplification when neither operand is special.
TEST(AlgebraicSimpGoldenTest, no_simplify_normal_add) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "no_simplify_normal_add",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            add_binop(g, NodeKind::Add, x, y);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 10. Empty graph — no-op.
TEST(AlgebraicSimpGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 11. 0 * x → 0 (left operand is zero)
TEST(AlgebraicSimpGoldenTest, zero_mul_returns_zero) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "zero_mul_returns_zero",
        []() {
            Graph g;
            NodeId zero = add_const(g, 0);
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            add_binop(g, NodeKind::Mul, zero, x);
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}

// 12. Multiple simplifications in one graph.
TEST(AlgebraicSimpGoldenTest, multiple_simplifications) {
    EXPECT_TRUE(check_golden("AlgebraicSimp", "multiple_simplifications",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId zero = add_const(g, 0);
            NodeId one = add_const(g, 1);
            add_binop(g, NodeKind::Add, x, zero);   // → x
            add_binop(g, NodeKind::Mul, x, one);    // → x
            return g;
        },
        [](Graph& g) { AlgebraicSimplificationPass p; p.run(g); }));
}
