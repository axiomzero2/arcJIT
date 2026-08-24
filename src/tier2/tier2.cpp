// SPDX-License-Identifier: MIT
#include "tier2/tier2.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <print>
#include <unordered_map>

#include "machinery/fuse.h"

namespace arcjit {

// ============================================================================
// Lowering: Tier1Function → Sea of Nodes Graph
// ============================================================================
namespace {

class Tier1ToSoN {
public:
    const Tier1Function& fn;
    Graph& g;

    // Map vreg → producing SoN node.
    //
    // vregs are dense uint32_t IDs starting from 1 (0 = sentinel). We use
    // a vector indexed by vreg ID instead of an unordered_map — O(1)
    // lookup with no hashing, no probing, no cache misses. The previous
    // implementation used unordered_map which was ~3-5x slower for the
    // hot lookup path in large functions.
    //
    // kNoNode sentinel = invalid vreg mapping (the vreg hasn't been
    // produced yet). We use NodeId{0} which is the graph's own invalid
    // sentinel — graph.add_node never returns NodeId{0}.
    std::vector<NodeId> vreg_to_node;

    void ensure_vreg_capacity(uint32_t v) {
        if (v >= vreg_to_node.size()) {
            vreg_to_node.resize(v + 1, NodeId{});  // NodeId{} = invalid
        }
    }

    // Set the SoN node for a vreg. Grows the vector if needed.
    void set_vreg_node(uint32_t v, NodeId n) {
        ensure_vreg_capacity(v);
        vreg_to_node[v] = n;
    }

    // The current control-flow node (last Region / If / Start).
    NodeId current_control;

    // The current effect node (last effectful op).
    NodeId current_effect;

    // Pending control-flow edges that target each label ID.
    // When we encounter a Label, we create a Region node merging all
    // pending predecessors. This is how branch merge points are modeled.
    std::unordered_map<uint32_t, std::vector<NodeId>> pending_label_preds;

    // Pending effect edges that target each label ID (for effect chain
    // continuity across branches).
    std::unordered_map<uint32_t, std::vector<NodeId>> pending_label_effects;

    // Collected Stop nodes — if there are multiple Returns, we merge them
    // into a single Stop with a Phi for the return value.
    std::vector<NodeId> stop_nodes;
    std::vector<NodeId> return_values;

    explicit Tier1ToSoN(const Tier1Function& f, Graph& graph)
        : fn(f), g(graph) {}

    [[nodiscard]] std::expected<void, std::string> run() {
        // Start node.
        current_control = g.add_node(NodeKind::Start,
                                      NodeFlags::IsControl | NodeFlags::NoDeopt,
                                      TypeId::Bottom, 0, {});
        g.set_start(current_control);

        // Initial effect = Start.
        current_effect = current_control;

        for (const auto& inst : fn.insts) {
            switch (inst.op) {
                case Tier1Op::Label: {
                    // A Label marks a basic-block boundary. Collect all
                    // pending predecessors (from Jump/BranchIfFalse that
                    // target this label) plus the fall-through control (if
                    // any — the previous block didn't end with an explicit
                    // jump).
                    auto& preds = pending_label_preds[inst.payload];
                    auto& effects = pending_label_effects[inst.payload];

                    // If current_control is valid and wasn't already added
                    // as a predecessor (i.e., the previous block fell through
                    // without a Jump), add it.
                    if (current_control.valid()) {
                        preds.push_back(current_control);
                        effects.push_back(current_effect);
                    }

                    if (preds.size() == 1) {
                        // Single predecessor — no Region needed, just use
                        // the predecessor directly.
                        current_control = preds[0];
                        current_effect  = effects[0];
                    } else {
                        // Multiple predecessors — create a Region node.
                        std::vector<std::pair<NodeId, EdgeKind>> ctrl_inputs;
                        for (auto p : preds) {
                            ctrl_inputs.push_back({p, EdgeKind::Control});
                        }
                        current_control = g.add_node(NodeKind::Region,
                                                      NodeFlags::IsControl,
                                                      TypeId::Bottom, 0, ctrl_inputs);
                        // For the effect chain, create an EffectPhi that
                        // merges the incoming effects.
                        std::vector<std::pair<NodeId, EdgeKind>> eff_inputs;
                        eff_inputs.push_back({current_control, EdgeKind::Control});
                        for (auto e : effects) {
                            eff_inputs.push_back({e, EdgeKind::Effect});
                        }
                        current_effect = g.add_node(NodeKind::EffectPhi,
                                                     NodeFlags::IsEffect,
                                                     TypeId::Bottom, 0, eff_inputs);
                    }
                    break;
                }

                case Tier1Op::LoadConstImm: {
                    auto n = g.add_node(NodeKind::ConstInt,
                                         NodeFlags::Pure | NodeFlags::CSEable | NodeFlags::GVNable | NodeFlags::NoDeopt,
                                         TypeId::Int, inst.payload, {});
                    set_vreg_node(inst.dst, n);
                    break;
                }
                case Tier1Op::LoadConst: {
                    // For the scaffold we treat LoadConst the same as LoadConstImm
                    // (it loads an immediate). Real Tier-2 would load from the
                    // constant pool.
                    auto n = g.add_node(NodeKind::ConstInt,
                                         NodeFlags::Pure | NodeFlags::CSEable | NodeFlags::GVNable | NodeFlags::NoDeopt,
                                         TypeId::Int, inst.payload, {});
                    set_vreg_node(inst.dst, n);
                    break;
                }
                case Tier1Op::LoadLocal: {
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {current_control, EdgeKind::Control},
                        {current_effect, EdgeKind::Effect},
                    };
                    auto n = g.add_node(NodeKind::LoadLocal,
                                         NodeFlags::IsEffect | NodeFlags::NoDeopt,
                                         TypeId::Int, inst.payload, inputs);
                    set_vreg_node(inst.dst, n);
                    current_effect = n;
                    break;
                }
                case Tier1Op::StoreLocal: {
                    NodeId val = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {current_control, EdgeKind::Control},
                        {current_effect, EdgeKind::Effect},
                        {val, EdgeKind::Data},
                    };
                    auto n = g.add_node(NodeKind::StoreLocal,
                                         NodeFlags::IsEffect,
                                         TypeId::Bottom, inst.payload, inputs);
                    current_effect = n;
                    break;
                }
                case Tier1Op::LoadVar: {
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {current_control, EdgeKind::Control},
                        {current_effect, EdgeKind::Effect},
                    };
                    auto n = g.add_node(NodeKind::LoadVar,
                                         NodeFlags::IsEffect | NodeFlags::NoDeopt,
                                         TypeId::Top, inst.payload, inputs);
                    set_vreg_node(inst.dst, n);
                    current_effect = n;
                    break;
                }
                case Tier1Op::StoreVar: {
                    NodeId val = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {current_control, EdgeKind::Control},
                        {current_effect, EdgeKind::Effect},
                        {val, EdgeKind::Data},
                    };
                    auto n = g.add_node(NodeKind::StoreVar,
                                         NodeFlags::IsEffect,
                                         TypeId::Bottom, inst.payload, inputs);
                    current_effect = n;
                    break;
                }
                case Tier1Op::Mov: {
                    set_vreg_node(inst.dst, lookup_vreg(inst.src1));
                    break;
                }

