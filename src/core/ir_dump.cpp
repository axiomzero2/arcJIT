// SPDX-License-Identifier: MIT
#include "core/ir_dump.h"

#include <format>
#include <print>

namespace arcjit {

std::string_view type_name(TypeId t) noexcept {
    switch (t) {
        case TypeId::Top:            return "Top";
        case TypeId::Bottom:         return "Bottom";
        case TypeId::Int:            return "Int";
        case TypeId::Float:          return "Float";
        case TypeId::Bool:           return "Bool";
        case TypeId::Null:           return "Null";
        case TypeId::Undef:          return "Undef";
        case TypeId::String:         return "String";
        case TypeId::List:           return "List";
        case TypeId::Function:       return "Function";
        case TypeId::NativeFunction: return "NativeFunction";
        case TypeId::Class:          return "Class";
        case TypeId::Instance:       return "Instance";
        case TypeId::Object:         return "Object";
        case TypeId::IntOrFloat:     return "IntOrFloat";
        case TypeId::NonNullObj:     return "NonNullObj";
    }
    return "?";
}

std::string_view edge_kind_name(EdgeKind k) noexcept {
    switch (k) {
        case EdgeKind::Data:       return "data";
        case EdgeKind::Control:    return "ctrl";
        case EdgeKind::Effect:     return "effect";
        case EdgeKind::FrameState: return "fs";
    }
    return "?";
}

std::string flags_string(NodeFlags f) noexcept {
    std::string s;
    auto add = [&](const char* name, NodeFlags mask) {
        if (has_flag(f, mask)) {
            if (!s.empty()) s += ", ";
            s += name;
        }
    };
    add("pure", NodeFlags::Pure);
    add("cse", NodeFlags::CSEable);
    add("gvn", NodeFlags::GVNable);
    add("commut", NodeFlags::Commutative);
    add("nothrow", NodeFlags::NoThrow);
    add("nodeopt", NodeFlags::NoDeopt);
    add("hasfs", NodeFlags::HasFrameState);
    add("alloc", NodeFlags::IsAllocated);
    add("pinned", NodeFlags::IsPinned);
    add("guard", NodeFlags::IsGuard);
    add("control", NodeFlags::IsControl);
    add("effect", NodeFlags::IsEffect);
    add("dead", NodeFlags::IsDead);
    return s;
}

// Format the inputs list: "data:n2, data:n3, ctrl:n1"
static std::string format_inputs(const Graph& g, NodeId id) {
    auto inputs = g.inputs_of(id);
    if (inputs.empty()) return "";

    std::string s;
    bool first = true;
    for (const auto& e : inputs) {
        if (!first) s += ", ";
        first = false;
        s += edge_kind_name(e.kind);
        s += ":n";
        s += std::to_string(e.target.value);
    }
    return s;
}

std::string dump_node(const Graph& g, NodeId id) {
    const Node& n = g.at(id);
    std::string s;

    if (has_flag(n.flags, NodeFlags::IsDead)) {
        s += "; dead: ";
    }

    s += "n";
    s += std::to_string(id.value);
    s += " = ";
    s += node_kind_name(n.kind);

    // Payload in parens, if non-zero.
    if (n.payload != 0) {
        s += "(";
        s += std::to_string(n.payload);
        s += ")";
    }

    // Inputs.
    std::string in = format_inputs(g, id);
    if (!in.empty()) {
        s += "(";
        s += in;
        s += ")";
    }

    // Annotations.
    std::string flags = flags_string(n.flags);
    if (!flags.empty() || n.type != TypeId::Top) {
        s += "  [";
        bool need_comma = false;
        if (n.type != TypeId::Top) {
            s += "type=";
            s += type_name(n.type);
            need_comma = true;
        }
        if (!flags.empty()) {
            if (need_comma) s += ", ";
            s += flags;
        }
        s += "]";
    }

    // Use count (for debugging).
    s += "  ; uses=";
    s += std::to_string(n.use_count);

    return s;
}

std::string dump_graph_text(const Graph& g) {
    std::string s;
    s.reserve(g.size() * 80);

    // Header.
    s += std::format("// graph: {} nodes, start=n{}, stop=n{}\n",
                     g.size() - 1,  // minus sentinel
                     g.start().valid() ? g.start().value : 0,
                     g.stop().valid() ? g.stop().value : 0);

    for (uint32_t i = 1; i < g.size(); ++i) {
        s += dump_node(g, NodeId{i});
        s += "\n";
    }

    return s;
}

std::string dump_graph_with_stats(const Graph& g) {
    // Count live/dead nodes by kind.
    size_t live = 0, dead = 0;
    for (uint32_t i = 1; i < g.size(); ++i) {
        const Node& n = g.at(NodeId{i});
        if (has_flag(n.flags, NodeFlags::IsDead)) dead++;
        else live++;
    }

    std::string s;
    s += std::format("// === Graph stats ===\n");
    s += std::format("// total nodes: {}\n", g.size() - 1);
    s += std::format("// live:  {}\n", live);
    s += std::format("// dead:  {}\n", dead);
    s += "\n";
    s += dump_graph_text(g);
    return s;
}

}  // namespace arcjit
