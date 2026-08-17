// SPDX-License-Identifier: MIT
// Golden tests for the GVN pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include <filesystem>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

// 1. Two identical constants → one survives.
TEST(GVNGoldenTest, deduplicates_identical_constants) {
    EXPECT_TRUE(check_golden("GVN", "deduplicates_identical_constants",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 2. Two identical Add nodes (same inputs) → one survives.
TEST(GVNGoldenTest, deduplicates_identical_add) {
    EXPECT_TRUE(check_golden("GVN", "deduplicates_identical_add",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in1[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in1);
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in1);
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 3. Non-GVNable nodes are NOT deduplicated.
TEST(GVNGoldenTest, does_not_deduplicate_non_gvnable) {
    EXPECT_TRUE(check_golden("GVN", "does_not_deduplicate_non_gvnable",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 4. Commutative ops with swapped inputs deduplicate.
TEST(GVNGoldenTest, commutative_swapped_inputs_deduplicate) {
    EXPECT_TRUE(check_golden("GVN", "commutative_swapped_inputs_deduplicate",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in1[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            std::pair<NodeId, EdgeKind> in2[] = {{c2, EdgeKind::Data}, {c1, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in1);
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in2);
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 5. Three-way deduplication.
TEST(GVNGoldenTest, three_way_deduplication) {
    EXPECT_TRUE(check_golden("GVN", "three_way_deduplication",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 99, {});
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 99, {});
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 99, {});
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 6. Different payloads → no dedup.
TEST(GVNGoldenTest, different_payloads_no_dedup) {
    EXPECT_TRUE(check_golden("GVN", "different_payloads_no_dedup",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 7. Dedup across different node kinds (Add vs Sub) — no dedup.
TEST(GVNGoldenTest, different_kinds_no_dedup) {
    EXPECT_TRUE(check_golden("GVN", "different_kinds_no_dedup",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in);
            g.add_node(NodeKind::Sub, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 0, in);
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 8. Single node — no-op.
TEST(GVNGoldenTest, single_node_noop) {
    EXPECT_TRUE(check_golden("GVN", "single_node_noop",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 5, {});
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 9. Empty graph — no-op.
TEST(GVNGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("GVN", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 10. Dedup in a realistic function (Start + consts + Add + Stop).
TEST(GVNGoldenTest, dedup_in_realistic_function) {
    EXPECT_TRUE(check_golden("GVN", "dedup_in_realistic_function",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});
            std::pair<NodeId, EdgeKind> add_in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}, {start, EdgeKind::Control}};
            NodeId add = g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                                     TypeId::Int, 0, add_in);
            std::pair<NodeId, EdgeKind> stop_in[] = {{add, EdgeKind::Data}, {start, EdgeKind::Control}};
            g.set_stop(g.add_node(NodeKind::Stop, NodeFlags::IsControl, TypeId::Bottom, 0, stop_in));
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 11. Dedup chain: three identical Adds.
TEST(GVNGoldenTest, dedup_chain_three_identical_adds) {
    EXPECT_TRUE(check_golden("GVN", "dedup_chain_three_identical_adds",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in);
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in);
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative, TypeId::Int, 0, in);
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}

// 12. Dedup with a dead node present.
TEST(GVNGoldenTest, dedup_with_dead_node_present) {
    EXPECT_TRUE(check_golden("GVN", "dedup_with_dead_node_present",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 3, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 3, {});
            g.mark_dead(c2);
            return g;
        },
        [](Graph& g) { GVNPass p; p.run(g); }));
}
