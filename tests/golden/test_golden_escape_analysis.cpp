// SPDX-License-Identifier: MIT
// Golden tests for the EscapeAnalysis pass (Rule 37: ≥10 per pass).
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

static NodeId make_alloc(Graph& g) {
    return g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0,
                       std::initializer_list<std::pair<NodeId, EdgeKind>>{});
}

// 1. Allocate used only by LoadField → non-escaping.
TEST(EscapeAnalysisGoldenTest, alloc_used_by_loadfield_is_non_escaping) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "alloc_used_by_loadfield_is_non_escaping",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> alloc_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, alloc_in);
            std::pair<NodeId, EdgeKind> load_in[] = {
                {alloc, EdgeKind::Data}, {start, EdgeKind::Control}, {alloc, EdgeKind::Effect}
            };
            g.add_node(NodeKind::LoadField, NodeFlags::IsEffect, TypeId::Int, 0, load_in);
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 2. Allocate used by Return → escapes.
TEST(EscapeAnalysisGoldenTest, alloc_used_by_return_escapes) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "alloc_used_by_return_escapes",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> alloc_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, alloc_in);
            std::pair<NodeId, EdgeKind> stop_in[] = {
                {alloc, EdgeKind::Data}, {start, EdgeKind::Control}, {alloc, EdgeKind::Effect}
            };
            g.set_stop(g.add_node(NodeKind::Stop, NodeFlags::IsControl, TypeId::Bottom, 0, stop_in));
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 3. Allocate used by Call → escapes.
TEST(EscapeAnalysisGoldenTest, alloc_used_by_call_escapes) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "alloc_used_by_call_escapes",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> alloc_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, alloc_in);
            std::pair<NodeId, EdgeKind> call_in[] = {
                {alloc, EdgeKind::Data}, {start, EdgeKind::Control}, {alloc, EdgeKind::Effect}
            };
            g.add_node(NodeKind::Call, NodeFlags::IsEffect, TypeId::Top, 0, call_in);
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 4. Allocate used by StoreField → non-escaping (storing INTO the alloc).
TEST(EscapeAnalysisGoldenTest, alloc_used_by_storefield_is_non_escaping) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "alloc_used_by_storefield_is_non_escaping",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> alloc_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, alloc_in);
            NodeId val = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 42, {});
            std::pair<NodeId, EdgeKind> store_in[] = {
                {alloc, EdgeKind::Data}, {val, EdgeKind::Data},
                {start, EdgeKind::Control}, {alloc, EdgeKind::Effect}
            };
            g.add_node(NodeKind::StoreField, NodeFlags::IsEffect, TypeId::Bottom, 0, store_in);
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 5. No Allocate nodes → no-op.
TEST(EscapeAnalysisGoldenTest, no_allocates_is_noop) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "no_allocates_is_noop",
        []() {
            Graph g;
            g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 6. Empty graph → no-op.
TEST(EscapeAnalysisGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 7. Multiple allocates, one escapes, one doesn't.
TEST(EscapeAnalysisGoldenTest, mixed_escape_allocates) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "mixed_escape_allocates",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            // alloc1: non-escaping (only LoadField)
            std::pair<NodeId, EdgeKind> a1_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc1 = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, a1_in);
            std::pair<NodeId, EdgeKind> load_in[] = {
                {alloc1, EdgeKind::Data}, {start, EdgeKind::Control}, {alloc1, EdgeKind::Effect}
            };
            g.add_node(NodeKind::LoadField, NodeFlags::IsEffect, TypeId::Int, 0, load_in);
            // alloc2: escaping (returned)
            std::pair<NodeId, EdgeKind> a2_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc2 = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, a2_in);
            std::pair<NodeId, EdgeKind> stop_in[] = {
                {alloc2, EdgeKind::Data}, {start, EdgeKind::Control}, {alloc2, EdgeKind::Effect}
            };
            g.set_stop(g.add_node(NodeKind::Stop, NodeFlags::IsControl, TypeId::Bottom, 0, stop_in));
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 8. Dead Allocate → skipped.
TEST(EscapeAnalysisGoldenTest, dead_allocate_skipped) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "dead_allocate_skipped",
        []() {
            Graph g;
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0,
                                       std::initializer_list<std::pair<NodeId, EdgeKind>>{});
            g.mark_dead(alloc);
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 9. Allocate used by StoreVar → escapes (stored into global).
TEST(EscapeAnalysisGoldenTest, alloc_used_by_storevar_escapes) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "alloc_used_by_storevar_escapes",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> alloc_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, alloc_in);
            std::pair<NodeId, EdgeKind> store_in[] = {
                {alloc, EdgeKind::Data}, {start, EdgeKind::Control}, {alloc, EdgeKind::Effect}
            };
            g.add_node(NodeKind::StoreVar, NodeFlags::IsEffect, TypeId::Bottom, 0, store_in);
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 10. Allocate used by CheckShape → non-escaping.
TEST(EscapeAnalysisGoldenTest, alloc_used_by_checkshape_non_escaping) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "alloc_used_by_checkshape_non_escaping",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> alloc_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, alloc_in);
            std::pair<NodeId, EdgeKind> check_in[] = {
                {alloc, EdgeKind::Data}, {start, EdgeKind::Control}
            };
            g.add_node(NodeKind::CheckShape, NodeFlags::IsGuard, TypeId::Bool, 0, check_in);
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}

// 11. Idempotent — running twice produces same result.
TEST(EscapeAnalysisGoldenTest, idempotent_run_twice) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "idempotent_run_twice",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> alloc_in[] = {
                {start, EdgeKind::Control}, {start, EdgeKind::Effect}
            };
            NodeId alloc = g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0, alloc_in);
            std::pair<NodeId, EdgeKind> load_in[] = {
                {alloc, EdgeKind::Data}, {start, EdgeKind::Control}, {alloc, EdgeKind::Effect}
            };
            g.add_node(NodeKind::LoadField, NodeFlags::IsEffect, TypeId::Int, 0, load_in);
            return g;
        },
        [](Graph& g) {
            EscapeAnalysisPass p;
            p.run(g);
            p.run(g);
        }));
}

// 12. Allocate with no uses → non-escaping (vacuously).
TEST(EscapeAnalysisGoldenTest, alloc_no_uses_non_escaping) {
    EXPECT_TRUE(check_golden("EscapeAnalysis", "alloc_no_uses_non_escaping",
        []() {
            Graph g;
            g.add_node(NodeKind::Allocate, NodeFlags::IsEffect, TypeId::Object, 0,
                       std::initializer_list<std::pair<NodeId, EdgeKind>>{});
            return g;
        },
        [](Graph& g) { EscapeAnalysisPass p; p.run(g); }));
}
