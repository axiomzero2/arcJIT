// SPDX-License-Identifier: MIT
// Golden tests for the BoundsCheckElimination pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

static NodeId add_const_bce(Graph& g, int64_t v) {
    return g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int,
                       static_cast<uint64_t>(v), {});
}

static NodeId add_check_bounds(Graph& g, NodeId idx, NodeId len) {
    std::pair<NodeId, EdgeKind> in[] = {{idx, EdgeKind::Data}, {len, EdgeKind::Data}};
    return g.add_node(NodeKind::CheckBounds, NodeFlags::IsGuard, TypeId::Bool, 0, in);
}

// 1. Constant index 3, length 10 → in bounds, check removed.
TEST(BCEGoldenTest, constant_index_in_bounds_removed) {
    EXPECT_TRUE(check_golden("BCE", "constant_index_in_bounds_removed",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, 3);
            NodeId len = add_const_bce(g, 10);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 2. Constant index 15, length 10 → out of bounds, check kept.
TEST(BCEGoldenTest, constant_index_out_of_bounds_kept) {
    EXPECT_TRUE(check_golden("BCE", "constant_index_out_of_bounds_kept",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, 15);
            NodeId len = add_const_bce(g, 10);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 3. Index 0, any length → always in bounds.
TEST(BCEGoldenTest, zero_index_always_in_bounds) {
    EXPECT_TRUE(check_golden("BCE", "zero_index_always_in_bounds",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, 0);
            NodeId len = add_const_bce(g, 100);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 4. Negative index → out of bounds, check kept.
TEST(BCEGoldenTest, negative_index_kept) {
    EXPECT_TRUE(check_golden("BCE", "negative_index_kept",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, static_cast<uint64_t>(-1));  // -1 as uint64
            NodeId len = add_const_bce(g, 10);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 5. Index == length → out of bounds (boundary).
TEST(BCEGoldenTest, index_equals_length_kept) {
    EXPECT_TRUE(check_golden("BCE", "index_equals_length_kept",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, 10);
            NodeId len = add_const_bce(g, 10);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 6. Index == length - 1 → in bounds (boundary).
TEST(BCEGoldenTest, index_length_minus_one_in_bounds) {
    EXPECT_TRUE(check_golden("BCE", "index_length_minus_one_in_bounds",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, 9);
            NodeId len = add_const_bce(g, 10);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 7. Non-constant index → check kept (conservative).
TEST(BCEGoldenTest, non_constant_index_kept) {
    EXPECT_TRUE(check_golden("BCE", "non_constant_index_kept",
        []() {
            Graph g;
            NodeId idx = g.add_node(NodeKind::Parameter, NodeFlags::None, TypeId::Int, 0, {});
            NodeId len = add_const_bce(g, 10);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 8. Empty graph → no-op.
TEST(BCEGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("BCE", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 9. No CheckBounds nodes → no-op.
TEST(BCEGoldenTest, no_check_bounds_noop) {
    EXPECT_TRUE(check_golden("BCE", "no_check_bounds_noop",
        []() {
            Graph g;
            add_const_bce(g, 1);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 10. Large constant index in bounds.
TEST(BCEGoldenTest, large_constant_index_in_bounds) {
    EXPECT_TRUE(check_golden("BCE", "large_constant_index_in_bounds",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, 999);
            NodeId len = add_const_bce(g, 1000);
            add_check_bounds(g, idx, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 11. Multiple CheckBounds — some removed, some kept.
TEST(BCEGoldenTest, mixed_checks_some_removed_some_kept) {
    EXPECT_TRUE(check_golden("BCE", "mixed_checks_some_removed_some_kept",
        []() {
            Graph g;
            NodeId idx1 = add_const_bce(g, 3);   // in bounds
            NodeId idx2 = add_const_bce(g, 15);  // out of bounds
            NodeId len = add_const_bce(g, 10);
            add_check_bounds(g, idx1, len);
            add_check_bounds(g, idx2, len);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}

// 12. Dead CheckBounds → skipped.
TEST(BCEGoldenTest, dead_check_bounds_skipped) {
    EXPECT_TRUE(check_golden("BCE", "dead_check_bounds_skipped",
        []() {
            Graph g;
            NodeId idx = add_const_bce(g, 3);
            NodeId len = add_const_bce(g, 10);
            NodeId cb = add_check_bounds(g, idx, len);
            g.mark_dead(cb);
            return g;
        },
        [](Graph& g) { BoundsCheckEliminationPass p; p.run(g); }));
}
