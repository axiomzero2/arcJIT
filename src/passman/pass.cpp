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

// ============================================================================
// AlgebraicSimplificationPass
// ============================================================================
//
// Applies algebraic identities:
//   x + 0  → x
//   0 + x  → x
//   x - 0  → x
//   x * 0  → 0
//   0 * x  → 0
//   x * 1  → x
//   1 * x  → x
//   x - x  → 0
//   x / 1  → x
//   !!x    → x  (Not(Not(x)) → x)
PassResult AlgebraicSimplificationPass::run(Graph& g) {
    PassResult r;

    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;

        // Only simplify pure binary ops with two data inputs.
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
        bool a_is_const = (a.kind == NodeKind::ConstInt);
        bool b_is_const = (b.kind == NodeKind::ConstInt);
        int64_t av = static_cast<int64_t>(a.payload);
        int64_t bv = static_cast<int64_t>(b.payload);

        NodeId replacement = {};
        bool replaced = false;

        switch (n.kind) {
            case NodeKind::Add:
                // x + 0 → x, 0 + x → x
                if (b_is_const && bv == 0) { replacement = data_inputs[0]; replaced = true; }
                else if (a_is_const && av == 0) { replacement = data_inputs[1]; replaced = true; }
                break;
            case NodeKind::Sub:
                // x - 0 → x
                if (b_is_const && bv == 0) { replacement = data_inputs[0]; replaced = true; }
                // x - x → 0
                else if (data_inputs[0] == data_inputs[1]) {
                    replacement = g.add_node(NodeKind::ConstInt,
                                              NodeFlags::Pure | NodeFlags::GVNable,
                                              TypeId::Int, 0, {});
                    replaced = true;
                }
                break;
            case NodeKind::Mul:
                // x * 0 → 0, 0 * x → 0
                if ((a_is_const && av == 0) || (b_is_const && bv == 0)) {
                    replacement = g.add_node(NodeKind::ConstInt,
                                              NodeFlags::Pure | NodeFlags::GVNable,
                                              TypeId::Int, 0, {});
                    replaced = true;
                }
                // x * 1 → x, 1 * x → x
                else if (b_is_const && bv == 1) { replacement = data_inputs[0]; replaced = true; }
                else if (a_is_const && av == 1) { replacement = data_inputs[1]; replaced = true; }
                break;
            case NodeKind::Div:
                // x / 1 → x
                if (b_is_const && bv == 1) { replacement = data_inputs[0]; replaced = true; }
                break;
            default:
                break;
        }

        // !!x → x (Not(Not(x)))
        if (n.kind == NodeKind::Not && a.kind == NodeKind::Not) {
            auto inner_inputs = g.inputs_of(data_inputs[0]);
            for (const auto& e : inner_inputs) {
                if (e.kind == EdgeKind::Data) {
                    replacement = e.target;
                    replaced = true;
                    break;
                }
            }
        }

        if (replaced && replacement.valid()) {
            g.replace_all_uses_with(id, replacement);
            g.mark_dead(id);
            r.changed = true;
            r.nodes_removed++;
        }
    }
    return r;
}

// ============================================================================
// StrengthReductionPass
// ============================================================================
//
// Replaces expensive operations with cheaper ones:
//   x * 2^k  → x << k
//   x / 2^k  → x >> k  (signedness-aware — we treat all ints as signed)
//
// We don't have a ShiftLeft/ShiftRight node kind yet, so we fold these to
// ConstInt when x is also a constant, and otherwise leave them alone.
// When the IR gains shift nodes, this pass will emit them.
PassResult StrengthReductionPass::run(Graph& g) {
    PassResult r;

    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;

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

        const Node& b = g.at(data_inputs[1]);

        // Only handle Mul/Div with a power-of-2 constant operand.
        if (n.kind != NodeKind::Mul && n.kind != NodeKind::Div) continue;
        if (b.kind != NodeKind::ConstInt) continue;

        int64_t bv = static_cast<int64_t>(b.payload);
        if (bv <= 0) continue;

        // Check if bv is a power of 2.
        bool is_pow2 = (bv & (bv - 1)) == 0;
        if (!is_pow2) continue;

        // For now, we just record that this could be strength-reduced.
        // A full implementation would replace Mul(x, 2^k) with Shl(x, k).
        // Since we don't have Shl nodes yet, we skip the actual replacement
        // but mark that we found an opportunity (for future use).
        // r.changed = true;  // would set this if we actually transformed
    }
    return r;
}

