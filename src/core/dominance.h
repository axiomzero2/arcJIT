// SPDX-License-Identifier: MIT
// arcJIT — Dominance analysis.
//
// Computes the dominator tree for a Sea of Nodes graph. A node D dominates
// node N if every path from the Start to N passes through D. The immediate
// dominator (idom) of N is the unique closest dominator.
//
// Uses the Cooper-Harvey-Kennedy algorithm (a simple, efficient iterative
// dominator computation that works on any control-flow graph).
//
// The dominator tree enables:
//   - LICM: a loop's body is the set of nodes dominated by the loop header
//   - GCM: "schedule early" places a node at its earliest legal position
//     (the deepest common dominator of its inputs)
//   - BCE: prove bounds from loop induction variables
//   - Dead code: a node not reachable from Start is dead
#pragma once

#include <cstdint>
#include <vector>

#include "core/graph.h"

namespace arcjit {

struct DominanceInfo {
    // idom[n] = immediate dominator of node n.
    // idom[start] = start (root of the tree).
    // idom[n] = 0 (invalid) if n is unreachable.
    std::vector<uint32_t> idom;

    // dominator depth — distance from the start node in the dom tree.
    std::vector<uint32_t> depth;

    // Whether node n is reachable from Start.
    std::vector<bool> reachable;

    // Find the nearest common dominator of two nodes.
    [[nodiscard]] NodeId common_dominator(NodeId a, NodeId b) const noexcept {
        if (!a.valid() || !b.valid()) return {};
        uint32_t ia = a.value, ib = b.value;
        while (ia != ib) {
            while (depth[ia] > depth[ib]) ia = idom[ia];
            while (depth[ib] > depth[ia]) ib = idom[ib];
            if (ia == ib) break;
            ia = idom[ia];
            ib = idom[ib];
            if (ia == 0 || ib == 0) return {};
        }
        return NodeId{ia};
    }

    // Does node `d` dominate node `n`?
    [[nodiscard]] bool dominates(NodeId d, NodeId n) const noexcept {
        if (!d.valid() || !n.valid()) return false;
        uint32_t cur = n.value;
        while (cur != 0 && cur < idom.size()) {
            if (cur == d.value) return true;
            if (cur == idom[cur]) break;  // root
            cur = idom[cur];
        }
        return cur == d.value;
    }
};

// Compute dominance information for a graph.
// The graph must have a valid Start node.
[[nodiscard]] DominanceInfo compute_dominance(const Graph& g);

// Detect loops in the graph by finding back-edges.
// A back-edge is an edge whose target dominates its source.
// Returns a list of (back_edge_source, loop_header) pairs.
struct LoopInfo {
    struct Loop {
        NodeId header;                    // the loop entry (back-edge target)
        std::vector<NodeId> body;         // all nodes dominated by header
        std::vector<NodeId> back_edges;   // sources of back-edges
    };
    std::vector<Loop> loops;
};

// Detect loops and compute loop bodies using dominance.
[[nodiscard]] LoopInfo compute_loops(const Graph& g, const DominanceInfo& dom);

}  // namespace arcjit
