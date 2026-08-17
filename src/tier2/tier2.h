// SPDX-License-Identifier: MIT
// arcJIT — Tier-2 Sea of Nodes JIT.
//
// Per docs/ARCHITECTURE.md §1.3 and §3, Tier 2 takes the Tier-1 SSA graph +
// Tier-1 profiles and lowers it to a Sea of Nodes graph. It then runs the
// optimization pipeline (GVN, escape analysis, GCM, OSR) and lowers back to
// machine code.
//
// For the scaffold, Tier 2 builds a small SoN graph from scratch (using the
// core Graph container), runs the GVN + DCE + constant-folding pipeline on
// it, and dumps the resulting graph. A full Tier 2 would consume the
// Tier-1 function as input and lower it to asmjit-emitted machine code.
#pragma once

#include <memory>
#include <string>

#include "core/graph.h"
#include "passman/pass.h"

namespace arcjit {

// A "compilation job" — the unit of work the compiler pool picks up.
struct Tier2Job {
    Graph                  graph;
    PassPipeline           pipeline;
    std::string            function_name;
};

// Build a demo SoN graph representing `1 + 2 + 3` so the pipeline can run
// on it. Returns a graph with:
//   Start
//   ConstInt 1, ConstInt 2, ConstInt 3
//   Add(1, 2) → Add(_, 3) → Stop
void build_demo_graph(Graph& g);

// Run the full Tier-2 pipeline on a graph.
PassResult run_tier2_pipeline(Tier2Job& job);

// Dump the graph as a Graphviz DOT file.
[[nodiscard]] std::string dump_graph_dot(const Graph& g);

}  // namespace arcjit