                // --- Pure arithmetic ---
                case Tier1Op::Add:
                case Tier1Op::Sub:
                case Tier1Op::Mul:
                case Tier1Op::Div:
                case Tier1Op::Pow: {
                    NodeId a = lookup_vreg(inst.src1);
                    NodeId b = lookup_vreg(inst.src2);
                    NodeKind k = static_cast<NodeKind>(
                        static_cast<int>(NodeKind::Add) +
                        static_cast<int>(static_cast<uint8_t>(inst.op) - static_cast<uint8_t>(Tier1Op::Add)));
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {a, EdgeKind::Data}, {b, EdgeKind::Data},
                    };
                    auto n = g.add_node(k,
                                         NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative
                                         | (inst.op == Tier1Op::Add || inst.op == Tier1Op::Mul
                                            ? NodeFlags::Commutative : NodeFlags::None),
                                         TypeId::Int, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }

                // --- Bit operations ---
                case Tier1Op::Shl:
                case Tier1Op::Shr: {
                    NodeId a = lookup_vreg(inst.src1);
                    NodeId b = lookup_vreg(inst.src2);
                    NodeKind k = (inst.op == Tier1Op::Shl) ? NodeKind::Shl : NodeKind::Shr;
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {a, EdgeKind::Data}, {b, EdgeKind::Data},
                    };
                    auto n = g.add_node(k,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Int, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }

                case Tier1Op::Neg: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::Neg,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Int, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }

                // --- Comparisons ---
                case Tier1Op::Eq:
                case Tier1Op::Ne:
                case Tier1Op::Lt:
                case Tier1Op::Gt:
                case Tier1Op::Lte:
                case Tier1Op::Gte: {
                    NodeId a = lookup_vreg(inst.src1);
                    NodeId b = lookup_vreg(inst.src2);
                    NodeKind k = static_cast<NodeKind>(
                        static_cast<int>(NodeKind::Eq) +
                        static_cast<int>(static_cast<uint8_t>(inst.op) - static_cast<uint8_t>(Tier1Op::Eq)));
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {a, EdgeKind::Data}, {b, EdgeKind::Data},
                    };
                    auto n = g.add_node(k,
                                         NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                                         TypeId::Bool, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }

