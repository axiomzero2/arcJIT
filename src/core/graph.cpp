// SPDX-License-Identifier: MIT
#include "graph.h"

#include <format>
#include <string>

namespace arcjit {

std::string Graph::dump_dot() const {
    std::string out;
    out.reserve(nodes_.size() * 32);
    out += "digraph G {\n";
    out += "  rankdir=TB;\n";
    out += "  node [shape=record, fontname=Courier];\n";

    for (uint32_t i = 1; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;

        const char* color = "white";
        if (has_flag(n.flags, NodeFlags::IsControl)) color = "lightblue";
        else if (has_flag(n.flags, NodeFlags::IsEffect)) color = "lightyellow";
        else if (has_flag(n.flags, NodeFlags::IsGuard)) color = "salmon";

        out += std::format("  n{} [label=\"{}#{}|uses={}\", style=filled, fillcolor={}];\n",
                           i, node_kind_name(n.kind), i, n.use_count, color);
    }

    // Edges
    for (uint32_t i = 1; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        if (has_flag(n.flags, NodeFlags::IsDead)) continue;
        for (uint32_t j = 0; j < n.input_count; ++j) {
            const Edge& e = edges_[n.first_input + j];
            if (!e.target.valid()) continue;
            const char* style = (e.kind == EdgeKind::Control) ? "dashed"
                            : (e.kind == EdgeKind::Effect)   ? "dotted"
                            :                                   "solid";
            out += std::format("  n{} -> n{} [style={}];\n", e.target.value, i, style);
        }
    }

    out += "}\n";
    return out;
}

}  // namespace arcjit
