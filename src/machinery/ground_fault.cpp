// SPDX-License-Identifier: MIT
#include "machinery/ground_fault.h"

#include <format>

namespace arcjit {

std::string_view GroundFault::reason_name(DeoptReason r) noexcept {
    switch (r) {
        case DeoptReason::ShapeMismatch:        return "ShapeMismatch";
        case DeoptReason::TypeMismatch:         return "TypeMismatch";
        case DeoptReason::NullCheck:            return "NullCheck";
        case DeoptReason::BoundsCheck:          return "BoundsCheck";
        case DeoptReason::DivisionByZero:       return "DivisionByZero";
        case DeoptReason::MonomorphicCallMiss:  return "MonomorphicCallMiss";
        case DeoptReason::InlineCacheMiss:      return "InlineCacheMiss";
        case DeoptReason::StackOverflow:        return "StackOverflow";
        case DeoptReason::OSRFailure:           return "OSRFailure";
        case DeoptReason::AssumptionInvalidated: return "AssumptionInvalidated";
    }
    return "Unknown";
}

std::string GroundFault::dump() const {
    std::lock_guard<std::mutex> g(mu_);
    std::string out;
    out += std::format("=== GroundFault ({} deopt events) ===\n",
                       total_count_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < events_.size(); ++i) {
        const DeoptEvent& e = events_[i];
        out += std::format("  [{}] {} chunk_ip={} code_id={} fn='{}' expected={:#x} actual={:#x} recompiled={}\n",
                           i, reason_name(e.reason), e.chunk_offset, e.code_id,
                           e.function_name, e.expected_value, e.actual_value, e.recompiled);
    }

    // Per-chunk summary.
    if (!per_chunk_count_.empty()) {
        out += "\n  Per-chunk deopt counts:\n";
        for (const auto& [offset, count] : per_chunk_count_) {
            const char* storm = count >= 10 ? " *** STORM ***" : "";
            out += std::format("    ip={}: {} deopts{}\n", offset, count, storm);
        }
    }

    return out;
}

GroundFault& global_ground_fault() {
    static thread_local GroundFault gf;
    return gf;
}

}  // namespace arcjit
