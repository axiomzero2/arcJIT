// SPDX-License-Identifier: MIT
// arcJIT — Graph verifier.
//
// Per Rule 42, the verifier runs after every pass in debug builds. It
// checks invariants that, if violated, indicate a pass bug:
//
//   1. No dangling NodeIds — every edge points to a live node.
//   2. Effect chain continuity — every effectful node has an effect input.
//   3. Use-def consistency — use lists match input lists (bidirectional).
//   4. No dead nodes with live users.
//   5. FrameState attached to every guard.
//   6. Start has no inputs.
//   7. Stop has exactly one data input (the return value).
//   8. Pure nodes have no effect edges.
//
// On failure, the verifier returns a VerifierError with a message and
// dumps the offending graph to stderr.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "core/graph.h"

namespace arcjit {

struct VerifierError {
    NodeId      node;        // the node that failed (invalid if 0)
    std::string check;       // name of the failed check
    std::string message;     // human-readable details
};

struct VerifierOptions {
    bool check_effect_chain      = true;
    bool check_use_def_consistency = true;
    bool check_no_dead_with_users  = true;
    bool check_guard_has_frame_state = false;  // disabled until FrameState is wired
    bool check_pure_no_effect_edges = true;
    bool dump_graph_on_failure  = true;
};

// Verify the graph. Returns a list of errors (empty if OK).
[[nodiscard]] std::vector<VerifierError>
verify_graph(const Graph& g, VerifierOptions opts = {});

// Verify and return a single error (the first one) for convenience.
// Returns std::nullopt (via expected) if the graph is valid.
[[nodiscard]] std::expected<void, VerifierError>
verify_graph_strict(const Graph& g, VerifierOptions opts = {});

// Convenience: verify or abort (for use in debug builds).
// Dumps the graph and the error to stderr before aborting.
void verify_or_die(const Graph& g, std::string_view context = "",
                   VerifierOptions opts = {});

}  // namespace arcjit
