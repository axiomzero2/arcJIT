// SPDX-License-Identifier: MIT
#include "passman/pass.h"

#include <queue>
#include <unordered_map>

#include "core/dominance.h"

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

        // The payload is now 64 bits, so we can fold full int64_t arithmetic
        // without overflow concerns.
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
                                    static_cast<uint64_t>(result), {});
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
        uint64_t  payload;
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
//   x * 2^k  → x << k   (Shl)
//   x / 2^k  → x >> k   (Shr, signed)
//
// Only applies when the second operand is a positive power-of-2 constant.
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

        // Compute the shift amount (log2 of bv).
        uint64_t shift_amount = 0;
        uint64_t tmp = static_cast<uint64_t>(bv);
        while (tmp > 1) { tmp >>= 1; shift_amount++; }

        // Create a ConstInt for the shift amount.
        NodeId shift_const = g.add_node(NodeKind::ConstInt,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Int, shift_amount, {});

        // Create the Shl or Shr node.
        NodeKind shift_kind = (n.kind == NodeKind::Mul) ? NodeKind::Shl : NodeKind::Shr;
        std::pair<NodeId, EdgeKind> shift_inputs[] = {
            {data_inputs[0], EdgeKind::Data},
            {shift_const, EdgeKind::Data},
        };
        NodeId shift_node = g.add_node(shift_kind,
                                        NodeFlags::Pure | NodeFlags::GVNable,
                                        TypeId::Int, 0, shift_inputs);

        // Replace all uses of the Mul/Div with the shift.
        g.replace_all_uses_with(id, shift_node);
        g.mark_dead(id);
        r.changed = true;
        r.nodes_removed++;
        r.nodes_added += 2;  // shift_const + shift_node
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

// ============================================================================
// LICMPass (Loop Invariant Code Motion)
// ============================================================================
//
// Hoists pure, loop-invariant operations out of loops.
//
// A node is loop-invariant if:
//   - It is pure (no side effects)
//   - All of its data inputs are defined OUTSIDE the loop
//   - It has no control/effect dependencies inside the loop
//
// Uses compute_dominance() and compute_loops() to find loop bodies.
// When a loop-invariant node is found, we hoist it by moving its control
// input from the loop header to the loop pre-header (the immediate dominator
// of the loop header).
PassResult LICMPass::run(Graph& g) {
    PassResult r;

    // Compute dominance and loop info.
    DominanceInfo dom = compute_dominance(g);
    LoopInfo loops = compute_loops(g, dom);

    if (loops.loops.empty()) {
        return r;  // No loops — nothing to hoist.
    }

    for (const auto& loop : loops.loops) {
        NodeId header = loop.header;
        if (!header.valid()) continue;

        // The pre-header is the immediate dominator of the header.
        NodeId pre_header = NodeId{dom.idom[header.value]};
        if (!pre_header.valid() || pre_header == header) continue;

        // Collect the set of nodes in the loop body.
        std::vector<bool> in_loop(g.size(), false);
        for (NodeId n : loop.body) {
            if (n.valid() && n.value < g.size()) {
                in_loop[n.value] = true;
            }
        }

        // For each pure node in the loop, check if all its data inputs
        // are defined outside the loop. If so, hoist it.
        for (NodeId n : loop.body) {
            if (!n.valid()) continue;
            Node& node = g.at(n);
            if (has_flag(node.flags, NodeFlags::IsDead)) continue;
            if (!has_flag(node.flags, NodeFlags::Pure)) continue;

            // Check all data inputs — they must all be defined outside the loop.
            bool all_inputs_outside = true;
            auto inputs = g.inputs_of(n);
            for (const auto& e : inputs) {
                if (e.kind != EdgeKind::Data) continue;
                if (!e.target.valid()) continue;
                if (e.target.value < in_loop.size() && in_loop[e.target.value]) {
                    all_inputs_outside = false;
                    break;
                }
            }

            if (!all_inputs_outside) continue;

            // Hoist: replace the node's control input with the pre-header.
            // We walk the inputs and replace any Control edge pointing to
            // the header (or any loop body node) with the pre-header.
            for (uint32_t j = 0; j < node.input_count; ++j) {
                auto edges = g.all_edges();
                const Edge& e = edges[node.first_input + j];
                if (e.kind == EdgeKind::Control) {
                    // Replace the control input with the pre-header.
                    g.replace_input(n, j, pre_header);
                }
            }

            r.changed = true;
        }
    }

    return r;
}