// ============================================================================
// ComparisonFoldingPass
// ============================================================================
//
//   x == x → ConstInt(1)
//   x != x → ConstInt(0)
//   !(x < y)  → x >= y   (Not(Lt) → Gte)
//   !(x > y)  → x <= y   (Not(Gt) → Lte)
//   !(x <= y) → x > y    (Not(Lte) → Gt)
//   !(x >= y) → x < y    (Not(Gte) → Lt)
//   !(x == y) → x != y   (Not(Eq) → Ne)
//   !(x != y) → x == y   (Not(Ne) → Eq)
PassResult ComparisonFoldingPass::run(Graph& g) {
    PassResult r;

    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;

        auto inputs = g.inputs_of(id);
        NodeId data_inputs[2] = {{}, {}};
        int data_count = 0;
        for (const auto& e : inputs) {
            if (e.kind == EdgeKind::Data) {
                if (data_count < 2) data_inputs[data_count] = e.target;
                data_count++;
            }
        }

        // x == x → true, x != x → false
        if (data_count == 2 && data_inputs[0] == data_inputs[1]) {
            if (n.kind == NodeKind::Eq) {
                NodeId one = g.add_node(NodeKind::ConstInt,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Int, 1, {});
                g.replace_all_uses_with(id, one);
                g.mark_dead(id);
                r.changed = true;
                r.nodes_removed++;
                continue;
            }
            if (n.kind == NodeKind::Ne) {
                NodeId zero = g.add_node(NodeKind::ConstInt,
                                          NodeFlags::Pure | NodeFlags::GVNable,
                                          TypeId::Int, 0, {});
                g.replace_all_uses_with(id, zero);
                g.mark_dead(id);
                r.changed = true;
                r.nodes_removed++;
                continue;
            }
        }

        // Not(comparison) → inverted comparison
        if (n.kind == NodeKind::Not && data_count == 1) {
            const Node& inner = g.at(data_inputs[0]);
            NodeKind inverted = NodeKind::Count;
            switch (inner.kind) {
                case NodeKind::Eq:  inverted = NodeKind::Ne;  break;
                case NodeKind::Ne:  inverted = NodeKind::Eq;  break;
                case NodeKind::Lt:  inverted = NodeKind::Gte; break;
                case NodeKind::Gt:  inverted = NodeKind::Lte; break;
                case NodeKind::Lte: inverted = NodeKind::Gt;  break;
                case NodeKind::Gte: inverted = NodeKind::Lt;  break;
                default: break;
            }
            if (inverted != NodeKind::Count) {
                // Get the inner comparison's data inputs.
                auto inner_data = g.inputs_of_kind(data_inputs[0], EdgeKind::Data);
                if (inner_data.size() == 2) {
                    std::pair<NodeId, EdgeKind> new_inputs[] = {
                        {inner_data[0], EdgeKind::Data},
                        {inner_data[1], EdgeKind::Data},
                    };
                    NodeId new_cmp = g.add_node(inverted,
                                                 NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                                                 TypeId::Bool, 0, new_inputs);
                    g.replace_all_uses_with(id, new_cmp);
                    g.mark_dead(id);
                    r.changed = true;
                    r.nodes_removed++;
                    r.nodes_added++;
                }
            }
        }
    }
    return r;
}

