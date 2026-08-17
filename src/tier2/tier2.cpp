// SPDX-License-Identifier: MIT
#include "tier2/tier2.h"

#include <algorithm>
#include <expected>
#include <format>
#include <print>
#include <unordered_map>

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
    std::unordered_map<uint32_t, NodeId> vreg_to_node;

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
                    vreg_to_node[inst.dst] = n;
                    break;
                }
                case Tier1Op::LoadConst: {
                    // For the scaffold we treat LoadConst the same as LoadConstImm
                    // (it loads an immediate). Real Tier-2 would load from the
                    // constant pool.
                    auto n = g.add_node(NodeKind::ConstInt,
                                         NodeFlags::Pure | NodeFlags::CSEable | NodeFlags::GVNable | NodeFlags::NoDeopt,
                                         TypeId::Int, inst.payload, {});
                    vreg_to_node[inst.dst] = n;
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
                    vreg_to_node[inst.dst] = n;
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
                    vreg_to_node[inst.dst] = n;
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
                    vreg_to_node[inst.dst] = lookup_vreg(inst.src1);
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
                    vreg_to_node[inst.dst] = n;
                    break;
                }

                case Tier1Op::Neg: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::Neg,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Int, 0, inputs);
                    vreg_to_node[inst.dst] = n;
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
                    vreg_to_node[inst.dst] = n;
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
                    vreg_to_node[inst.dst] = n;
                    break;
                }
                case Tier1Op::Not: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::Not,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Bool, 0, inputs);
                    vreg_to_node[inst.dst] = n;
                    break;
                }

                case Tier1Op::IsTruthy: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::ToBool,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Bool, 0, inputs);
                    vreg_to_node[inst.dst] = n;
                    break;
                }
                case Tier1Op::ToFloat: {
                    NodeId a = lookup_vreg(inst.src1);
                    std::pair<NodeId, EdgeKind> inputs[] = {{a, EdgeKind::Data}};
                    auto n = g.add_node(NodeKind::ToFloat,
                                         NodeFlags::Pure | NodeFlags::GVNable,
                                         TypeId::Float, 0, inputs);
                    vreg_to_node[inst.dst] = n;
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
                    if (inst.dst != 0) vreg_to_node[inst.dst] = n;
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
        auto it = vreg_to_node.find(v);
        if (it != vreg_to_node.end()) return it->second;
        // Synthesize a fresh ConstInt(0) if undefined.
        auto n = g.add_node(NodeKind::ConstInt,
                             NodeFlags::Pure | NodeFlags::GVNable,
                             TypeId::Int, 0, {});
        vreg_to_node[v] = n;
        return n;
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

    explicit SoNToTier1(const Tier2Job& j, Tier1Function& f) : job(j), fn(f) {}

    [[nodiscard]] std::expected<void, std::string> run() {
        // Walk nodes in ID order. Skip the Start node and dead nodes.
        // For each data node, emit the corresponding Tier1Inst.
        for (uint32_t i = 1; i < job.graph.size(); ++i) {
            NodeId id{static_cast<uint32_t>(i)};
            const Node& n = job.graph.at(id);
            if (has_flag(n.flags, NodeFlags::IsDead)) continue;

            switch (n.kind) {
                case NodeKind::Start:
                case NodeKind::Region:
                case NodeKind::If:
                case NodeKind::IfTrue:
                case NodeKind::IfFalse:
                case NodeKind::Stop:
                case NodeKind::FrameState:
                case NodeKind::Deopt:
                    // Control-only nodes don't produce Tier1Insts in our
                    // linearization.
                    break;

                case NodeKind::ConstInt:
                case NodeKind::ConstFloat: {
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::LoadConstImm, dst, 0, 0, n.payload);
                    node_to_vreg[i] = dst;
                    break;
                }

                case NodeKind::Add:
                case NodeKind::Sub:
                case NodeKind::Mul:
                case NodeKind::Div:
                case NodeKind::Pow: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.size() < 2) break;
                    uint32_t dst = fn.alloc_vreg();
                    Tier1Op op = static_cast<Tier1Op>(
                        static_cast<int>(Tier1Op::Add) +
                        static_cast<int>(static_cast<uint8_t>(n.kind) - static_cast<uint8_t>(NodeKind::Add)));
                    fn.emit(op, dst, get_vreg(data[0]), get_vreg(data[1]), 0);
                    node_to_vreg[i] = dst;
                    break;
                }

                case NodeKind::Neg: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.empty()) break;
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::Neg, dst, get_vreg(data[0]), 0, 0);
                    node_to_vreg[i] = dst;
                    break;
                }

                case NodeKind::Eq:
                case NodeKind::Ne:
                case NodeKind::Lt:
                case NodeKind::Gt:
                case NodeKind::Lte:
                case NodeKind::Gte: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.size() < 2) break;
                    uint32_t dst = fn.alloc_vreg();
                    Tier1Op op = static_cast<Tier1Op>(
                        static_cast<int>(Tier1Op::Eq) +
                        static_cast<int>(static_cast<uint8_t>(n.kind) - static_cast<uint8_t>(NodeKind::Eq)));
                    fn.emit(op, dst, get_vreg(data[0]), get_vreg(data[1]), 0);
                    node_to_vreg[i] = dst;
                    break;
                }

                case NodeKind::And:
                case NodeKind::Or: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.size() < 2) break;
                    uint32_t dst = fn.alloc_vreg();
                    Tier1Op op = (n.kind == NodeKind::And) ? Tier1Op::And : Tier1Op::Or;
                    fn.emit(op, dst, get_vreg(data[0]), get_vreg(data[1]), 0);
                    node_to_vreg[i] = dst;
                    break;
                }

                case NodeKind::Not: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.empty()) break;
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::Not, dst, get_vreg(data[0]), 0, 0);
                    node_to_vreg[i] = dst;
                    break;
                }

                case NodeKind::ToBool: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.empty()) break;
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::IsTruthy, dst, get_vreg(data[0]), 0, 0);
                    node_to_vreg[i] = dst;
                    break;
                }
                case NodeKind::ToFloat: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.empty()) break;
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::ToFloat, dst, get_vreg(data[0]), 0, 0);
                    node_to_vreg[i] = dst;
                    break;
                }

                case NodeKind::LoadLocal: {
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::LoadLocal, dst, 0, 0, n.payload);
                    node_to_vreg[i] = dst;
                    break;
                }
                case NodeKind::StoreLocal: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.empty()) break;
                    fn.emit(Tier1Op::StoreLocal, 0, get_vreg(data[0]), 0, n.payload);
                    break;
                }
                case NodeKind::LoadVar: {
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::LoadVar, dst, 0, 0, n.payload);
                    node_to_vreg[i] = dst;
                    break;
                }
                case NodeKind::StoreVar: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    if (data.empty()) break;
                    fn.emit(Tier1Op::StoreVar, 0, get_vreg(data[0]), 0, n.payload);
                    break;
                }

                case NodeKind::Call: {
                    auto data = job.graph.inputs_of_kind(id, EdgeKind::Data);
                    uint32_t dst = fn.alloc_vreg();
                    uint32_t src1 = data.empty() ? 0 : get_vreg(data[0]);
                    uint32_t src2 = data.size() >= 2 ? get_vreg(data[1]) : 0;
                    fn.emit(Tier1Op::Call, dst, src1, src2, n.payload);
                    node_to_vreg[i] = dst;
                    break;
                }

                // Other node kinds not yet supported in lowering-back.
                default:
                    break;
            }
        }

        // Emit a Return at the end, sourced from the Stop node's data input.
        if (job.graph.stop().valid()) {
            auto stop_data = job.graph.inputs_of_kind(job.graph.stop(), EdgeKind::Data);
            if (!stop_data.empty()) {
                uint32_t v = get_vreg(stop_data[0]);
                fn.emit(Tier1Op::Return, 0, v, 0, 0);
            } else {
                // Return 0.
                uint32_t z = fn.alloc_vreg();
                fn.emit(Tier1Op::LoadConstImm, z, 0, 0, 0);
                fn.emit(Tier1Op::Return, 0, z, 0, 0);
            }
        }

        return {};
    }

