// SPDX-License-Identifier: MIT
#include "machinery/regulator.h"

#include <format>

namespace arcjit {

std::string CompileCost::dump() const {
    return std::format(
        "CompileCost(time={}us nodes={} edges={} code={}B guards={} deopts={} risk={} speedup={}%)",
        compile_time_us, graph_nodes, graph_edges, code_size_bytes,
        guard_count, deopt_sites, deopt_risk_score, expected_speedup_pct);
}

std::string Regulator::dump() const {
    return std::format(
        "Regulator(compiles={} time={}us code={}B guards={} deopt_sites={})",
        compile_count_, total_compile_time_us_, total_code_bytes_,
        total_guards_, total_deopt_sites_);
}

Regulator& global_regulator() {
    static thread_local Regulator r;
    return r;
}

}  // namespace arcjit
