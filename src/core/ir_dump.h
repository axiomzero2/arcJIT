// SPDX-License-Identifier: MIT
// arcJIT — Textual IR dumper.
//
// Produces a human-readable, diffable dump of a Sea of Nodes graph. The
// format is designed to be:
//   - Stable (small IR changes produce small diffs)
//   - Annotated (every node shows kind, type, flags, payload, inputs)
//   - Eventually round-trippable (parser planned for replay)
//
// Format:
//   n<id> = <Kind>(<payload>) <inputs> [type=<T>, <flags>]
//
// Inputs are listed as `data:n2, ctrl:n1, effect:n3, fs:n4`.
//
// Example:
//   n1 = Start
//   n2 = ConstInt(1)                             [type=Int, pure, gvnable]
//   n3 = ConstInt(2)                             [type=Int, pure, gvnable]
//   n4 = Add(data:n2, data:n3, ctrl:n1)          [type=Int, pure, gvnable, commutative]
//   n5 = Stop(data:n4, ctrl:n1)                  [control]
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "core/graph.h"

namespace arcjit {

// Dump a graph as text. Dead nodes are prefixed with `; dead:`.
[[nodiscard]] std::string dump_graph_text(const Graph& g);

// Dump a single node as text (for debugging a specific node).
[[nodiscard]] std::string dump_node(const Graph& g, NodeId id);

// Dump the graph with a header line showing stats.
[[nodiscard]] std::string dump_graph_with_stats(const Graph& g);

// --- Type name helpers (used by the dumper) --------------------------------
[[nodiscard]] std::string_view type_name(TypeId t) noexcept;
[[nodiscard]] std::string_view edge_kind_name(EdgeKind k) noexcept;
[[nodiscard]] std::string      flags_string(NodeFlags f) noexcept;

}  // namespace arcjit