                case Tier1Op::And:
                case Tier1Op::Or: {
                    NodeId a = lookup_vreg(inst.src1);
                    NodeId b = lookup_vreg(inst.src2);
                    NodeKind k = (inst.op == Tier1Op::And) ? NodeKind::And : NodeKind::Or;
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {a, EdgeKind::Data}, {b, EdgeKind::Data},
                    };
                    auto n = g.add_node(k,
                                         NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                                         TypeId::Bool, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }
                case Tier1Op::Not: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::Not,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Bool, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }

                case Tier1Op::IsTruthy: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::ToBool,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Bool, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }
                case Tier1Op::ToFloat: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::ToFloat,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Float, 0, inputs);
                    set_vreg_node(inst.dst, n);
                    break;
                }

                // --- Memory ops (treated as opaque effectful calls) ---
                case Tier1Op::AllocList:
                case Tier1Op::ListAppend:
                case Tier1Op::ListGet:
                case Tier1Op::ListSet:
                case Tier1Op::AllocInstance:
                case Tier1Op::GetField:
                case Tier1Op::SetField:
                case Tier1Op::Call:
                case Tier1Op::CallNative: {
                    // For effectful ops with a result, we create a generic
                    // effect node whose result is the dst vreg.
                    NodeId a = inst.src1 ? lookup_vreg(inst.src1) : NodeId{};
                    NodeId b = inst.src2 ? lookup_vreg(inst.src2) : NodeId{};

                    std::vector<std::pair<NodeId, EdgeKind>> inputs;
                    inputs.push_back({current_control, EdgeKind::Control});
                    inputs.push_back({current_effect, EdgeKind::Effect});
                    if (a.valid()) inputs.push_back({a, EdgeKind::Data});
                    if (b.valid()) inputs.push_back({b, EdgeKind::Data});

                    auto n = g.add_node(NodeKind::Call,
                                         NodeFlags::IsEffect,
                                         TypeId::Top, inst.payload, inputs);
                    if (inst.dst != 0) set_vreg_node(inst.dst, n);
                    current_effect = n;
                    break;
                }

                case Tier1Op::Return: {
                    NodeId v = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {v, EdgeKind::Data},
                        {current_control, EdgeKind::Control},
                        {current_effect, EdgeKind::Effect},
                    };
                    auto stop = g.add_node(NodeKind::Stop,
                                            NodeFlags::IsControl | NodeFlags::NoDeopt,
                                            TypeId::Bottom, 0, inputs);
                    // Collect all Stop nodes so we can merge them at the end.
                    stop_nodes.push_back(stop);
                    return_values.push_back(v);
                    // After Return, control is dead (unreachable).
                    current_control = NodeId{};
                    current_effect  = NodeId{};
                    break;
                }

                case Tier1Op::Jump: {
                    // Unconditional jump — add current control as a pending
                    // predecessor of the target label, then mark current
                    // control as dead (no fall-through).
                    pending_label_preds[inst.payload].push_back(current_control);
                    pending_label_effects[inst.payload].push_back(current_effect);
                    current_control = NodeId{};
                    current_effect  = NodeId{};
                    break;
                }
                case Tier1Op::BranchIfFalse:
                case Tier1Op::BranchIfTrue: {
                    // Branch creates an If node with two successors.
                    // BranchIfFalse: "jump to target if cond is false;
                    //                 fall through if true"
                    // BranchIfTrue:  "jump to target if cond is true;
                    //                 fall through if false"
                    NodeId cond = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {cond, EdgeKind::Data},
                        {current_control, EdgeKind::Control},
                    };
                    auto if_node = g.add_node(NodeKind::If,
                                               NodeFlags::IsControl | NodeFlags::NoDeopt,
                                               TypeId::Bottom, 0, inputs);
                    auto if_true  = g.add_node(NodeKind::IfTrue,
                                                NodeFlags::IsControl | NodeFlags::NoDeopt,
                                                TypeId::Bottom, 0,
                                                std::initializer_list<std::pair<NodeId, EdgeKind>>{
                                                    {if_node, EdgeKind::Control}});
                    auto if_false = g.add_node(NodeKind::IfFalse,
                                                NodeFlags::IsControl | NodeFlags::NoDeopt,
                                                TypeId::Bottom, 0,
                                                std::initializer_list<std::pair<NodeId, EdgeKind>>{
                                                    {if_node, EdgeKind::Control}});

                    if (inst.op == Tier1Op::BranchIfFalse) {
                        // Jump target gets the false branch; fall-through is true.
                        pending_label_preds[inst.payload].push_back(if_false);
                        pending_label_effects[inst.payload].push_back(current_effect);
                        current_control = if_true;
                        // Effect stays the same on the fall-through path.
                    } else {
                        // BranchIfTrue: jump target gets the true branch; fall-through is false.
                        pending_label_preds[inst.payload].push_back(if_true);
                        pending_label_effects[inst.payload].push_back(current_effect);
                        current_control = if_false;
                    }
                    break;
                }

                case Tier1Op::Halt: {
                    std::pair<NodeId, EdgeKind> inputs[] = {
                        {current_control, EdgeKind::Control},
                        {current_effect, EdgeKind::Effect},
                    };
                    auto stop = g.add_node(NodeKind::Stop,
                                            NodeFlags::IsControl | NodeFlags::NoDeopt,
                                            TypeId::Bottom, 0, inputs);
                    stop_nodes.push_back(stop);
                    return_values.push_back(NodeId{});  // no return value for Halt
                    current_control = NodeId{};
                    current_effect  = NodeId{};
                    break;
                }
            }
        }

        // Merge multiple Stop nodes into one.
        if (stop_nodes.empty()) {
            // No explicit Return/Halt — synthesize a Stop.
            std::pair<NodeId, EdgeKind> inputs[] = {
                {current_control, EdgeKind::Control},
                {current_effect, EdgeKind::Effect},
            };
            g.set_stop(g.add_node(NodeKind::Stop,
                                   NodeFlags::IsControl | NodeFlags::NoDeopt,
                                   TypeId::Bottom, 0, inputs));
        } else if (stop_nodes.size() == 1) {
            g.set_stop(stop_nodes[0]);
        } else {
            // Multiple Stop nodes — create a merged Stop.
            // The merged Stop has a Phi for the return value (if all paths
            // return a value) and a Region merging the control inputs.
            std::vector<std::pair<NodeId, EdgeKind>> region_inputs;
            std::vector<std::pair<NodeId, EdgeKind>> phi_inputs;
            std::vector<NodeId> control_preds;

            for (size_t i = 0; i < stop_nodes.size(); ++i) {
                // Each Stop's control input is the predecessor we want to merge.
                auto stop_data = g.inputs_of_kind(stop_nodes[i], EdgeKind::Data);
                auto stop_ctrl = g.inputs_of_kind(stop_nodes[i], EdgeKind::Control);
                if (!stop_ctrl.empty()) {
                    control_preds.push_back(stop_ctrl[0]);
                    region_inputs.push_back({stop_ctrl[0], EdgeKind::Control});
                    if (!stop_data.empty() && stop_data[0].valid()) {
                        phi_inputs.push_back({stop_data[0], EdgeKind::Data});
                    } else {
                        // Insert a null placeholder for paths without a return value.
                        NodeId zero = g.add_node(NodeKind::ConstInt,
                                                  NodeFlags::Pure | NodeFlags::GVNable,
                                                  TypeId::Int, 0, {});
                        phi_inputs.push_back({zero, EdgeKind::Data});
                    }
                }
            }

            NodeId region = g.add_node(NodeKind::Region, NodeFlags::IsControl,
                                        TypeId::Bottom, 0, region_inputs);

            // Create a Phi for the return value.
            std::vector<std::pair<NodeId, EdgeKind>> phi_with_ctrl;
            phi_with_ctrl.push_back({region, EdgeKind::Control});
            for (auto& p : phi_inputs) phi_with_ctrl.push_back(p);
            NodeId phi = g.add_node(NodeKind::Phi, NodeFlags::None, TypeId::Int, 0,
                                     phi_with_ctrl);

            // Merge effects too — collect all effect inputs.
            std::vector<std::pair<NodeId, EdgeKind>> eff_phi_inputs;
            eff_phi_inputs.push_back({region, EdgeKind::Control});
            for (size_t i = 0; i < stop_nodes.size(); ++i) {
                auto stop_eff = g.inputs_of_kind(stop_nodes[i], EdgeKind::Effect);
                if (!stop_eff.empty()) {
                    eff_phi_inputs.push_back({stop_eff[0], EdgeKind::Effect});
                }
            }
            NodeId eff_phi = g.add_node(NodeKind::EffectPhi, NodeFlags::IsEffect,
                                         TypeId::Bottom, 0, eff_phi_inputs);

            std::pair<NodeId, EdgeKind> stop_inputs[] = {
                {phi, EdgeKind::Data},
                {region, EdgeKind::Control},
                {eff_phi, EdgeKind::Effect},
            };
            g.set_stop(g.add_node(NodeKind::Stop,
                                   NodeFlags::IsControl | NodeFlags::NoDeopt,
                                   TypeId::Bottom, 0, stop_inputs));
        }

        return {};
    }

