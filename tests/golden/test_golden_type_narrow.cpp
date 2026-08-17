// SPDX-License-Identifier: MIT
// Golden tests for the TypeNarrowing pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

// 1. ConstInt gets type Int.
TEST(TypeNarrowGoldenTest, const_int_gets_int_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "const_int_gets_int_type",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Top, 42, {});
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 2. ConstFloat gets type Float.
TEST(TypeNarrowGoldenTest, const_float_gets_float_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "const_float_gets_float_type",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstFloat, NodeFlags::Pure, TypeId::Top, 0, {});
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 3. Add(Int, Int) → Int.
TEST(TypeNarrowGoldenTest, add_int_int_gets_int) {
    EXPECT_TRUE(check_golden("TypeNarrow", "add_int_int_gets_int",
        []() {
            Graph g;
            NodeId a = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId b = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}, {b, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Top, 0, in);
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 4. Comparison gets type Bool.
TEST(TypeNarrowGoldenTest, comparison_gets_bool_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "comparison_gets_bool_type",
        []() {
            Graph g;
            NodeId a = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId b = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}, {b, EdgeKind::Data}};
            g.add_node(NodeKind::Lt, NodeFlags::Pure, TypeId::Top, 0, in);
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 5. Not gets type Bool.
TEST(TypeNarrowGoldenTest, not_gets_bool_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "not_gets_bool_type",
        []() {
            Graph g;
            NodeId a = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}};
            g.add_node(NodeKind::Not, NodeFlags::Pure, TypeId::Top, 0, in);
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 6. ToFloat gets type Float.
TEST(TypeNarrowGoldenTest, tofloat_gets_float_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "tofloat_gets_float_type",
        []() {
            Graph g;
            NodeId a = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}};
            g.add_node(NodeKind::ToFloat, NodeFlags::Pure, TypeId::Top, 0, in);
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 7. And/Or get type Bool.
TEST(TypeNarrowGoldenTest, and_or_get_bool_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "and_or_get_bool_type",
        []() {
            Graph g;
            NodeId a = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId b = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 0, {});
            std::pair<NodeId, EdgeKind> in[] = {{a, EdgeKind::Data}, {b, EdgeKind::Data}};
            g.add_node(NodeKind::And, NodeFlags::Pure, TypeId::Top, 0, in);
            g.add_node(NodeKind::Or,  NodeFlags::Pure, TypeId::Top, 0, in);
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 8. Empty graph — no-op.
TEST(TypeNarrowGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("TypeNarrow", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 9. ConstNull gets type Null.
TEST(TypeNarrowGoldenTest, const_null_gets_null_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "const_null_gets_null_type",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstNull, NodeFlags::Pure, TypeId::Top, 0, {});
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 10. ConstString gets type String.
TEST(TypeNarrowGoldenTest, const_string_gets_string_type) {
    EXPECT_TRUE(check_golden("TypeNarrow", "const_string_gets_string_type",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstString, NodeFlags::Pure, TypeId::Top, 0, {});
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 11. Multiple nodes narrowed in one pass.
TEST(TypeNarrowGoldenTest, multiple_nodes_narrowed) {
    EXPECT_TRUE(check_golden("TypeNarrow", "multiple_nodes_narrowed",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Top, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Top, 2, {});
            std::pair<NodeId, EdgeKind> add_in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Top, 0, add_in);
            std::pair<NodeId, EdgeKind> cmp_in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Lt, NodeFlags::Pure, TypeId::Top, 0, cmp_in);
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}

// 12. Already-correct types are preserved.
TEST(TypeNarrowGoldenTest, preserves_correct_types) {
    EXPECT_TRUE(check_golden("TypeNarrow", "preserves_correct_types",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
            return g;
        },
        [](Graph& g) { TypeNarrowingPass p; p.run(g); }));
}
