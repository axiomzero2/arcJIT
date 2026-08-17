// SPDX-License-Identifier: MIT
#include "tier2/tier2.h"

#include <format>
#include <print>

namespace arcjit {

void build_demo_graph(Graph& g) {
    // Start node — the entry control node.
    NodeId start = g.add_node(NodeKind::Start,
                              NodeFlags::IsControl | NodeFlags::NoDeopt,
                              TypeId::Bottom, 0, {});
    g.set_start(start);

    // Three integer constants.
    auto pure_int_flags = NodeFlags::Pure | NodeFlags::CSEable | NodeFlags::GVNable | NodeFlags::NoDeopt;
    NodeId c1 = g.add_node(NodeKind::ConstInt, pure_int_flags, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, pure_int_flags, TypeId::Int, 2, {});
    NodeId c3 = g.add_node(NodeKind::ConstInt, pure_int_flags, TypeId::Int, 3, {});

    // First Add: c1 + c2 → result r1
    std::pair<NodeId, EdgeKind> add1_inputs[] = {
        {c1, EdgeKind::Data}, {c2, EdgeKind::Data}, {start, EdgeKind::Control},
    };
    NodeId r1 = g.add_node(NodeKind::Add,
                            NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                            TypeId::Int, 0, add1_inputs);

    // Second Add: r1 + c3 → result r2
    std::pair<NodeId, EdgeKind> add2_inputs[] = {
        {r1, EdgeKind::Data}, {c3, EdgeKind::Data}, {start, EdgeKind::Control},
    };
    NodeId r2 = g.add_node(NodeKind::Add,
                            NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                            TypeId::Int, 0, add2_inputs);

    // Stop.
    std::pair<NodeId, EdgeKind> stop_inputs[] = {
        {r2, EdgeKind::Data}, {start, EdgeKind::Control},
    };
    NodeId stop = g.add_node(NodeKind::Stop,
                              NodeFlags::IsControl | NodeFlags::NoDeopt,
                              TypeId::Bottom, 0, stop_inputs);
    g.set_stop(stop);
}

PassResult run_tier2_pipeline(Tier2Job& job) {
    // Build the standard pipeline: GVN, ConstFold, DCE.
    job.pipeline.add(std::make_unique<GVNPass>());
    job.pipeline.add(std::make_unique<ConstantFoldingPass>());
    job.pipeline.add(std::make_unique<DeadCodeElimPass>());

    // Run to fixpoint (max 8 iterations to bound work).
    return job.pipeline.run_to_fixpoint(job.graph, 8);
}

[[nodiscard]] std::string dump_graph_dot(const Graph& g) {
    return g.dump_dot();
}

}  // namespace arcjit