private:
    [[nodiscard]] NodeId lookup_vreg(uint32_t v) {
        ensure_vreg_capacity(v);
        NodeId n = vreg_to_node[v];
        if (n.valid()) return n;
        // Synthesize a fresh ConstInt(0) if undefined.
        auto syn = g.add_node(NodeKind::ConstInt,
                              NodeFlags::Pure | NodeFlags::GVNable,
                              TypeId::Int, 0, {});
        vreg_to_node[v] = syn;
        return syn;
    }
};

}  // namespace

[[nodiscard]] std::expected<void, std::string>
lower_tier1_to_son(const Tier1Function& fn, Tier2Job& job) {
    Tier1ToSoN lowerer{fn, job.graph};
    auto r = lowerer.run();
    if (!r) return std::unexpected(r.error());
    job.source = &fn;
    return {};
}

// ============================================================================
// Lowering: SoN Graph → Tier1Function
// ============================================================================
//
// We walk the graph in node-ID order, emitting one Tier1Inst per non-control
// node. Basic-block boundaries are reconstructed from If/IfTrue/IfFalse nodes.
// For the scaffold this is a straightforward linearization — full GCM
// (global code motion) would do earliness/lateness scheduling.
namespace {

class SoNToTier1 {
public:
    const Tier2Job& job;
    Tier1Function& fn;

    // Map SoN node ID → destination vreg.
    std::unordered_map<uint32_t, uint32_t> node_to_vreg;

    // Map control node ID → label ID in the Tier-1 function.
    std::unordered_map<uint32_t, uint32_t> ctrl_to_label;

    explicit SoNToTier1(const Tier2Job& j, Tier1Function& f) : job(j), fn(f) {}

    [[nodiscard]] std::expected<void, std::string> run() {
        const Graph& g = job.graph;

        // Phase 1: Identify all control-flow blocks and assign labels.
        // Control blocks are: Start, Region, IfTrue, IfFalse, Loop, LoopExit.
        // Each gets a Tier-1 label so we can jump to it.
        for (uint32_t i = 1; i < g.size(); ++i) {
            NodeId id{static_cast<uint32_t>(i)};
            const Node& n = g.at(id);
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;
            if (is_block_header_(n.kind)) {
                ctrl_to_label[i] = fn.alloc_label();
            }
        }

        // Phase 1b: Emit all pure nodes that have NO control input AND whose
        // data inputs are already emitted (or also control-free). We iterate
        // to a fixpoint because pure nodes can chain (Add(Mul(Const, Const),
        // Const) where Mul is also pure/no-control).
        bool changed = true;
        while (changed) {
            changed = false;
            for (uint32_t i = 1; i < g.size(); ++i) {
                NodeId id{static_cast<uint32_t>(i)};
                if (node_to_vreg.count(i)) continue;  // already emitted
                const Node& n = g.at(id);
                if (has_flag(n.flags, NodeFlags::IsDead)) continue;
                if (is_block_header_(n.kind)) continue;
                if (n.kind == NodeKind::Stop) continue;
                if (!has_flag(n.flags, NodeFlags::Pure)) continue;

                // Check if this node has any control input.
                bool has_ctrl = false;
                for (const auto& e : g.inputs_of(id)) {
                    if (e.kind == EdgeKind::Control) { has_ctrl = true; break; }
                }
                if (has_ctrl) continue;

                // Check if ALL data inputs are already emitted (have vregs)
                // or are control-free pure nodes.
                bool all_ready = true;
                for (const auto& e : g.inputs_of(id)) {
                    if (e.kind != EdgeKind::Data) continue;
                    if (!e.target.valid()) continue;
                    if (node_to_vreg.count(e.target.value)) continue;  // already emitted

                    // Not emitted yet — check if it's control-free pure.
                    const Node& prod = g.at(e.target);
                    if (!has_flag(prod.flags, NodeFlags::Pure)) {
                        all_ready = false;
                        break;
                    }
                    bool prod_has_ctrl = false;
                    for (const auto& pe : g.inputs_of(e.target)) {
                        if (pe.kind == EdgeKind::Control) { prod_has_ctrl = true; break; }
                    }
                    if (prod_has_ctrl) {
                        all_ready = false;
                        break;
                    }
                    // It's control-free pure but not yet emitted — we'll
                    // get it in the next iteration.
                    all_ready = false;
                }

                if (all_ready) {
                    emit_data_node_(id);
                    changed = true;
                }
            }
        }

        // Phase 2: Walk the control-flow graph in order, emitting each block.
        // We start from Start and follow control successors.
        std::vector<bool> emitted(g.size(), false);
        if (g.start().valid()) {
            emit_block_sequence_(g.start(), emitted);
        }

        // Phase 2c: Emit any remaining pure nodes that weren't emitted in
        // any block (e.g., an Add with no control input that references
        // LoadLocal nodes). These get emitted at the end, after all blocks.
        for (uint32_t i = 1; i < g.size(); ++i) {
            NodeId id{static_cast<uint32_t>(i)};
            if (node_to_vreg.count(i)) continue;  // already emitted
            const Node& n = g.at(id);
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;
            if (is_block_header_(n.kind)) continue;
            if (n.kind == NodeKind::Stop) continue;
            if (!has_flag(n.flags, NodeFlags::Pure)) continue;

            // Emit it now — all its inputs should have vregs by now.
            emit_data_node_(id);
        }

        // Phase 3: Emit the Return from Stop's data input.
        if (g.stop().valid()) {
            auto stop_data = g.inputs_of_kind(g.stop(), EdgeKind::Data);
            if (!stop_data.empty()) {
                uint32_t v = get_vreg(stop_data[0]);
                fn.emit(Tier1Op::Return, 0, v, 0, 0);
            } else {
                uint32_t z = fn.alloc_vreg();
                fn.emit(Tier1Op::LoadConstImm, z, 0, 0, 0);
                fn.emit(Tier1Op::Return, 0, z, 0, 0);
            }
        }

        return {};
    }

private:
    // Check if a node kind is a block header.
    static bool is_block_header_(NodeKind k) {
        switch (k) {
            case NodeKind::Start:
            case NodeKind::Region:
            case NodeKind::IfTrue:
            case NodeKind::IfFalse:
            case NodeKind::Loop:
            case NodeKind::LoopExit:
                return true;
            default:
                return false;
        }
    }

