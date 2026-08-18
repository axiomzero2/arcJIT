// SPDX-License-Identifier: MIT
// arcJIT — Fuse: Compile Budget Engine
//
// Enforces limits on compile time, memory, graph size, pass iterations,
// code clones, and deopt metadata. When a budget blows, compilation stops
// or falls back to a lower tier.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace arcjit {

struct CompileBudget {
    // Time budget (microseconds).
    uint64_t max_compile_time_us = 50000;  // 50ms

    // Memory budget (bytes).
    uint64_t max_compile_memory  = 64 * 1024 * 1024;  // 64 MB

    // Graph budget.
    uint32_t max_graph_nodes     = 100000;
    uint32_t max_graph_edges     = 500000;

    // Pass budget.
    uint32_t max_pass_iterations = 16;
    uint32_t max_passes          = 32;

    // Clone budget.
    uint32_t max_clones_per_function = 4;
    uint32_t max_total_clones        = 256;

    // Code size budget.
    uint32_t max_code_size_bytes = 256 * 1024;  // 256 KB per function

    // Guard budget.
    uint32_t max_guards          = 64;
    uint32_t max_deopt_sites     = 128;
};

class Fuse {
public:
    explicit Fuse(CompileBudget budget = {}) : budget_(budget) {}

    // Check if we've blown a budget. Returns the name of the blown budget,
    // or empty string if all budgets are OK.
    [[nodiscard]] std::string check(uint64_t elapsed_us, uint32_t graph_nodes,
                                     uint32_t graph_edges, uint32_t code_size,
                                     uint32_t guard_count) const {
        if (elapsed_us > budget_.max_compile_time_us)
            return "compile_time";
        if (graph_nodes > budget_.max_graph_nodes)
            return "graph_nodes";
        if (graph_edges > budget_.max_graph_edges)
            return "graph_edges";
        if (code_size > budget_.max_code_size_bytes)
            return "code_size";
        if (guard_count > budget_.max_guards)
            return "guard_count";
        return {};
    }

    // Record a clone creation. Returns false if clone budget exceeded.
    bool register_clone(uint32_t function_id) {
        auto& count = clone_counts_[function_id];
        if (count >= budget_.max_clones_per_function) return false;
        if (total_clones_ >= budget_.max_total_clones) return false;
        count++;
        total_clones_++;
        return true;
    }

    [[nodiscard]] const CompileBudget& budget() const noexcept { return budget_; }
    [[nodiscard]] uint32_t total_clones() const noexcept { return total_clones_; }

    [[nodiscard]] std::string dump() const;

private:
    CompileBudget budget_;
    std::unordered_map<uint32_t, uint32_t> clone_counts_;
    uint32_t total_clones_ = 0;
};

Fuse& global_fuse();

}  // namespace arcjit
