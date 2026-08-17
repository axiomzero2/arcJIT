// SPDX-License-Identifier: MIT
// Golden tests for the LICM pass (Rule 37: ≥10 per pass).
//
// LICM currently detects loops via explicit Loop nodes. When no Loop nodes
// are present, LICM is a no-op. These tests verify both the no-op case and
// the loop-present case.
#include <gtest/gtest.h>

#include "core/ir_dump.h"
#include "passman/pass.h"
#include "golden.h"

using namespace arcjit;

// 1. No loop nodes → no-op.
TEST(LICMGoldenTest, no_loops_is_noop) {
    EXPECT_TRUE(check_golden("LICM", "no_loops_is_noop",
        []() {
            Graph g;
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> in[] = {{c1, EdgeKind::Data}, {c2, EdgeKind::Data}};
            g.add_node(NodeKind::Add, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 0, in);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 2. Empty graph → no-op.
TEST(LICMGoldenTest, empty_graph_noop) {
    EXPECT_TRUE(check_golden("LICM", "empty_graph_noop",
        []() { return Graph{}; },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 3. Loop node present but no loop-invariant code → no-op.
TEST(LICMGoldenTest, loop_present_no_invariant_code) {
    EXPECT_TRUE(check_golden("LICM", "loop_present_no_invariant_code",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> loop_in[] = {{start, EdgeKind::Control}};
            g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, loop_in);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 4. Single loop node → no crash, no change.
TEST(LICMGoldenTest, single_loop_node_no_crash) {
    EXPECT_TRUE(check_golden("LICM", "single_loop_node_no_crash",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> in[] = {{start, EdgeKind::Control}};
            g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, in);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 5. Multiple Loop nodes → no crash.
TEST(LICMGoldenTest, multiple_loops_no_crash) {
    EXPECT_TRUE(check_golden("LICM", "multiple_loops_no_crash",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> in1[] = {{start, EdgeKind::Control}};
            g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, in1);
            std::pair<NodeId, EdgeKind> in2[] = {{start, EdgeKind::Control}};
            g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, in2);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 6. Dead Loop node → skipped.
TEST(LICMGoldenTest, dead_loop_node_skipped) {
    EXPECT_TRUE(check_golden("LICM", "dead_loop_node_skipped",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> in[] = {{start, EdgeKind::Control}};
            NodeId loop = g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, in);
            g.mark_dead(loop);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 7. Loop with pure arithmetic inside → no change (conservative).
TEST(LICMGoldenTest, loop_with_pure_arith_no_change) {
    EXPECT_TRUE(check_golden("LICM", "loop_with_pure_arith_no_change",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> loop_in[] = {{start, EdgeKind::Control}};
            NodeId loop = g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, loop_in);
            NodeId c1 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            NodeId c2 = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 2, {});
            std::pair<NodeId, EdgeKind> add_in[] = {
                {c1, EdgeKind::Data}, {c2, EdgeKind::Data}, {loop, EdgeKind::Control}
            };
            g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, add_in);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 8. Loop with LoopExit → no crash.
TEST(LICMGoldenTest, loop_with_loop_exit) {
    EXPECT_TRUE(check_golden("LICM", "loop_with_loop_exit",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> loop_in[] = {{start, EdgeKind::Control}};
            NodeId loop = g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, loop_in);
            std::pair<NodeId, EdgeKind> exit_in[] = {{loop, EdgeKind::Control}};
            g.add_node(NodeKind::LoopExit, NodeFlags::IsControl, TypeId::Bottom, 0, exit_in);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 9. Idempotent — running twice produces same result.
TEST(LICMGoldenTest, idempotent_run_twice) {
    EXPECT_TRUE(check_golden("LICM", "idempotent_run_twice",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> in[] = {{start, EdgeKind::Control}};
            g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, in);
            return g;
        },
        [](Graph& g) {
            LICMPass p;
            p.run(g);
            p.run(g);
        }));
}

// 10. Loop with effectful node inside → no hoisting (conservative).
TEST(LICMGoldenTest, loop_with_effectful_node_no_hoist) {
    EXPECT_TRUE(check_golden("LICM", "loop_with_effectful_node_no_hoist",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> loop_in[] = {{start, EdgeKind::Control}};
            NodeId loop = g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, loop_in);
            // Effectful StoreLocal inside the loop.
            NodeId c = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 0, {});
            std::pair<NodeId, EdgeKind> store_in[] = {
                {loop, EdgeKind::Control}, {loop, EdgeKind::Effect}, {c, EdgeKind::Data}
            };
            g.add_node(NodeKind::StoreLocal, NodeFlags::IsEffect, TypeId::Bottom, 0, store_in);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 11. Nested loops → no crash.
TEST(LICMGoldenTest, nested_loops_no_crash) {
    EXPECT_TRUE(check_golden("LICM", "nested_loops_no_crash",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> in1[] = {{start, EdgeKind::Control}};
            NodeId outer = g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, in1);
            std::pair<NodeId, EdgeKind> in2[] = {{outer, EdgeKind::Control}};
            g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, in2);
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}

// 12. Loop with dead body → no crash.
TEST(LICMGoldenTest, loop_with_dead_body) {
    EXPECT_TRUE(check_golden("LICM", "loop_with_dead_body",
        []() {
            Graph g;
            NodeId start = g.add_node(NodeKind::Start, NodeFlags::IsControl, TypeId::Bottom, 0, {});
            g.set_start(start);
            std::pair<NodeId, EdgeKind> loop_in[] = {{start, EdgeKind::Control}};
            NodeId loop = g.add_node(NodeKind::Loop, NodeFlags::IsControl, TypeId::Bottom, 0, loop_in);
            NodeId c = g.add_node(NodeKind::ConstInt, NodeFlags::Pure, TypeId::Int, 1, {});
            std::pair<NodeId, EdgeKind> add_in[] = {{c, EdgeKind::Data}, {loop, EdgeKind::Control}};
            NodeId add = g.add_node(NodeKind::Add, NodeFlags::Pure, TypeId::Int, 0, add_in);
            g.mark_dead(add);  // body is dead
            return g;
        },
        [](Graph& g) { LICMPass p; p.run(g); }));
}