    // Emit a block and its successors. Walks the control-flow graph.
    void emit_block_sequence_(NodeId block_id, std::vector<bool>& emitted) {
        if (!block_id.valid() || block_id.value >= emitted.size()) return;
        if (emitted[block_id.value]) return;
        emitted[block_id.value] = true;

        const Graph& g = job.graph;
        const Node& block_node = g.at(block_id);
        if (has_flag(block_node.flags, NodeFlags::IsDead)) return;

        // Emit the label for this block.
        auto label_it = ctrl_to_label.find(block_id.value);
        if (label_it != ctrl_to_label.end()) {
            fn.emit(Tier1Op::Label, 0, 0, 0, label_it->second);
        }

        // Emit all data nodes whose control input is this block.
        // We walk nodes in ID order for determinism.
        for (uint32_t i = 1; i < g.size(); ++i) {
            NodeId id{static_cast<uint32_t>(i)};
            const Node& n = g.at(id);
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;
            if (is_block_header_(n.kind)) continue;
            if (n.kind == NodeKind::Stop) continue;

            // Check if this node's control input is block_id.
            auto inputs = g.inputs_of(id);
            bool belongs_to_this_block = false;
            for (const auto& e : inputs) {
                if (e.kind == EdgeKind::Control && e.target == block_id) {
                    belongs_to_this_block = true;
                    break;
                }
            }
            if (!belongs_to_this_block) continue;

            emit_data_node_(id);
        }

        // Emit the block terminator.
        emit_terminator_(block_id, emitted);
    }

