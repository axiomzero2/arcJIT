// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "tier2/tier2.h"
#include "tier1/tier1.h"

using namespace arcjit;

TEST(Tier2Test, BuildDemoGraph) {
    Graph g;
    build_demo_graph(g);
    EXPECT_EQ(g.size(), 8u);
    EXPECT_TRUE(g.start().valid());
    EXPECT_TRUE(g.stop().valid());
}

TEST(Tier2Test, RunPipeline) {
    Tier2Job job;
    job.function_name = "test";
    build_demo_graph(job.graph);

    PassResult r = run_tier2_pipeline(job);
    EXPECT_TRUE(r.changed);
    EXPECT_GT(r.nodes_added, 0);

    bool found_six = false;
    for (size_t i = 1; i < job.graph.size(); ++i) {
        const Node& n = job.graph.at(NodeId{static_cast<uint32_t>(i)});
        if (n.kind == NodeKind::ConstInt && n.payload == 6) found_six = true;
    }
    EXPECT_TRUE(found_six);
}

TEST(Tier2Test, DumpDot) {
    Graph g;
    build_demo_graph(g);
    std::string dot = dump_graph_dot(g);
    EXPECT_NE(dot.find("digraph G"), std::string::npos);
    EXPECT_NE(dot.find("ConstInt"), std::string::npos);
    EXPECT_NE(dot.find("Start"), std::string::npos);
}

// Test the full Tier-1 → SoN → Tier-1 round trip.
TEST(Tier2Test, LowerAndBack) {
    // Start from a Tier-1 function: 1 + 2 + 3.
    Tier1Function fn = make_demo_add3();

    // Lower to SoN.
    Tier2Job job;
    job.function_name = fn.name;
    auto r = lower_tier1_to_son(fn, job);
    ASSERT_TRUE(r.has_value()) << r.error();

    // The graph should have a Start, 3 ConstInts, 2 Adds, and a Stop.
    EXPECT_GE(job.graph.size(), 7u);
    EXPECT_TRUE(job.graph.start().valid());
    EXPECT_TRUE(job.graph.stop().valid());

    // Run the pipeline.
    PassResult pr = run_tier2_pipeline(job);
    EXPECT_TRUE(pr.changed);

    // Lower back to Tier-1.
    auto maybe_back = lower_son_to_tier1(job);
    ASSERT_TRUE(maybe_back.has_value()) << maybe_back.error();

    // The lowered-back function should be runnable and produce 6.
    Tier1Compiler compiler;
    auto maybe_entry = compiler.compile(*maybe_back);
    ASSERT_TRUE(maybe_entry.has_value()) << maybe_entry.error();
    auto f = *maybe_entry;
    EXPECT_EQ(f(nullptr), 6);
}

// Test compile_at_tier2 — the end-to-end compilation entry point.
TEST(Tier2Test, CompileAtTier2) {
    Tier1Function fn = make_demo_add3();
    Fuse fuse;  // default budget
    auto maybe_entry = compile_at_tier2(fn, fuse);
    ASSERT_TRUE(maybe_entry.has_value()) << maybe_entry.error();
    auto f = *maybe_entry;
    EXPECT_EQ(f(nullptr), 6);
}
