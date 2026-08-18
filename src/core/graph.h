// SPDX-License-Identifier: MIT
// arcJIT — Sea of Nodes graph container.
//
// Per docs/ARCHITECTURE.md §3.5, the graph is a flat `std::vector<Node>` with
// a separate edge pool (for inputs) and use pool (for reverse edges). All
// long-lived references use `NodeId`, never raw `Node*`.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "arena.h"
#include "node.h"

namespace arcjit {

// Input slices are tagged with this kind so passes can filter control/effect/data edges.
enum class EdgeKind : uint8_t {
    Data        = 0,
    Control     = 1,
    Effect      = 2,
    FrameState  = 3,
};

// An edge in the pool. We use 8 bytes per edge so the pool is trivially
// traversable.
struct Edge {
    NodeId      target;
    EdgeKind    kind;
    uint8_t     pad[3];  // align to 8
};
static_assert(sizeof(Edge) == 8);

// A back-edge in the use pool.
//
// The use pool is an intrusive singly-linked list per producer node.
// `node_use_heads_[producer]` is the index of the first Use (or 0xFFFFFFFF
// if no uses). Each Use has a `next_use` index chaining to the next Use of
// the same producer.
//
// This makes `replace_all_uses_with(old, new_)` O(uses_of_old) instead of
// O(total_edges). Critical: the SoN pipeline calls this in every rewriting
// pass (LocalForwarding, GVN, ConstFold, BranchFold, DCE, etc.) — the
// previous O(E) scan made every pass O(N²).
struct Use {
    NodeId      user;       // the node that uses the target
    uint32_t    edge_idx;   // index into edges_ where this use's input lives
    EdgeKind    kind;
    uint8_t     pad[3];
    uint32_t    next_use;   // index of next Use of the same producer, or 0xFFFFFFFF
};
static_assert(sizeof(Use) == 16);

// --- Graph ------------------------------------------------------------------
class Graph {
public:
    Graph() {
        // Reserve slot 0 as the invalid NodeId sentinel.
        nodes_.push_back(Node{});
        // Mirror the sentinel in node_use_heads_ so indices stay 1:1
        // with nodes_. (Without this, node_use_heads_[i] doesn't correspond
        // to nodes_[i] — a latent bug that segfaults the first time we
        // call replace_all_uses_with on a node whose ID exceeds the
        // heads vector size.)
        node_use_heads_.push_back(0xFFFFFFFFu);
    }
    ~Graph() = default;

    Graph(const Graph&)            = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&)                 = default;
    Graph& operator=(Graph&&)      = default;

    // --- Node construction ---------------------------------------------------
    //
    // Add a node of kind `k`, with `flags`, `type`, `payload`, and a list of
    // inputs. Inputs is a span of (NodeId, EdgeKind) pairs.
    //
    // Returns the new NodeId.
    NodeId add_node(NodeKind k, NodeFlags flags, TypeId type, uint64_t payload,
                    std::span<const std::pair<NodeId, EdgeKind>> inputs = {}) {
        const uint32_t id  = static_cast<uint32_t>(nodes_.size());
        const uint32_t eid = static_cast<uint32_t>(edges_.size());

        Node n{};
        n.kind         = k;
        n.flags        = flags;
        n.type         = type;
        n.payload      = payload;
        n.first_input  = eid;
        n.first_use    = 0xFFFFFFFFu;  // no uses yet
        n.input_count  = static_cast<uint16_t>(inputs.size());
        n.use_count    = 0;

        for (const auto& [target, kind] : inputs) {
            edges_.push_back(Edge{target, kind, {}});
        }

        nodes_.push_back(n);
        node_use_heads_.push_back(0xFFFFFFFFu);

        // Register back-edges (uses) for each input. We push each Use to the
        // HEAD of the producer's use list (O(1) — no tail traversal needed).
        for (uint32_t i = 0; i < inputs.size(); ++i) {
            const auto& [target, kind] = inputs[i];
            if (!target.valid()) continue;
            if (target.value >= nodes_.size()) continue;

            Use u{};
            u.user     = NodeId{id};
            u.edge_idx = eid + i;
            u.kind     = kind;
            u.next_use = node_use_heads_[target.value];  // link to old head
            uint32_t use_idx = static_cast<uint32_t>(uses_.size());
            uses_.push_back(u);
            node_use_heads_[target.value] = use_idx;
            nodes_[target.value].use_count++;
        }

        return NodeId{id};
    }

    // --- Accessors ----------------------------------------------------------
    [[nodiscard]] const Node& at(NodeId id) const noexcept { return nodes_[id.value]; }
    [[nodiscard]] Node&       at(NodeId id)       noexcept { return nodes_[id.value]; }
    [[nodiscard]] size_t      size() const noexcept { return nodes_.size(); }

    [[nodiscard]] NodeId start() const noexcept { return start_; }
    [[nodiscard]] NodeId stop()  const noexcept { return stop_; }