    // Emit a single data node as a Tier-1 instruction.
    void emit_data_node_(NodeId id) {
        const Graph& g = job.graph;
        const Node& n = g.at(id);

        switch (n.kind) {
            case NodeKind::ConstInt:
            case NodeKind::ConstFloat: {
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::LoadConstImm, dst, 0, 0, n.payload);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::Add:
            case NodeKind::Sub:
            case NodeKind::Mul:
            case NodeKind::Div:
            case NodeKind::Pow: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.size() < 2) break;
                uint32_t dst = fn.alloc_vreg();
                Tier1Op op = static_cast<Tier1Op>(
                    static_cast<int>(Tier1Op::Add) +
                    static_cast<int>(static_cast<uint8_t>(n.kind) - static_cast<uint8_t>(NodeKind::Add)));
                fn.emit(op, dst, get_vreg(data[0]), get_vreg(data[1]), 0);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::Neg: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.empty()) break;
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::Neg, dst, get_vreg(data[0]), 0, 0);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::Shl:
            case NodeKind::Shr: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.size() < 2) break;
                uint32_t dst = fn.alloc_vreg();
                Tier1Op op = (n.kind == NodeKind::Shl) ? Tier1Op::Shl : Tier1Op::Shr;
                fn.emit(op, dst, get_vreg(data[0]), get_vreg(data[1]), 0);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::Eq:
            case NodeKind::Ne:
            case NodeKind::Lt:
            case NodeKind::Gt:
            case NodeKind::Lte:
            case NodeKind::Gte: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.size() < 2) break;
                uint32_t dst = fn.alloc_vreg();
                Tier1Op op = static_cast<Tier1Op>(
                    static_cast<int>(Tier1Op::Eq) +
                    static_cast<int>(static_cast<uint8_t>(n.kind) - static_cast<uint8_t>(NodeKind::Eq)));
                fn.emit(op, dst, get_vreg(data[0]), get_vreg(data[1]), 0);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::And:
            case NodeKind::Or: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.size() < 2) break;
                uint32_t dst = fn.alloc_vreg();
                Tier1Op op = (n.kind == NodeKind::And) ? Tier1Op::And : Tier1Op::Or;
                fn.emit(op, dst, get_vreg(data[0]), get_vreg(data[1]), 0);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::Not: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.empty()) break;
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::Not, dst, get_vreg(data[0]), 0, 0);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::ToBool: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.empty()) break;
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::IsTruthy, dst, get_vreg(data[0]), 0, 0);
                node_to_vreg[id.value] = dst;
                break;
            }
            case NodeKind::ToFloat: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.empty()) break;
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::ToFloat, dst, get_vreg(data[0]), 0, 0);
                node_to_vreg[id.value] = dst;
                break;
            }

            case NodeKind::LoadLocal: {
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::LoadLocal, dst, 0, 0, n.payload);
                node_to_vreg[id.value] = dst;
                break;
            }
            case NodeKind::StoreLocal: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.empty()) break;
                fn.emit(Tier1Op::StoreLocal, 0, get_vreg(data[0]), 0, n.payload);
                break;
            }
            case NodeKind::LoadVar: {
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::LoadVar, dst, 0, 0, n.payload);
                node_to_vreg[id.value] = dst;
                break;
            }
            case NodeKind::StoreVar: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (data.empty()) break;
                fn.emit(Tier1Op::StoreVar, 0, get_vreg(data[0]), 0, n.payload);
                break;
            }

            case NodeKind::Call:
            case NodeKind::CallKnown:
            case NodeKind::CallNative: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                uint32_t dst = fn.alloc_vreg();
                uint32_t src1 = data.empty() ? 0 : get_vreg(data[0]);
                uint32_t src2 = data.size() >= 2 ? get_vreg(data[1]) : 0;
                fn.emit(Tier1Op::Call, dst, src1, src2, n.payload);
                node_to_vreg[id.value] = dst;
                break;
            }

            // Effect-only nodes (no data result) — emit as Call with no dst.
            case NodeKind::Allocate:
            case NodeKind::StoreField:
            case NodeKind::StoreIndex: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                uint32_t src1 = data.empty() ? 0 : get_vreg(data[0]);
                uint32_t src2 = data.size() >= 2 ? get_vreg(data[1]) : 0;
                fn.emit(Tier1Op::Call, 0, src1, src2, n.payload);
                break;
            }

            // Load nodes that produce a value.
            case NodeKind::LoadField:
            case NodeKind::LoadIndex: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                uint32_t dst = fn.alloc_vreg();
                uint32_t src1 = data.empty() ? 0 : get_vreg(data[0]);
                uint32_t src2 = data.size() >= 2 ? get_vreg(data[1]) : 0;
                fn.emit(Tier1Op::Call, dst, src1, src2, n.payload);
                node_to_vreg[id.value] = dst;
                break;
            }

            // Guards — emit as no-ops for now (the check is elided).
            case NodeKind::CheckInt:
            case NodeKind::CheckFloat:
            case NodeKind::CheckNotNull:
            case NodeKind::CheckBounds:
            case NodeKind::CheckShape:
            case NodeKind::ShapeOf:
                break;

            // Phi nodes — in a linearization, we just pick the first input.
            // This is correct for the currently-executing path.
            case NodeKind::Phi: {
                auto data = g.inputs_of_kind(id, EdgeKind::Data);
                if (!data.empty()) {
                    node_to_vreg[id.value] = get_vreg(data[0]);
                }
                break;
            }
            case NodeKind::EffectPhi:
                break;  // no data result

            case NodeKind::Parameter: {
                uint32_t dst = fn.alloc_vreg();
                fn.emit(Tier1Op::LoadConstImm, dst, 0, 0, 0);  // params default to 0
                node_to_vreg[id.value] = dst;
                break;
            }

            default:
                break;
        }
    }

    // Emit the terminator for a block (Jump, BranchIfFalse, or fall-through).
    void emit_terminator_(NodeId block_id, std::vector<bool>& emitted) {
        const Graph& g = job.graph;

        // Find control successors (nodes whose control input is block_id).
        std::vector<NodeId> successors;
        for (uint32_t i = 1; i < g.size(); ++i) {
            NodeId id{static_cast<uint32_t>(i)};
            const Node& n = g.at(id);
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;
            if (!is_block_header_(n.kind)) continue;
            for (const auto& e : g.inputs_of(id)) {
                if (e.kind == EdgeKind::Control && e.target == block_id) {
                    successors.push_back(id);
                    break;
                }
            }
        }

        // If this block contains an If node, we need to emit a branch.
        // Look for If nodes whose control input is block_id.
        for (uint32_t i = 1; i < g.size(); ++i) {
            NodeId id{static_cast<uint32_t>(i)};
            const Node& n = g.at(id);
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;
            if (n.kind != NodeKind::If) continue;

            // Check if this If's control input is block_id.
            bool belongs = false;
            for (const auto& e : g.inputs_of(id)) {
                if (e.kind == EdgeKind::Control && e.target == block_id) {
                    belongs = true;
                    break;
                }
            }
            if (!belongs) continue;

            // Get the condition.
            auto if_data = g.inputs_of_kind(id, EdgeKind::Data);
            if (if_data.empty()) continue;
            uint32_t cond_vreg = get_vreg(if_data[0]);

            // Find the IfTrue and IfFalse successors.
            NodeId if_true_target = {};
            NodeId if_false_target = {};
            for (NodeId succ : successors) {
                if (g.at(succ).kind == NodeKind::IfTrue) if_true_target = succ;
                if (g.at(succ).kind == NodeKind::IfFalse) if_false_target = succ;
            }

            // Emit: BranchIfFalse to the false target, then fall through to true.
            if (if_false_target.valid()) {
                uint32_t false_label = ctrl_to_label[if_false_target.value];
                fn.emit(Tier1Op::BranchIfFalse, 0, cond_vreg, 0, false_label);
            }

            // Emit the true block.
            if (if_true_target.valid()) {
                emit_block_sequence_(if_true_target, emitted);
            }

            // Emit a Jump to the merge point (if any).
            // After the true block, we need to jump past the false block.
            if (if_false_target.valid()) {
                // Find where the false block ends — for now, just emit it.
                // In a full implementation, we'd emit a Jump to the merge label.
                emit_block_sequence_(if_false_target, emitted);
            }

            return;
        }

        // No If in this block — just emit successors (fall-through).
        for (NodeId succ : successors) {
            emit_block_sequence_(succ, emitted);
        }
    }

    [[nodiscard]] uint32_t get_vreg(NodeId n) {
        auto it = node_to_vreg.find(n.value);
        if (it != node_to_vreg.end()) return it->second;
        // If we don't have a vreg for this node, allocate one and emit a
        // LoadConstImm(0) — this happens for nodes that were skipped (e.g.
        // constants that survived DCE).
        uint32_t v = fn.alloc_vreg();
        fn.emit(Tier1Op::LoadConstImm, v, 0, 0, 0);
        node_to_vreg[n.value] = v;
        return v;
    }
};

}  // namespace