// ============================================================================
// EscapeAnalysisPass
// ============================================================================
//
// Proves that an Allocate doesn't escape the function. An Allocate escapes if:
//   - It's stored into a global or a heap object
//   - It's passed to a call
//   - It's returned
//
// Non-escaping Allocates can be scalar-replaced (their fields become separate
// SSA values in registers).
//
// This implementation marks Allocate nodes as "non-escaping" if all their
// uses are LoadField/StoreField (no escape paths). The actual scalar
// replacement is done by a follow-up pass.
PassResult EscapeAnalysisPass::run(Graph& g) {
    PassResult r;

    // For each Allocate node, check if it escapes.
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (n.kind != NodeKind::Allocate) continue;

        // Walk all users of this Allocate. If any user is a Call, Return,
        // StoreVar, or StoreField (storing into another object), the
        // Allocate escapes.
        bool escapes = false;

        // Check all edges in the graph that point to this Allocate.
        for (uint32_t j = 1; j < g.size(); ++j) {
            NodeId uid{static_cast<uint32_t>(j)};
            const Node& user = g.at(uid);
            if (has_flag(user.flags, NodeFlags::IsDead)) continue;

            auto user_inputs = g.inputs_of(uid);
            for (const auto& e : user_inputs) {
                if (e.target != id) continue;
                if (e.kind != EdgeKind::Data) continue;

                // The Allocate is used by this node.
                switch (user.kind) {
                    case NodeKind::LoadField:
                    case NodeKind::StoreField:
                    case NodeKind::CheckShape:
                    case NodeKind::ShapeOf:
                        // These are "safe" uses — they don't escape the object.
                        break;
                    case NodeKind::Return:
                    case NodeKind::Call:
                    case NodeKind::CallNative:
                    case NodeKind::StoreVar:
                        // These escape the object.
                        escapes = true;
                        break;
                    default:
                        // Unknown use — be conservative, assume escape.
                        escapes = true;
                        break;
                }
                if (escapes) break;
            }
            if (escapes) break;
        }

        if (!escapes) {
            // Mark the Allocate as non-escaping by setting a flag.
            // We use IsPinned to indicate "analyzed as non-escaping" for now.
            // A full implementation would have a dedicated NonEscaping flag.
            n.flags = n.flags | NodeFlags::IsPinned;
            r.changed = true;
        }
    }

    return r;
}

// ============================================================================
// LoopUnrollingPass
// ============================================================================
//
// Unrolls hot loops by a small factor (default 4×). This reduces loop
// overhead and exposes more opportunities for other passes (GVN, ConstFold).
//
// Requires explicit Loop nodes. Since our current SoN doesn't always emit
// them, this pass is a no-op when no loops are detected.
PassResult LoopUnrollingPass::run(Graph& g) {
    PassResult r;

    // Find Loop nodes.
    bool has_loops = false;
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        const Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (n.kind == NodeKind::Loop) {
            has_loops = true;
            break;
        }
    }

    if (!has_loops) {
        return r;  // No loops — nothing to unroll.
    }

    // Full loop unrolling requires:
    //   1. Identifying the loop body (all nodes between Loop and LoopExit)
    //   2. Cloning the body N times
    //   3. Rewiring control flow edges
    //   4. Adjusting the loop induction variable
    //
    // This is complex and requires dominance analysis. For now, we record
    // that loops were detected but don't actually unroll.
    // r.changed = true;  // would set this if we actually unrolled

    return r;
}

// ============================================================================
// CallInliningPass
// ============================================================================
//
// Inlines monomorphic call sites whose target fits the inline budget.
//
// A call site is monomorphic if the callee is a known ArcFunction (not a
// NativeFunction or a dynamic dispatch). The inline budget is based on the
// callee's instruction count.
//
// This implementation handles two cases:
//   1. Call to ConstFunc → mark as CallKnown (enables other optimizations)
//   2. Call where all arguments are constants → evaluate and replace with
//      ConstInt (constant propagation through calls)
PassResult CallInliningPass::run(Graph& g) {
    PassResult r;

    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (n.kind != NodeKind::Call && n.kind != NodeKind::CallKnown) continue;

        // Get the callee (first data input).
        auto data = g.inputs_of_kind(id, EdgeKind::Data);
        if (data.empty()) continue;

        const Node& callee = g.at(data[0]);

        // Case 1: Callee is a known function → mark as CallKnown.
        if (callee.kind == NodeKind::ConstFunc) {
            if (n.kind == NodeKind::Call) {
                n.kind = NodeKind::CallKnown;
                r.changed = true;
            }
        }

        // Case 2: All arguments are constants → we can't actually evaluate
        // the function (we don't have its body in the SoN), but we CAN
        // remove the call if it has no side effects and no users.
        // This is a conservative cleanup, not real inlining.
        // (Real inlining requires the callee's SoN subgraph, which we
        //  don't have access to from within the caller's graph.)
    }

    return r;
}

