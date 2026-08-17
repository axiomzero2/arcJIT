// SPDX-License-Identifier: MIT
// Tests for the pass instrumentation system.
#include <gtest/gtest.h>

#include "core/graph.h"
#include "passman/instrument.h"
#include "passman/pass.h"

using namespace arcjit;

TEST(InstrumentationTest, records_pass_begin_end_events) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 7, {});

    auto& inst = global_instrumentation();
    inst.clear();
    inst.set_enabled(true);

    PassPipeline pipe;
    pipe.add(std::make_unique<GVNPass>());
    pipe.run_once(g);

    auto& events = inst.events();
    ASSERT_GE(events.size(), 2u);

    bool found_begin = false, found_end = false;
    for (const auto& e : events) {
        if (e.pass_name == "GVN") {
            if (e.type == PassEventType::PassBegin) found_begin = true;
            if (e.type == PassEventType::PassEnd)   found_end   = true;
        }
    }
    EXPECT_TRUE(found_begin);
    EXPECT_TRUE(found_end);
}

TEST(InstrumentationTest, disabled_records_nothing) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
    auto& inst = global_instrumentation();
    inst.clear();
    inst.set_enabled(false);

    PassPipeline pipe;
    pipe.add(std::make_unique<GVNPass>());
    pipe.run_once(g);

    EXPECT_TRUE(inst.events().empty());
}

TEST(InstrumentationTest, summarize_aggregates_stats) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});

    auto& inst = global_instrumentation();
    inst.clear();
    inst.set_enabled(true);

    PassPipeline pipe;
    pipe.add(std::make_unique<GVNPass>());
    pipe.add(std::make_unique<DeadCodeElimPass>());
    pipe.run_once(g);

    auto stats = inst.summarize();
    ASSERT_EQ(stats.size(), 2u);
    for (const auto& s : stats) {
        EXPECT_GE(s.begin_ns, 0u);
        EXPECT_GE(s.end_ns, s.begin_ns);
    }
}

TEST(InstrumentationTest, pipeline_records_all_passes) {
    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 2, {});

    auto& inst = global_instrumentation();
    inst.clear();
    inst.set_enabled(true);

    PassPipeline pipe;
    pipe.add(std::make_unique<GVNPass>());
    pipe.add(std::make_unique<ConstantFoldingPass>());
    pipe.add(std::make_unique<DeadCodeElimPass>());
    pipe.run_once(g);

    EXPECT_GE(inst.events().size(), 6u);

    auto stats = inst.summarize();
    EXPECT_EQ(stats.size(), 3u);
}

TEST(InstrumentationTest, clear_resets_events) {
    auto& inst = global_instrumentation();
    inst.set_enabled(true);

    Graph g;
    g.add_node(NodeKind::ConstInt, NodeFlags::Pure | NodeFlags::GVNable, TypeId::Int, 1, {});
    PassPipeline pipe;
    pipe.add(std::make_unique<GVNPass>());
    pipe.run_once(g);

    EXPECT_FALSE(inst.events().empty());
    inst.clear();
    EXPECT_TRUE(inst.events().empty());
}
