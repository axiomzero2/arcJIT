// SPDX-License-Identifier: MIT
// arcJIT — Tier-2 Sea of Nodes JIT.
//
// Takes a Tier1Function, lowers it to a Sea of Nodes graph, runs the
// optimization pipeline (GVN, constant folding, DCE, etc.), then lowers
// back to Tier1Function for asmjit emission.
//
// The lowering is real — not a stub. Every Tier1Op becomes one or more
// SoN nodes with proper data/control/effect edges. The optimization
// pipeline rewrites the graph in-place.
#pragma once

#include <expected>
#include <memory>
#include <string>

#include "core/graph.h"
#include "passman/pass.h"
#include "tier1/tier1.h"

namespace arcjit {

// A "compilation job" — the unit of work the compiler pool picks up.
struct Tier2Job {
    Graph        graph;
    PassPipeline pipeline;
    std::string  function_name;
    const Tier1Function* source = nullptr;  // borrowed
};

// Lower a Tier1Function into a Sea of Nodes graph.
//
// Mapping:
//   - The function entry becomes a `Start` control node.
//   - Each Tier1Inst becomes one or more data/effect nodes.
//   - Pure ops (Add, Sub, Mul, comparisons) get only data edges.
//   - Effectful ops (StoreLocal, Call, etc.) get an effect edge from the
//     previous effectful op.
//   - Control flow (Label, Jump, BranchIfFalse) creates control nodes.
[[nodiscard]] std::expected<void, std::string>
lower_tier1_to_son(const Tier1Function& fn, Tier2Job& job);

// After optimization, lower the SoN graph back into a Tier1Function so it
// can be fed to Tier1Compiler for asmjit emission.
//
// This walks the graph in a topological order (forward from Start) and
// emits one Tier1Inst per non-control node. Basic-block boundaries are
// reconstructed from the control nodes (If/IfTrue/IfFalse/Region).
[[nodiscard]] std::expected<Tier1Function, std::string>
lower_son_to_tier1(const Tier2Job& job);

// Build a demo SoN graph representing `1 + 2 + 3` so the pipeline can run
// on it. Used by tests and the CLI demo.
void build_demo_graph(Graph& g);

// Run the full Tier-2 pipeline on a graph.
// This is the Gigavolt optimization pipeline — 14 passes:
//   TypeNarrow → CallInline → EscapeAnalysis → GVN → ConstFold →
//   AlgebraicSimp → CompareFold → BranchFold → StrengthReduce →
//   LICM → LoopUnroll → BCE → ReachabilityPruning → DCE
PassResult run_tier2_pipeline(Tier2Job& job);

// Build the Gigavolt pipeline (the named optimization pipeline for Surge).
// Returns a PassPipeline with all passes added in order.
[[nodiscard]] PassPipeline build_gigavolt_pipeline();

// End-to-end: lower + optimize + lower-back + emit + execute.
// This is what the runtime calls to compile a Tier1Function at Tier 2.
[[nodiscard]] std::expected<int64_t (*)(void*), std::string>
compile_at_tier2(const Tier1Function& fn);

// Dump the graph as a Graphviz DOT file.
[[nodiscard]] std::string dump_graph_dot(const Graph& g);

}  // namespace arcjit