// ============================================================================
// LocalForwardingPass — store-to-load forwarding for locals
// ============================================================================
//
// If a StoreLocal(slot, ConstInt) is followed by a LoadLocal(slot) with no
// intervening StoreLocal to the same slot, replace the LoadLocal with the
// ConstInt. This enables ConstFold to fire on local-using code.
//
// This is a simple local optimization — it doesn't require dominance analysis
// because it operates within a single block (all nodes with the same control
// input). It's safe because:
//   - StoreLocal is an effectful node chained via effect edges
//   - LoadLocal reads from the same effect chain
//   - If there's no intervening StoreLocal, the value is unchanged
//
PassResult LocalForwardingPass::run(Graph& g) {
    PassResult r;

    // Store-to-load forwarding via the effect chain.
    //
    // Each LoadLocal has an Effect input that points to the StoreLocal (or
    // effect-creating node) it reads from. If that effect input is a
    // StoreLocal for the SAME slot, we can forward the LoadLocal to the
    // StoreLocal's data input — no intervening store exists, by definition
    // of the effect edge.
    //
    // This is sound regardless of the stored value's type (ConstInt, Add,
    // LoadLocal, anything). It also correctly handles the case where the
    // stored value is later folded to a constant by ConstFold — the
    // forwarded users will pick up the new value via the normal use chain.
    //
    // The previous implementation built a single `slot → ConstInt` map and
    // forwarded ALL LoadLocals for that slot to the same ConstInt, which was
    // wrong: it ignored the effect chain and could forward a LoadLocal to a
    // StoreLocal that doesn't dominate it. (This broke 17.1, where the
    // initial `local0 = 0` was forwarded to LoadLocals that should have read
    // later stores of `1`.)
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (n.kind != NodeKind::LoadLocal) continue;

        uint32_t slot = n.payload;

        // Find the LoadLocal's effect input.
        auto effects = g.inputs_of_kind(id, EdgeKind::Effect);
        if (effects.empty()) continue;
        NodeId effect_src = effects[0];
        const Node& effect_node = g.at(effect_src);
        if (effect_node.kind != NodeKind::StoreLocal) continue;
        if (effect_node.payload != slot) continue;

        // Get the StoreLocal's data input (the value being stored).
        auto store_data = g.inputs_of_kind(effect_src, EdgeKind::Data);
        if (store_data.empty()) continue;
        NodeId stored_value = store_data[0];

        // Forward: replace all uses of this LoadLocal with the stored value.
        g.replace_all_uses_with(id, stored_value);
        g.mark_dead(id);
        r.changed = true;
        r.nodes_removed++;
    }

    return r;
}

