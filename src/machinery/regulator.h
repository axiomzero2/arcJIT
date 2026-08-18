// SPDX-License-Identifier: MIT
// arcJIT — Regulator: Cost Model Engine
//
// Every optimization has costs. Regulator tracks compile time, code size,
// register pressure, guard count, and deopt risk to make informed decisions.
//
// Rule 47: No aggressive pass without cost model.
#pragma once

#include <cstdint>
#include <string>

namespace arcjit {

struct CompileCost {
    // Time costs (microseconds).
    uint64_t compile_time_us = 0;
    uint64_t pass_time_us    = 0;

    // Size costs.
    uint32_t graph_nodes     = 0;
    uint32_t graph_edges     = 0;
    uint32_t code_size_bytes = 0;
    uint32_t deopt_metadata_bytes = 0;

    // Pressure costs.
    uint32_t max_register_pressure = 0;
    uint32_t guard_count           = 0;
    uint32_t deopt_sites           = 0;

    // Risk.
    uint32_t deopt_risk_score = 0;  // 0-100, higher = more likely to deopt

    // Benefits.
    uint32_t expected_speedup_pct = 0;  // expected % speedup

    // Compute whether an optimization is worth it.
    [[nodiscard]] bool is_worth_it() const noexcept {
        // Don't optimize if compile time is too high relative to benefit.
        if (compile_time_us > 10000 && expected_speedup_pct < 10) return false;
        // Don't optimize if code size explosion.
        if (code_size_bytes > 65536 && expected_speedup_pct < 20) return false;
        // Don't optimize if too many guards (high deopt risk).
        if (guard_count > 20 && deopt_risk_score > 50) return false;
        // Otherwise, worth it.
        return expected_speedup_pct > 0;
    }

    // Merge costs from a sub-compilation (e.g., inlined function).
    void merge(const CompileCost& other) {
        compile_time_us       += other.compile_time_us;
        graph_nodes           += other.graph_nodes;
        graph_edges           += other.graph_edges;
        code_size_bytes       += other.code_size_bytes;
        deopt_metadata_bytes  += other.deopt_metadata_bytes;
        max_register_pressure  = std::max(max_register_pressure, other.max_register_pressure);
        guard_count           += other.guard_count;
        deopt_sites           += other.deopt_sites;
        deopt_risk_score       = std::max(deopt_risk_score, other.deopt_risk_score);
        expected_speedup_pct   = std::max(expected_speedup_pct, other.expected_speedup_pct);
    }

    [[nodiscard]] std::string dump() const;
};

// Inline budget for inlining decisions.
struct InlineBudget {
    uint32_t max_nodes       = 32;   // max nodes to inline
    uint32_t max_depth       = 3;    // max inline depth
    uint32_t max_total_nodes = 256;  // total inlined nodes across all sites
    uint32_t used_nodes      = 0;

    [[nodiscard]] bool can_inline(uint32_t callee_nodes, uint32_t depth) const noexcept {
        if (depth >= max_depth) return false;
        if (callee_nodes > max_nodes) return false;
        if (used_nodes + callee_nodes > max_total_nodes) return false;
        return true;
    }

    void consume(uint32_t callee_nodes) { used_nodes += callee_nodes; }
};

class Regulator {
public:
    void record_compile(const CompileCost& cost) {
        total_compile_time_us_ += cost.compile_time_us;
        total_code_bytes_      += cost.code_size_bytes;
        total_guards_          += cost.guard_count;
        total_deopt_sites_     += cost.deopt_sites;
        compile_count_++;
    }

    [[nodiscard]] uint64_t total_compile_time_us() const noexcept { return total_compile_time_us_; }
    [[nodiscard]] uint64_t total_code_bytes() const noexcept { return total_code_bytes_; }
    [[nodiscard]] uint32_t compile_count() const noexcept { return compile_count_; }

    [[nodiscard]] std::string dump() const;

private:
    uint64_t total_compile_time_us_ = 0;
    uint64_t total_code_bytes_      = 0;
    uint32_t total_guards_          = 0;
    uint32_t total_deopt_sites_     = 0;
    uint32_t compile_count_         = 0;
};

Regulator& global_regulator();

}  // namespace arcjit
