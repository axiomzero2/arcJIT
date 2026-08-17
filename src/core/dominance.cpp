// SPDX-License-Identifier: MIT
#include "core/dominance.h"

#include <algorithm>
#include <queue>

namespace arcjit {

// ============================================================================
// Dominance analysis (Cooper-Harvey-Kennedy)
// ============================================================================
//
// We compute dominators on the control-flow graph extracted from the SoN.
// Control nodes: Start, Region, If, IfTrue, IfFalse, Loop, LoopExit, Stop.
// Control edges: each control node's Control input edges.
//
// The CHK algorithm:
//   1. Compute reverse post-order of control nodes (forward DFS from Start).
//   2. Initialize idom[Start] = Start, all others = undefined.
//   3. Iterate until no changes:
//        for each node b in RPO (except Start):
//          new_idom = first processed predecessor
//          for other predecessors p:
//            if idom[p] is defined:
//              new_idom = intersect(p, new_idom)
//          if idom[b] != new_idom:
//            idom[b] = new_idom
//            changed = true

namespace {

// Get the control predecessors of a node n: the nodes that appear as
// Control inputs OF n. (Not nodes that use n as a control input.)
std::vector<NodeId> control_predecessors(const Graph& g, NodeId n) {
    std::vector<NodeId> preds;
    for (const auto& e : g.inputs_of(n)) {
        if (e.kind == EdgeKind::Control) {
            preds.push_back(e.target);
        }
    }
    return preds;
}

// Check if a node is a control-flow node.
bool is_control_node(NodeKind k) {
    switch (k) {
        case NodeKind::Start:
        case NodeKind::Region:
        case NodeKind::If:
        case NodeKind::IfTrue:
        case NodeKind::IfFalse:
        case NodeKind::Loop:
        case NodeKind::LoopExit:
        case NodeKind::Stop:
        case NodeKind::Jump:
        case NodeKind::Return:
        case NodeKind::Unreachable:
            return true;
        default:
            return false;
    }
}

// DFS from Start in post-order, collecting only control nodes.
void post_order_dfs(const Graph& g, NodeId start, std::vector<bool>& visited,
                    std::vector<NodeId>& post_order) {
    if (!start.valid() || start.value >= g.size()) return;
    if (visited[start.value]) return;
    visited[start.value] = true;

    // Visit control successors (nodes whose Control input is `start`).
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        const Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (!is_control_node(n.kind)) continue;
        for (const auto& e : g.inputs_of(id)) {
            if (e.kind == EdgeKind::Control && e.target == start) {
                post_order_dfs(g, id, visited, post_order);
                break;
            }
        }
    }

    post_order.push_back(start);
}

}  // namespace

[[nodiscard]] DominanceInfo compute_dominance(const Graph& g) {
    DominanceInfo dom;
    dom.idom.assign(g.size(), 0);
    dom.depth.assign(g.size(), 0);
    dom.reachable.assign(g.size(), false);

    if (!g.start().valid()) return dom;

    // Step 1: Compute reverse post-order of control nodes.
    std::vector<bool> visited(g.size(), false);
    std::vector<NodeId> post_order;
    post_order_dfs(g, g.start(), visited, post_order);

    // Reverse to get RPO.
    std::reverse(post_order.begin(), post_order.end());

    // Mark reachable nodes.
    for (NodeId n : post_order) {
        dom.reachable[n.value] = true;
    }

    // Map node ID → index in RPO array (for fast comparison).
    std::vector<uint32_t> rpo_index(g.size(), 0);
    for (uint32_t i = 0; i < post_order.size(); ++i) {
        rpo_index[post_order[i].value] = i;
    }

    // Step 2: Initialize.
    NodeId start = g.start();
    dom.idom[start.value] = start.value;

    // Step 3: Iterate.
    auto intersect = [&](NodeId b1, NodeId b2) -> NodeId {
        uint32_t finger1 = b1.value;
        uint32_t finger2 = b2.value;
        while (finger1 != finger2) {
            while (finger1 != 0 && rpo_index[finger1] > rpo_index[finger2]) {
                finger1 = dom.idom[finger1];
                if (finger1 == 0) break;
            }
            while (finger2 != 0 && rpo_index[finger2] > rpo_index[finger1]) {
                finger2 = dom.idom[finger2];
                if (finger2 == 0) break;
            }
            if (finger1 == 0 || finger2 == 0) break;
        }
        return NodeId{finger1};
    };

    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 100) {
        changed = false;
        iterations++;

        for (NodeId n : post_order) {
            if (n == start) continue;

            // Find predecessors with defined idom.
            auto preds = control_predecessors(g, n);
            NodeId new_idom = {};
            for (NodeId p : preds) {
                if (!dom.reachable[p.value]) continue;
                if (dom.idom[p.value] == 0) continue;  // not yet processed
                if (!new_idom.valid()) {
                    new_idom = p;
                } else {
                    new_idom = intersect(p, new_idom);
                }
            }

            if (new_idom.valid() && dom.idom[n.value] != new_idom.value) {
                dom.idom[n.value] = new_idom.value;
                changed = true;
            }
        }
    }

    // Compute depths.
    std::queue<uint32_t> worklist;
    worklist.push(start.value);
    dom.depth[start.value] = 0;
    while (!worklist.empty()) {
        uint32_t n = worklist.front();
        worklist.pop();
        for (uint32_t i = 1; i < g.size(); ++i) {
            if (i == n) continue;
            if (dom.idom[i] == n && dom.depth[i] == 0 && i != start.value) {
                dom.depth[i] = dom.depth[n] + 1;
                worklist.push(i);
            }
        }
    }

    return dom;
}

