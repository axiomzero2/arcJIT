// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "tier2/tier2.h"

using namespace arcjit;

TEST(Tier2Test, BuildDemoGraph) {
    Graph g;
    build_demo_graph(g);
    // Start + 3 ConstInt + 2 Add + Stop = 7 nodes (plus sentinel at 0 = 8)
    EXPECT_EQ(g.size(), 8u);
    EXPECT_TRUE(g.start().valid());
    EXPECT_TRUE(g.stop().valid());
}

TEST(Tier2Test, RunPipeline) {
    Tier2Job job;
    job.function_name = "test";
    build_demo_graph(job.graph);

    PassResult r = run_tier2_pipeline(job);
    // After GVN + ConstFold + DCE, the graph should have folded:
    //   1 + 2 → 3
    //   3 + 3 → 6
    // So we expect at least one ConstInt(6) node and the original Add nodes dead.
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
