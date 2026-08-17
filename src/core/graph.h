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
struct Use {
    NodeId      user;     // the node that uses the target
    uint32_t    edge_idx; // index into edges_ where this use's input lives
    EdgeKind    kind;
    uint8_t     pad[3];
};
static_assert(sizeof(Use) == 12);

// --- Graph ------------------------------------------------------------------
class Graph {
public:
    Graph() {
        // Reserve slot 0 as the invalid NodeId sentinel.
        nodes_.push_back(Node{});
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
        n.first_use    = static_cast<uint32_t>(uses_.size());  // placeholder; updated when used
        n.input_count  = static_cast<uint16_t>(inputs.size());
        n.use_count    = 0;

        for (const auto& [target, kind] : inputs) {
            edges_.push_back(Edge{target, kind, {}});
        }

        nodes_.push_back(n);

        // Register back-edges (uses) for each input.
        for (uint32_t i = 0; i < inputs.size(); ++i) {
            const auto& [target, kind] = inputs[i];
            if (!target.valid()) continue;
            // Guard against out-of-range targets (defensive — passes should
            // never create these, but the verifier needs to catch them).
            if (target.value >= nodes_.size()) continue;
            Node& producer = nodes_[target.value];
            Use  u{};
            u.user     = NodeId{id};
            u.edge_idx = eid + i;
            u.kind     = kind;
            // Update the producer's first_use pointer if it was zero.
            // We keep a per-node head pointer in `node_uses_`.
            node_use_heads_.resize(nodes_.size(), 0xFFFFFFFFu);
            u.edge_idx = eid + i;  // redundant but explicit
            // For simplicity, just push and fix head below.
            uses_.push_back(u);
            uint32_t use_idx = static_cast<uint32_t>(uses_.size() - 1);
            if (node_use_heads_[target.value] == 0xFFFFFFFFu) {
                node_use_heads_[target.value] = use_idx;
            }
            producer.use_count++;
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
    void replace_input(NodeId id, uint32_t i, NodeId new_target) {
        Node& n = nodes_[id.value];
        const uint32_t eid = n.first_input + i;
        NodeId old = edges_[eid].target;
        if (old == new_target) return;
        edges_[eid].target = new_target;

        // Decrement use count of old producer.
        if (old.valid()) {
            nodes_[old.value].use_count--;
        }
        // Increment use count of new producer.
        if (new_target.valid()) {
            nodes_[new_target.value].use_count++;
            Use u{};
            u.user     = id;
            u.edge_idx = eid;
            u.kind     = edges_[eid].kind;
            uses_.push_back(u);
        }
    }

    // Replace all uses of `old` with `new_`.
    void replace_all_uses_with(NodeId old, NodeId new_) {
        // Walk use edges pointing at `old` and rewrite them.
        for (uint32_t i = 0; i < edges_.size(); ++i) {
            if (edges_[i].target == old) edges_[i].target = new_;
        }
        if (new_.valid()) nodes_[new_.value].use_count += nodes_[old.value].use_count;
        nodes_[old.value].use_count = 0;
    }

    // Kill a node — mark dead. Removed by DCE pass.
    void mark_dead(NodeId id) {
        nodes_[id.value].flags = nodes_[id.value].flags | NodeFlags::IsDead;
    }

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