    void set_start(NodeId n) noexcept { start_ = n; }
    void set_stop(NodeId n)  noexcept { stop_  = n; }

    // --- Edge iteration -----------------------------------------------------
    [[nodiscard]] std::span<const Edge> inputs_of(NodeId id) const noexcept {
        const Node& n = nodes_[id.value];
        if (n.input_count == 0) return {};
        return {&edges_[n.first_input], n.input_count};
    }

    // Filter inputs by kind.
    [[nodiscard]] std::vector<NodeId> inputs_of_kind(NodeId id, EdgeKind kind) const {
        std::vector<NodeId> out;
        for (const auto& e : inputs_of(id)) {
            if (e.kind == kind) out.push_back(e.target);
        }
        return out;
    }

    // --- Mutation -----------------------------------------------------------
    //
    // Replace one input edge of `id` at position `i` with `new_target`.
    // Updates use lists on both old and new producers.
    //
    // NOTE: we don't remove the stale Use from the old producer's list
    // (would be O(uses_of_old) to find it). Instead we leave the stale Use
    // in place — `replace_all_uses_with` walks the list and skips any Use
    // whose edge no longer points at the producer. This is sound and keeps
    // `replace_input` O(1).
    void replace_input(NodeId id, uint32_t i, NodeId new_target) {
        Node& n = nodes_[id.value];
        const uint32_t eid = n.first_input + i;
        NodeId old = edges_[eid].target;
        if (old == new_target) return;
        edges_[eid].target = new_target;

        if (old.valid()) {
            nodes_[old.value].use_count--;
        }
        if (new_target.valid()) {
            nodes_[new_target.value].use_count++;
            Use u{};
            u.user     = id;
            u.edge_idx = eid;
            u.kind     = edges_[eid].kind;
            u.next_use = node_use_heads_[new_target.value];
            uint32_t use_idx = static_cast<uint32_t>(uses_.size());
            uses_.push_back(u);
            node_use_heads_[new_target.value] = use_idx;
        }
    }

    // Replace all uses of `old` with `new_`.
    //
    // Walks the intrusive use list of `old` (O(use_count of old) — NOT
    // O(total edges)). For each Use, we patch the corresponding edge in
    // `edges_` to point at `new_`, and prepend a fresh Use to `new_`'s
    // list. We then splice the entire chain onto `new_`'s head and clear
    // `old`'s head — this preserves the linked-list invariant without
    // walking `new_`'s existing list.
    //
    // Stale Uses (from prior replace_input calls whose edge was later
    // re-patched) are filtered by re-checking `edges_[edge_idx].target`
    // against `old` before counting them.
    void replace_all_uses_with(NodeId old, NodeId new_) {
        uint32_t use_idx = node_use_heads_[old.value];
        node_use_heads_[old.value] = 0xFFFFFFFFu;

        uint32_t live_uses = 0;
        uint32_t chain_head = 0xFFFFFFFFu;
        uint32_t chain_tail = 0xFFFFFFFFu;

        while (use_idx != 0xFFFFFFFFu) {
            Use& u = uses_[use_idx];
            uint32_t next = u.next_use;

            // Verify this Use is still live — replace_input may have
            // re-patched the edge to point elsewhere.
            if (u.edge_idx < edges_.size() && edges_[u.edge_idx].target == old) {
                edges_[u.edge_idx].target = new_;
                live_uses++;

                u.next_use = 0xFFFFFFFFu;
                if (chain_head == 0xFFFFFFFFu) {
                    chain_head = use_idx;
                } else {
                    uses_[chain_tail].next_use = use_idx;
                }
                chain_tail = use_idx;
            }

            use_idx = next;
        }

        // Splice the live-use chain onto new_'s use list head.
        if (chain_head != 0xFFFFFFFFu) {
            uses_[chain_tail].next_use = node_use_heads_[new_.value];
            node_use_heads_[new_.value] = chain_head;
        }

        if (new_.valid()) nodes_[new_.value].use_count += live_uses;
        nodes_[old.value].use_count = 0;
    }

    // Kill a node — mark dead. Removed by DCE pass.
    void mark_dead(NodeId id) {
        nodes_[id.value].flags = nodes_[id.value].flags | NodeFlags::IsDead;
    }

    // --- Edge access (for passes that need to iterate edges directly) --------
    [[nodiscard]] std::span<const Edge> all_edges() const noexcept { return edges_; }
    [[nodiscard]] std::span<Edge> all_edges() noexcept { return edges_; }

    // --- IR dump ------------------------------------------------------------
    [[nodiscard]] std::string dump_dot() const;

private:
    std::vector<Node>     nodes_;            // index 0 = invalid sentinel
    std::vector<Edge>     edges_;            // input edges
    std::vector<Use>      uses_;             // back-edges (uses_)
    std::vector<uint32_t> node_use_heads_;   // head of per-node use list
    NodeId                start_ = {};
    NodeId                stop_  = {};
};

}  // namespace arcjit