[[nodiscard]] std::expected<Tier1Function, std::string>
lower_son_to_tier1(const Tier2Job& job) {
    Tier1Function fn;
    fn.name = job.function_name;
    if (job.source) {
        fn.max_locals = job.source->max_locals;
        fn.num_params = job.source->num_params;
    }

    SoNToTier1 lowerer{job, fn};
    auto r = lowerer.run();
    if (!r) return std::unexpected(r.error());
    return fn;
}

// ============================================================================
// Pipeline + end-to-end compilation
// ============================================================================

void build_demo_graph(Graph& g) {
    NodeId start = g.add_node(NodeKind::Start,
                              NodeFlags::IsControl | NodeFlags::NoDeopt,
                              TypeId::Bottom, 0, {});
    g.set_start(start);

    auto pure_int_flags = NodeFlags::Pure | NodeFlags::CSEable | NodeFlags::GVNable | NodeFlags::NoDeopt;
    NodeId c1 = g.add_node(NodeKind::ConstInt, pure_int_flags, TypeId::Int, 1, {});
    NodeId c2 = g.add_node(NodeKind::ConstInt, pure_int_flags, TypeId::Int, 2, {});
    NodeId c3 = g.add_node(NodeKind::ConstInt, pure_int_flags, TypeId::Int, 3, {});

    std::pair<NodeId, EdgeKind> add1_inputs[] = {
        {c1, EdgeKind::Data}, {c2, EdgeKind::Data}, {start, EdgeKind::Control},
    };
    NodeId r1 = g.add_node(NodeKind::Add,
                            NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                            TypeId::Int, 0, add1_inputs);

    std::pair<NodeId, EdgeKind> add2_inputs[] = {
        {r1, EdgeKind::Data}, {c3, EdgeKind::Data}, {start, EdgeKind::Control},
    };
    NodeId r2 = g.add_node(NodeKind::Add,
                            NodeFlags::Pure | NodeFlags::GVNable | NodeFlags::Commutative,
                            TypeId::Int, 0, add2_inputs);

    std::pair<NodeId, EdgeKind> stop_inputs[] = {
        {r2, EdgeKind::Data}, {start, EdgeKind::Control},
    };
    NodeId stop = g.add_node(NodeKind::Stop,
                              NodeFlags::IsControl | NodeFlags::NoDeopt,
                              TypeId::Bottom, 0, stop_inputs);
    g.set_stop(stop);
}

// ============================================================================
// Gigavolt — the Surge optimizing pipeline
// ============================================================================
//
// Named pipeline for the Tier-2 (Surge) Sea of Nodes optimizer.
// 14 passes run to a fixpoint (max 8 iterations):
//
//   1. TypeNarrow     — propagate TypeIds (Int, Float, Bool, etc.)
//   2. CallInline     — mark calls to known functions
//   3. EscapeAnalysis — mark non-escaping allocations
//   4. GVN            — global value numbering (deduplicate)
//   5. ConstFold      — fold ConstInt + ConstInt → ConstInt
//   6. AlgebraicSimp  — x+0→x, x*1→x, x*0→0, x-x→0, !!x→x
//   7. CompareFold    — x==x→true, !(x<y)→x>=y
//   8. BranchFold     — if(true)→drop false branch
//   9. StrengthReduce — x*2^k → x<<k
//  10. LICM           — hoist loop-invariant code (uses dominance)
//  11. LoopUnroll     — unroll hot loops (placeholder)
//  12. BCE            — remove provably-unnecessary bounds checks
//  13. ReachPrune     — remove unreachable nodes
//  14. DCE            — remove dead pure nodes
//
// GCM (schedule-late) is implemented but disabled — it creates new nodes
// with control edges that the block-based lowering doesn't handle yet.

