// SPDX-License-Identifier: MIT
// Golden tests for the StrengthReduction pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

static NodeId add_const_sr(Graph& g, int64_t v) {
    return g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int,
                       static_cast<uint32_t>(v), {});
}

static NodeId add_binop_sr(Graph& g, NodeKind k, NodeId a, NodeId b) {
    std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}, {b, EdgeKind::Data}};
    return g.add_node(k, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                       TypeId::Int, 0, in);
}

// 1. x * 2 → x << 1
TEST(StrengthReduceGoldenTest, mul_by_2_becomes_shl_1) {
    EXPECT_TRUE(check_golden("StrengthReduce", "mul_by_2_becomes_shl_1",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId two = add_const_sr(g, 2);
            add_binop_sr(g, NodeKind::Mul, x, two);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 2. x * 4 → x << 2
TEST(StrengthReduceGoldenTest, mul_by_4_becomes_shl_2) {
    EXPECT_TRUE(check_golden("StrengthReduce", "mul_by_4_becomes_shl_2",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId four = add_const_sr(g, 4);
            add_binop_sr(g, NodeKind::Mul, x, four);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 3. x * 8 → x << 3
TEST(StrengthReduceGoldenTest, mul_by_8_becomes_shl_3) {
    EXPECT_TRUE(check_golden("StrengthReduce", "mul_by_8_becomes_shl_3",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId eight = add_const_sr(g, 8);
            add_binop_sr(g, NodeKind::Mul, x, eight);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 4. x / 2 → x >> 1
TEST(StrengthReduceGoldenTest, div_by_2_becomes_shr_1) {
    EXPECT_TRUE(check_golden("StrengthReduce", "div_by_2_becomes_shr_1",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId two = add_const_sr(g, 2);
            add_binop_sr(g, NodeKind::Div, x, two);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 5. x / 16 → x >> 4
TEST(StrengthReduceGoldenTest, div_by_16_becomes_shr_4) {
    EXPECT_TRUE(check_golden("StrengthReduce", "div_by_16_becomes_shr_4",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId sixteen = add_const_sr(g, 16);
            add_binop_sr(g, NodeKind::Div, x, sixteen);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 6. x * 3 → no reduction (3 is not a power of 2).
TEST(StrengthReduceGoldenTest, mul_by_3_no_reduction) {
    EXPECT_TRUE(check_golden("StrengthReduce", "mul_by_3_no_reduction",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId three = add_const_sr(g, 3);
            add_binop_sr(g, NodeKind::Mul, x, three);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 7. x * 0 → no reduction (0 is not positive).
TEST(StrengthReduceGoldenTest, mul_by_0_no_reduction) {
    EXPECT_TRUE(check_golden("StrengthReduce", "mul_by_0_no_reduction",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId zero = add_const_sr(g, 0);
            add_binop_sr(g, NodeKind::Mul, x, zero);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 8. x * 1 → no reduction (handled by AlgebraicSimp, not StrengthReduce).
TEST(StrengthReduceGoldenTest, mul_by_1_no_reduction) {
    EXPECT_TRUE(check_golden("StrengthReduce", "mul_by_1_no_reduction",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId one = add_const_sr(g, 1);
            add_binop_sr(g, NodeKind::Mul, x, one);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 9. Non-constant operand → no reduction.
TEST(StrengthReduceGoldenTest, non_constant_operand_no_reduction) {
    EXPECT_TRUE(check_golden("StrengthReduce", "non_constant_operand_no_reduction",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId y = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 1, {});
            add_binop_sr(g, NodeKind::Mul, x, y);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 10. Empty graph — no-op.
TEST(StrengthReduceGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("StrengthReduce", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 11. Large power of 2: x * 1024 → x << 10
TEST(StrengthReduceGoldenTest, mul_by_1024_becomes_shl_10) {
    EXPECT_TRUE(check_golden("StrengthReduce", "mul_by_1024_becomes_shl_10",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId k = add_const_sr(g, 1024);
            add_binop_sr(g, NodeKind::Mul, x, k);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}

// 12. x / 1 → no reduction (handled by AlgebraicSimp).
TEST(StrengthReduceGoldenTest, div_by_1_no_reduction) {
    EXPECT_TRUE(check_golden("StrengthReduce", "div_by_1_no_reduction",
        []() {
            Graph g;
            NodeId x = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId one = add_const_sr(g, 1);
            add_binop_sr(g, NodeKind::Div, x, one);
            return g;
        },
        [](Graph& g) { StrengthReductionPass p; p.run(g); }));
}