private:
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

PassResult run_tier2_pipeline(Tier2Job& job) {
    // Pipeline order matters:
    //   1. TypeNarrowing — establish types first (other passes use them)
    //   2. GVN — deduplicate before other passes can create dupes
    //   3. ConstantFolding — fold constants
    //   4. AlgebraicSimplification — simplify identities
    //   5. ComparisonFolding — fold comparisons
    //   6. BranchFolding — fold constant branches
    //   7. StrengthReduction — replace expensive ops (future)
    //   8. DCE — clean up dead nodes
    job.pipeline.add(std::make_unique<TypeNarrowingPass>());
    job.pipeline.add(std::make_unique<GVNPass>());
    job.pipeline.add(std::make_unique<ConstantFoldingPass>());
    job.pipeline.add(std::make_unique<AlgebraicSimplificationPass>());
    job.pipeline.add(std::make_unique<ComparisonFoldingPass>());
    job.pipeline.add(std::make_unique<BranchFoldingPass>());
    job.pipeline.add(std::make_unique<StrengthReductionPass>());
    job.pipeline.add(std::make_unique<DeadCodeElimPass>());
    return job.pipeline.run_to_fixpoint(job.graph, 8);
}

[[nodiscard]] std::expected<int64_t (*)(void*), std::string>
compile_at_tier2(const Tier1Function& fn) {
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
        static thread_local std::unique_ptr<Tier1Compiler> tls_compiler;
        if (!tls_compiler) tls_compiler = std::make_unique<Tier1Compiler>();
        return tls_compiler->compile(fn);
    }

    Tier2Job job;
    job.function_name = fn.name;

    // 1. Lower Tier-1 → SoN.
    auto r1 = lower_tier1_to_son(fn, job);
    if (!r1) return std::unexpected(r1.error());

    // 2. Run the optimization pipeline.
    run_tier2_pipeline(job);

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