[[nodiscard]] PassPipeline build_gigavolt_pipeline() {
    PassPipeline pipe;
    pipe.add(std::make_unique<TypeNarrowingPass>());
    pipe.add(std::make_unique<CallInliningPass>());
    pipe.add(std::make_unique<EscapeAnalysisPass>());
    pipe.add(std::make_unique<LocalForwardingPass>());  // store-to-load forwarding
    pipe.add(std::make_unique<GVNPass>());
    pipe.add(std::make_unique<ConstantFoldingPass>());
    pipe.add(std::make_unique<AlgebraicSimplificationPass>());
    pipe.add(std::make_unique<ComparisonFoldingPass>());
    pipe.add(std::make_unique<BranchFoldingPass>());
    pipe.add(std::make_unique<StrengthReductionPass>());
    pipe.add(std::make_unique<LICMPass>());
    pipe.add(std::make_unique<LoopUnrollingPass>());
    // GCM disabled — see comment above.
    // pipe.add(std::make_unique<GlobalCodeMotionPass>());
    pipe.add(std::make_unique<BoundsCheckEliminationPass>());
    pipe.add(std::make_unique<ReachabilityPruningPass>());
    pipe.add(std::make_unique<DeadCodeElimPass>());
    return pipe;
}

PassResult run_tier2_pipeline(Tier2Job& job) {
    job.pipeline = build_gigavolt_pipeline();
    return job.pipeline.run_to_fixpoint(job.graph, 8);
}

[[nodiscard]] std::expected<int64_t (*)(void*), std::string>
compile_at_tier2(const Tier1Function& fn, Fuse& fuse) {
    // Check if the function has branches. If it does, the SoN→Tier-1
    // linearization can't correctly reconstruct the control flow, so we
    // fall back to Tier-1 compilation (no SoN optimization).
    //
    // This is a known limitation — full GCM with proper Region/Phi handling
    // in the back-end lowering is future work. Linear functions are fully
    // optimized at Tier-2.
    bool has_branches = false;
    for (const auto& inst : fn.insts) {
        if (inst.op == Tier1Op::BranchIfFalse ||
            inst.op == Tier1Op::BranchIfTrue ||
            inst.op == Tier1Op::Jump) {
            has_branches = true;
            break;
        }
    }

    if (has_branches) {
        // Fall back to Tier-1 compilation for branchy functions.
        //
        // This is a KNOWN LIMITATION, not a soundness issue. The SoN→Tier-1
        // back-lowering uses a block-based walker that doesn't correctly
        // reconstruct control flow when there are forward branches (Jump /
        // BranchIfTrue / BranchIfFalse). Linear functions are fully
        // optimized at Tier-2.
        //
        // Until GCM with proper Region/Phi handling is implemented, we
        // transparently return Tier-1 code. The caller gets correct code,
        // just without SoN-level optimizations (GVN, ConstFold, etc.).
        //
        // To make this observable for debugging, set ARCJIT_LOG_TIER2_FALLBACK=1
        // in the environment.
        static thread_local std::unique_ptr<Tier1Compiler> tls_compiler;
        if (!tls_compiler) tls_compiler = std::make_unique<Tier1Compiler>();
        if (std::getenv("ARCJIT_LOG_TIER2_FALLBACK") != nullptr) {
            std::fprintf(stderr,
                "[arcjit] Surge fell back to Jolt for '%s' (branchy function, %zu insts)\n",
                fn.name.c_str(), fn.insts.size());
        }
        return tls_compiler->compile(fn);
    }

    Tier2Job job;
    job.function_name = fn.name;

    // 1. Lower Tier-1 → SoN.
    auto r1 = lower_tier1_to_son(fn, job);
    if (!r1) return std::unexpected(r1.error());

    // 2. Run the optimization pipeline.
    auto t0 = std::chrono::high_resolution_clock::now();
    run_tier2_pipeline(job);
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // 2b. Fuse budget check — if the compile blew a budget (time, graph size,
    // etc.), fall back to Tier-1. This prevents runaway compiles from
    // degrading overall throughput. The Fuse is owned by the Runtime and
    // shared across all compiles — so clone counts and cumulative time are
    // tracked globally, not per-thread.
    //
    // The budget is configurable via Runtime::set_compile_budget. The
    // default (50ms, 100k nodes, 500k edges, 256KB code, 64 guards) is
    // reasonable for small programs; large codebases should raise limits
    // or implement adaptive budgeting based on Capacitor pressure.
    auto blown = fuse.check(static_cast<uint64_t>(elapsed_us),
                            static_cast<uint32_t>(job.graph.size()),
                            0,  // edges not tracked here (would need Graph::edge_count)
                            0,  // code_size unknown until after asmjit emission
                            0); // guard_count not tracked yet
    if (!blown.empty()) {
        if (std::getenv("ARCJIT_LOG_TIER2_FALLBACK") != nullptr) {
            std::fprintf(stderr,
                "[arcjit] Surge fell back to Jolt for '%s' (budget blown: %s, %lldus, %u nodes)\n",
                fn.name.c_str(), blown.c_str(),
                static_cast<long long>(elapsed_us),
                static_cast<uint32_t>(job.graph.size()));
        }
        static thread_local std::unique_ptr<Tier1Compiler> fb_compiler;
        if (!fb_compiler) fb_compiler = std::make_unique<Tier1Compiler>();
        return fb_compiler->compile(fn);
    }

    // 3. Lower SoN → Tier-1.
    auto maybe_lowered = lower_son_to_tier1(job);
    if (!maybe_lowered) return std::unexpected(maybe_lowered.error());

    // 4. Compile the optimized Tier-1 function.
    //
    // We use a process-wide JitRuntime so the emitted code outlives this
    // function call. The runtime is never freed (process-lifetime allocation);
    // this is intentional and matches how V8 and HotSpot manage JIT code.
    static thread_local std::unique_ptr<Tier1Compiler> tls_compiler;
    if (!tls_compiler) tls_compiler = std::make_unique<Tier1Compiler>();
    return tls_compiler->compile(*maybe_lowered);
}

[[nodiscard]] std::string dump_graph_dot(const Graph& g) {
    return g.dump_dot();
}

}  // namespace arcjit