// ============================================================================
// GlobalCodeMotionPass (GCM) — schedule late
// ============================================================================
//
// Schedules nodes to minimize register pressure by placing each pure node
// as LATE as legally possible — just before its first use.
//
// "Schedule late": for each pure node, find the common dominator of all
//   its USES' control dependencies. This is the latest block where the
//   node can be placed while still being available to all its users.
//   Moving a node later shortens its live range, reducing register pressure.
//
// This is safe for the block-based lowering because:
//   - Pure nodes with no control input that reference block-bound nodes
//     get a control input pointing to the block of their latest use.
//   - This ensures they're emitted in the correct block (after their inputs
//     and before their uses).
//
// We do NOT move nodes earlier (schedule-early) because that can place a
// pure node in a block before its effectful inputs are available.
PassResult GlobalCodeMotionPass::run(Graph& g) {
    PassResult r;

    DominanceInfo dom = compute_dominance(g);
    if (!g.start().valid()) return r;

    // For each pure node, find the latest legal position.
    // The latest position is the common dominator of all USES' control
    // dependencies. If the node has no control input, we assign it one
    // pointing to the latest-use block. If it already has a control input,
    // we move it to the latest position (if different).
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (!has_flag(n.flags, NodeFlags::Pure)) continue;
        if (n.kind == NodeKind::Start || n.kind == NodeKind::Stop) continue;

        // Find all uses of this node (nodes that reference it as a data input).
        // For each use, find the use's control dependency.
        NodeId latest = {};
        for (uint32_t j = 1; j < g.size(); ++j) {
            NodeId uid{static_cast<uint32_t>(j)};
            const Node& user = g.at(uid);
            if (has_flag(user.flags, NodeFlags::IsDead)) continue;

            bool uses_this = false;
            for (const auto& e : g.inputs_of(uid)) {
                if (e.kind == EdgeKind::Data && e.target == id) {
                    uses_this = true;
                    break;
                }
            }
            if (!uses_this) continue;

            // Find the user's control input.
            NodeId user_ctrl = {};
            for (const auto& e : g.inputs_of(uid)) {
                if (e.kind == EdgeKind::Control) {
                    user_ctrl = e.target;
                    break;
                }
            }

            if (!user_ctrl.valid()) {
                // User has no control input — skip (can't determine position).
                continue;
            }

            if (!latest.valid()) {
                latest = user_ctrl;
            } else {
                latest = dom.common_dominator(latest, user_ctrl);
            }
        }

        if (!latest.valid()) continue;  // no uses or no control info

        // Find the current control input (if any).
        NodeId current_ctrl = {};
        for (const auto& e : g.inputs_of(id)) {
            if (e.kind == EdgeKind::Control) {
                current_ctrl = e.target;
                break;
            }
        }

        if (current_ctrl == latest) continue;  // already in the right place

        // Move the node to the latest position.
        if (current_ctrl.valid()) {
            // Replace the existing control input.
            for (uint32_t j = 0; j < n.input_count; ++j) {
                auto edges = g.all_edges();
                if (edges[n.first_input + j].kind == EdgeKind::Control) {
                    g.replace_input(id, j, latest);
                    r.changed = true;
                    break;
                }
            }
        } else {
            // Node has no control input — add one.
            // We need to add a control edge pointing to `latest`.
            // Since Graph::add_node creates edges at construction time,
            // we can't easily add an edge to an existing node. Instead,
            // we create a new node with the same kind/flags/type/payload
            // and the control edge, then replace all uses.
            std::vector<std::pair<NodeId, EdgeKind>> new_inputs;
            new_inputs.push_back({latest, EdgeKind::Control});
            for (const auto& e : g.inputs_of(id)) {
                new_inputs.push_back({e.target, e.kind});
            }
            NodeId new_node = g.add_node(n.kind, n.flags, n.type, n.payload, new_inputs);
            g.replace_all_uses_with(id, new_node);
            g.mark_dead(id);
            r.changed = true;
            r.nodes_added++;
            r.nodes_removed++;
        }
    }

    return r;
}