// ============================================================================
// BranchFoldingPass
// ============================================================================
//
//   if (ConstInt(1))  → drop the IfFalse branch
//   if (ConstInt(0))  → drop the IfTrue branch
//
// We mark the dead branch's IfTrue/IfFalse as dead. DCE will clean up the
// unreachable nodes.
PassResult BranchFoldingPass::run(Graph& g) {
    PassResult r;

    for (size_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (n.kind != NodeKind::If) continue;

        // The If's data input is the condition.
        auto data = g.inputs_of_kind(id, EdgeKind::Data);
        if (data.empty()) continue;
        const Node& cond = g.at(data[0]);
        if (cond.kind != NodeKind::ConstInt) continue;

        int64_t cv = static_cast<int64_t>(cond.payload);

        // Find the IfTrue and IfFalse successors.
        for (size_t j = 1; j < g.size(); ++j) {
            NodeId sid{static_cast<uint32_t>(j)};
            Node& s = g.at(sid);
            if (has_flag(s.flags, NodeFlags::IsDead)) continue;

            if (s.kind == NodeKind::IfTrue || s.kind == NodeKind::IfFalse) {
                auto s_inputs = g.inputs_of(sid);
                if (s_inputs.size() != 1) continue;
                if (s_inputs[0].target != id) continue;

                // If cond is true (non-zero), IfFalse is dead.
                // If cond is false (zero), IfTrue is dead.
                bool cond_true = (cv != 0);
                if ((cond_true && s.kind == NodeKind::IfFalse) ||
                    (!cond_true && s.kind == NodeKind::IfTrue)) {
                    g.mark_dead(sid);
                    r.changed = true;
                    r.nodes_removed++;
                }
            }
        }
    }
    return r;
}

// ============================================================================
// TypeNarrowingPass
// ============================================================================
//
// Propagates TypeIds through the graph:
//   - ConstInt → Int
//   - ConstFloat → Float
//   - Add(Int, Int) → Int
//   - Add(Int, Float) → Float
//   - Add(Float, Float) → Float
//   - Comparisons → Bool
//   - Not(Bool) → Bool
//
// This is a forward dataflow analysis. We iterate to a fixpoint.
PassResult TypeNarrowingPass::run(Graph& g) {
    PassResult r;
    bool changed = true;
    int iterations = 0;

    while (changed && iterations < 8) {
        changed = false;
        iterations++;

        for (size_t i = 1; i < g.size(); ++i) {
            NodeId id{static_cast<uint32_t>(i)};
            Node& n = g.at(id);
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;

            TypeId old_type = n.type;
            TypeId new_type = old_type;

            switch (n.kind) {
                case NodeKind::ConstInt:
                case NodeKind::ConstUndef:
                    new_type = TypeId::Int;
                    break;
                case NodeKind::ConstFloat:
                    new_type = TypeId::Float;
                    break;
                case NodeKind::ConstNull:
                    new_type = TypeId::Null;
                    break;
                case NodeKind::ConstString:
                    new_type = TypeId::String;
                    break;
                case NodeKind::Add:
                case NodeKind::Sub:
                case NodeKind::Mul:
                case NodeKind::Div:
                case NodeKind::Pow: {
                    // Result type is the join of operand types.
                    auto data = g.inputs_of_kind(id, EdgeKind::Data);
                    TypeId joined = TypeId::Bottom;
                    for (auto d : data) {
                        TypeId t = g.at(d).type;
                        if (joined == TypeId::Bottom) joined = t;
                        else if (joined == TypeId::Int && t == TypeId::Float) joined = TypeId::Float;
                        else if (joined == TypeId::Float && t == TypeId::Int) joined = TypeId::Float;
                    }
                    if (joined != TypeId::Bottom) new_type = joined;
                    break;
                }
                case NodeKind::Eq:
                case NodeKind::Ne:
                case NodeKind::Lt:
                case NodeKind::Gt:
                case NodeKind::Lte:
                case NodeKind::Gte:
                case NodeKind::And:
                case NodeKind::Or:
                case NodeKind::Not:
                case NodeKind::ToBool:
                    new_type = TypeId::Bool;
                    break;
                case NodeKind::ToFloat:
                    new_type = TypeId::Float;
                    break;
                default:
                    break;
            }

            if (new_type != old_type) {
                n.type = new_type;
                changed = true;
                r.changed = true;
            }
        }
    }

    return r;
}

}  // namespace arcjit
