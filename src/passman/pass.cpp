// SPDX-License-Identifier: MIT
#include "passman/pass.h"

#include <unordered_map>

namespace arcjit {

// --- DCE -------------------------------------------------------------------
PassResult DeadCodeElimPass::run(Graph& g) {
    PassResult r;
    // Walk all nodes; any with use_count == 0 and Pure flag → mark dead.
    // (A real implementation would walk in reverse post-order to handle
    // chains of dead pure nodes.)
    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        const Node& n = g.at(id);
        if (n.use_count == 0 && has_flag(n.flags, NodeFlags::Pure) && !has_flag(n.flags, NodeFlags::IsDead)) {
            g.mark_dead(id);
            r.nodes_removed++;
            r.changed = true;
        }
    }
    return r;
}

// --- Constant folding ------------------------------------------------------
PassResult ConstantFoldingPass::run(Graph& g) {
    PassResult r;

    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;

        // Only fold pure binary ops with two ConstInt inputs.
        if (n.kind != NodeKind::Add && n.kind != NodeKind::Sub && n.kind != NodeKind::Mul) continue;

        // Filter to data edges only — nodes may also have control/effect edges
        // that should NOT be considered as operands.
        auto inputs = g.inputs_of(id);
        NodeId data_inputs[2] = {{}, {}};
        int data_count = 0;
        for (const auto& e : inputs) {
            if (e.kind == EdgeKind::Data) {
                if (data_count < 2) data_inputs[data_count] = e.target;
                data_count++;
            }
        }
        if (data_count != 2) continue;
        if (!data_inputs[0].valid() || !data_inputs[1].valid()) continue;

        const Node& a = g.at(data_inputs[0]);
        const Node& b = g.at(data_inputs[1]);
        if (a.kind != NodeKind::ConstInt || b.kind != NodeKind::ConstInt) continue;

        int64_t av = static_cast<int64_t>(a.payload);
        int64_t bv = static_cast<int64_t>(b.payload);
        int64_t result = 0;
        switch (n.kind) {
            case NodeKind::Add: result = av + bv; break;
            case NodeKind::Sub: result = av - bv; break;
            case NodeKind::Mul: result = av * bv; break;
            default: continue;
        }

        // Replace this node with a new ConstInt and rewrite all uses.
        NodeId folded = g.add_node(NodeKind::ConstInt,
                                    NodeFlags::Pure | NodeFlags::CSEable | NodeFlags::GVNable,
                                    TypeId::Int,
                                    static_cast<uint32_t>(result), {});
        g.replace_all_uses_with(id, folded);
        g.mark_dead(id);
        r.changed = true;
        r.nodes_added++;
        r.nodes_removed++;
    }
    return r;
}

// --- GVN -------------------------------------------------------------------
PassResult GVNPass::run(Graph& g) {
    PassResult r;

    // Hash key: (kind, payload, [input IDs sorted by kind|target])
    struct Key {
        NodeKind  kind;
        uint32_t  payload;
        uint64_t  h1, h2;  // hashes of input slices
        bool operator==(const Key& o) const noexcept {
            return kind == o.kind && payload == o.payload && h1 == o.h1 && h2 == o.h2;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            return std::hash<uint64_t>{}(k.h1 ^ (k.h2 << 1) ^ static_cast<uint64_t>(k.kind) * 0x9E3779B97F4A7C15ULL);
        }
    };

    std::unordered_map<Key, NodeId, KeyHash> seen;

    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        const Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (!has_flag(n.flags, NodeFlags::GVNable)) continue;

        auto inputs = g.inputs_of(id);
        uint64_t h1 = 0, h2 = 0;
        for (size_t j = 0; j < inputs.size(); ++j) {
            uint64_t v = (static_cast<uint64_t>(inputs[j].target.value) << 8) | static_cast<uint8_t>(inputs[j].kind);
            if (j < 4) h1 ^= v << (j * 8);
            else       h2 ^= v << ((j - 4) * 8);
        }

        Key k{n.kind, n.payload, h1, h2};
        auto [it, inserted] = seen.emplace(k, id);
        if (!inserted) {
            // Already saw this — replace all uses of `id` with the canonical one.
            g.replace_all_uses_with(id, it->second);
            g.mark_dead(id);
            r.nodes_removed++;
            r.changed = true;
        }
    }
    return r;
}

}  // namespace arcjit
