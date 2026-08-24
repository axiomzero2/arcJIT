// SPDX-License-Identifier: MIT
#include "core/verifier.h"

#include <cstdlib>
#include <print>

#include "core/ir_dump.h"

namespace arcjit {

[[nodiscard]] std::vector<VerifierError>
verify_graph(const Graph& g, VerifierOptions opts) {
    std::vector<VerifierError> errors;

    // Helper to add an error.
    auto err = [&](NodeId n, std::string_view check, std::string msg) {
        errors.push_back({n, std::string(check), std::move(msg)});
    };

    // Fused single-pass verification.
    //
    // The previous implementation did 5 separate full-graph walks for
    // checks 1-5. Each walk was O(N × avg_inputs). For a 200-node graph
    // with avg 3 inputs, that's 5 × 200 × 3 = 3000 node-visits.
    //
    // Fused into a single walk: 200 × 3 = 600 visits. 5x fewer.
    // Debug-only (verifier runs under #ifndef NDEBUG), but brutal on
    // test suite runtime when many graphs are verified.
    for (uint32_t i = 1; i < g.size(); ++i) {
        NodeId id{static_cast<uint32_t>(i)};
        const Node& n = g.at(id);
        if (has_flag(n.flags, NodeFlags::IsDead)) {
            // Check 4: No dead nodes with live users.
            if (opts.check_no_dead_with_users && n.use_count > 0) {
                err(id, "no_dead_with_users",
                    std::format("dead node n{} still has {} users", i, n.use_count));
            }
            continue;
        }

        // Walk this node's input edges once, running all edge-relevant
        // checks in the same loop.
        bool is_effectful = has_flag(n.flags, NodeFlags::IsEffect);
        bool is_pure      = has_flag(n.flags, NodeFlags::Pure);
        bool has_effect_input = false;

        for (const auto& e : g.inputs_of(id)) {
            // Check 1: No dangling NodeIds.
            if (!e.target.valid()) {
                err(id, "no_dangling_edges",
                    std::format("input edge has invalid target (kind={})",
                                edge_kind_name(e.kind)));
                continue;
            }
            if (e.target.value >= g.size()) {
                err(id, "no_dangling_edges",
                    std::format("input edge points to n{} which is out of range (graph has {} nodes)",
                                e.target.value, g.size()));
                continue;
            }

            // Track effect-input presence for Check 2.
            if (e.kind == EdgeKind::Effect) {
                has_effect_input = true;
                // Check 3: Pure nodes have no effect edges.
                if (opts.check_pure_no_effect_edges && is_pure) {
                    err(id, "pure_no_effect_edges",
                        std::format("pure node {} has an effect input (n{})",
                                    node_kind_name(n.kind), e.target.value));
                }
            }

            // Check 5: Use-def consistency — live node uses a dead node.
            if (opts.check_use_def_consistency) {
                const Node& producer = g.at(e.target);
                if (has_flag(producer.flags, NodeFlags::IsDead) && producer.use_count > 0) {
                    err(id, "use_def_consistency",
                        std::format("node n{} uses dead node n{}", i, e.target.value));
                }
            }
        }

        // Check 2: Effect chain continuity — every effectful node (except
        // Start) has at least one effect input.
        if (opts.check_effect_chain && is_effectful && n.kind != NodeKind::Start && !has_effect_input) {
            err(id, "effect_chain_continuity",
                std::format("{} node has no effect input", node_kind_name(n.kind)));
        }
    }

    // 6. Start has no inputs.
    if (g.start().valid()) {
        const Node& start = g.at(g.start());
        if (start.input_count != 0) {
            err(g.start(), "start_no_inputs",
                std::format("Start node has {} inputs (expected 0)", start.input_count));
        }
    }

    // 7. Stop has exactly one data input.
    if (g.stop().valid()) {
        auto stop_data = g.inputs_of_kind(g.stop(), EdgeKind::Data);
        if (stop_data.size() != 1) {
            err(g.stop(), "stop_one_data_input",
                std::format("Stop node has {} data inputs (expected 1)", stop_data.size()));
        }
    }

    return errors;
}

[[nodiscard]] std::expected<void, VerifierError>
verify_graph_strict(const Graph& g, VerifierOptions opts) {
    auto errors = verify_graph(g, opts);
    if (errors.empty()) return {};
    return std::unexpected(errors[0]);
}

void verify_or_die(const Graph& g, std::string_view context, VerifierOptions opts) {
    auto errors = verify_graph(g, opts);
    if (errors.empty()) return;

    if (opts.dump_graph_on_failure) {
        std::println(stderr, "=== Graph verifier failure {}===",
                     context.empty() ? "" : std::format("(context: {}) ", context));
        std::println(stderr, "{}", dump_graph_with_stats(g));
        std::println(stderr, "=== Errors ===");
    }

    for (const auto& e : errors) {
        std::println(stderr, "  [{}] n{}: {}",
                     e.check,
                     e.node.valid() ? static_cast<uint64_t>(e.node.value) : 0,
                     e.message);
    }

    std::abort();
}

}  // namespace arcjit