// ============================================================================
// Loop detection
// ============================================================================
//
// A loop is identified by a back-edge: an edge from node S to node H where
// H dominates S. H is the loop header. The loop body is all nodes dominated
// by H that can reach S (i.e., nodes in the cycle).

[[nodiscard]] LoopInfo compute_loops(const Graph& g, const DominanceInfo& dom) {
    LoopInfo info;

    // Find all back-edges.
    // A back-edge is a control edge from src to target where target
    // dominates src AND src can reach target (forming a cycle).
    // In a linear graph (Start → Stop), Start dominates Stop but Stop
    // cannot reach Start, so it's NOT a back-edge.
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId src{static_cast<uint32_t>(i)};
        const Node& n = g.at(src);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (!is_control_node(n.kind)) continue;

        for (const auto& e : g.inputs_of(src)) {
            if (e.kind != EdgeKind::Control) continue;
            NodeId target = e.target;
            if (!target.valid()) continue;
            // Check if target dominates src — necessary for a back-edge.
            if (!dom.dominates(target, src)) continue;
            // Also check that src != target (self-loop is OK, but not
            // a forward edge to a dominator).
            if (src == target) {
                // Self-loop — this is a back-edge.
            } else {
                // Check that src can reach target (i.e., there's a path
                // from target to src and back). We do this by checking if
                // target is reachable from src via control successors.
                // For now, we use a simple check: if target's depth in the
                // dom tree is less than src's depth AND src is in the
                // subtree of target, it might be a back-edge. But we need
                // to verify the cycle exists.
                //
                // A simpler approach: only treat it as a back-edge if
                // src has target as a control input AND target is NOT
                // the immediate dominator of src (because if it's the
                // idom, it's just a normal tree edge, not a back-edge).
                //
                // Actually, the correct check is: target dominates src,
                // and there exists a path from src back to target that
                // doesn't go through target's idom. We approximate this
                // by checking if src can reach target.
                std::vector<bool> visited(g.size(), false);
                std::queue<uint32_t> reach_q;
                // Start from src's control successors.
                for (uint32_t j = 1; j < g.size(); ++j) {
                    if (j == src.value) continue;
                    NodeId sid{static_cast<uint32_t>(j)};
                    const Node& sn = g.at(sid);
                    if (has_flag(sn.flags, NodeFlags::IsDead)) continue;
                    if (!is_control_node(sn.kind)) continue;
                    for (const auto& se : g.inputs_of(sid)) {
                        if (se.kind == EdgeKind::Control && se.target == src) {
                            reach_q.push(j);
                            visited[j] = true;
                            break;
                        }
                    }
                }
                bool found_cycle = false;
                while (!reach_q.empty() && !found_cycle) {
                    uint32_t cur = reach_q.front();
                    reach_q.pop();
                    if (cur == target.value) {
                        found_cycle = true;
                        break;
                    }
                    for (uint32_t j = 1; j < g.size(); ++j) {
                        if (visited[j]) continue;
                        NodeId sid{static_cast<uint32_t>(j)};
                        const Node& sn = g.at(sid);
                        if (has_flag(sn.flags, NodeFlags::IsDead)) continue;
                        if (!is_control_node(sn.kind)) continue;
                        for (const auto& se : g.inputs_of(sid)) {
                            if (se.kind == EdgeKind::Control && se.target.value == cur) {
                                visited[j] = true;
                                reach_q.push(j);
                                break;
                            }
                        }
                    }
                }
                if (!found_cycle) continue;
            }

            // Found a loop with header = target.
            {
                LoopInfo::Loop loop;
                loop.header = target;
                loop.back_edges.push_back(src);

                // Compute loop body: all nodes dominated by the header that
                // are reachable from the back-edge source without going
                // through the header.
                std::vector<bool> in_loop(g.size(), false);
                in_loop[target.value] = true;
                in_loop[src.value] = true;

                // Work backward from src, collecting nodes that can reach src
                // and are dominated by the header.
                std::queue<uint32_t> worklist;
                worklist.push(src.value);
                while (!worklist.empty()) {
                    uint32_t cur = worklist.front();
                    worklist.pop();
                    loop.body.push_back(NodeId{cur});

                    // Find predecessors of cur.
                    for (uint32_t j = 1; j < g.size(); ++j) {
                        if (in_loop[j]) continue;
                        NodeId pred{static_cast<uint32_t>(j)};
                        const Node& pn = g.at(pred);
                        if (has_flag(pn.flags, NodeFlags::IsDead)) continue;
                        for (const auto& pe : g.inputs_of(pred)) {
                            if (pe.kind == EdgeKind::Control && pe.target.value == cur) {
                                if (dom.dominates(target, pred) || pred == target) {
                                    in_loop[j] = true;
                                    worklist.push(j);
                                }
                                break;
                            }
                        }
                    }
                }

                info.loops.push_back(std::move(loop));
            }
        }
    }

    return info;
}

}  // namespace arcjit