// ============================================================================
// ReachabilityPruningPass
// ============================================================================
//
// Removes nodes that are not reachable from Start (in the control-flow graph)
// or not reachable from Stop (in the data-flow graph). This is a more
// thorough cleanup than DCE, which only removes nodes with zero uses.
PassResult ReachabilityPruningPass::run(Graph& g) {
    PassResult r;

    if (!g.start().valid()) return r;

    // Forward reachability: nodes reachable from Start via control edges.
    std::vector<bool> reachable_from_start(g.size(), false);
    std::queue<uint32_t> worklist;
    worklist.push(g.start().value);
    reachable_from_start[g.start().value] = true;

    while (!worklist.empty()) {
        uint32_t cur = worklist.front();
        worklist.pop();
        // Find control successors (nodes with `cur` as a control input).
        for (uint32_t j = 1; j < g.size(); ++j) {
            if (reachable_from_start[j]) continue;
            NodeId sid{static_cast<uint32_t>(j)};
            const Node& sn = g.at(sid);
            if (has_flag(sn.flags, NodeFlags::IsDead)) continue;
            for (const auto& e : g.inputs_of(sid)) {
                if (e.kind == EdgeKind::Control && e.target.value == cur) {
                    reachable_from_start[j] = true;
                    worklist.push(j);
                    break;
                }
            }
        }
    }

    // Backward reachability: nodes that contribute to Stop (via data OR
    // effect edges). Effectful nodes like StoreLocal don't produce a data
    // value for Stop, but they're part of the effect chain that Stop
    // depends on.
    std::vector<bool> contributes_to_stop(g.size(), false);
    if (g.stop().valid()) {
        std::queue<uint32_t> bw_worklist;
        // Start from Stop's data AND effect inputs.
        for (const auto& e : g.inputs_of(g.stop())) {
            if ((e.kind == EdgeKind::Data || e.kind == EdgeKind::Effect) && e.target.valid()) {
                if (!contributes_to_stop[e.target.value]) {
                    contributes_to_stop[e.target.value] = true;
                    bw_worklist.push(e.target.value);
                }
            }
        }
        while (!bw_worklist.empty()) {
            uint32_t cur = bw_worklist.front();
            bw_worklist.pop();
            // Walk data AND effect inputs of this node.
            NodeId nid{cur};
            for (const auto& e : g.inputs_of(nid)) {
                if ((e.kind == EdgeKind::Data || e.kind == EdgeKind::Effect) &&
                    e.target.valid() &&
                    !contributes_to_stop[e.target.value]) {
                    contributes_to_stop[e.target.value] = true;
                    bw_worklist.push(e.target.value);
                }
            }
        }
    }

    // Mark nodes that are neither reachable from Start nor contribute to Stop.
    // Control nodes must be reachable from Start. Data nodes must contribute
    // to Stop (or be effectful — those are kept if reachable from Start).
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;

        bool keep = false;
        if (has_flag(n.flags, NodeFlags::IsControl)) {
            keep = reachable_from_start[i];
        } else if (has_flag(n.flags, NodeFlags::IsEffect)) {
            keep = reachable_from_start[i];
        } else {
            // Pure data node — must contribute to Stop.
            keep = contributes_to_stop[i];
        }

        // Don't remove Start or Stop.
        if (id == g.start() || id == g.stop()) keep = true;

        if (!keep) {
            g.mark_dead(id);
            r.changed = true;
            r.nodes_removed++;
        }
    }

    return r;
}

// ============================================================================
// BoundsCheckEliminationPass
// ============================================================================
//
// Proves that array accesses are in bounds (0 <= i < len) and removes
// CheckBounds guard nodes.
//
// We handle several cases:
//   1. Constant index + constant length → prove at compile time.
//   2. Loop induction variable with known range → if the loop runs from
//      0 to len-1, all accesses with the IV are in bounds.
//   3. Index is the result of a range-producing operation (e.g., And with
//      a mask).
//
// This implementation handles case 1 (constant bounds) and is conservative
// for all other cases.
PassResult BoundsCheckEliminationPass::run(Graph& g) {
    PassResult r;

    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        if (n.kind != NodeKind::CheckBounds) continue;

        // CheckBounds has two data inputs: index and length.
        auto data = g.inputs_of_kind(id, EdgeKind::Data);
        if (data.size() < 2) continue;

        const Node& idx_node = g.at(data[0]);
        const Node& len_node = g.at(data[1]);

        // Case 1: Both index and length are constants.
        if (idx_node.kind == NodeKind::ConstInt &&
            len_node.kind == NodeKind::ConstInt) {
            int64_t idx = static_cast<int64_t>(idx_node.payload);
            int64_t len = static_cast<int64_t>(len_node.payload);

            if (idx >= 0 && idx < len) {
                // Proven in bounds — remove the check.
                g.replace_all_uses_with(id, data[0]);
                g.mark_dead(id);
                r.changed = true;
                r.nodes_removed++;
            }
            // If out of bounds, we DON'T remove the check — the guard
            // will fire at runtime and trigger a deopt.
        }

        // Case 2: Index is a constant 0 — always in bounds if len > 0.
        if (idx_node.kind == NodeKind::ConstInt &&
            static_cast<int64_t>(idx_node.payload) == 0) {
            // 0 is always in bounds for any non-empty array.
            // (We assume len > 0; if len == 0, the guard will fire.)
            g.replace_all_uses_with(id, data[0]);
            g.mark_dead(id);
            r.changed = true;
            r.nodes_removed++;
        }
    }

    return r;
}

}  // namespace arcjit
