// SPDX-License-Identifier: MIT
#include "machinery/governor.h"

#include <format>

namespace arcjit {

SpeculationDecision Governor::decide(const SpeculationContext& ctx) {
    // Rule 1: If we're in a deopt storm, do not speculate.
    if (ctx.is_storm) {
        return SpeculationDecision::Recompile;
    }

    // Rule 2: If deopt count is high (>5) and confidence is low, don't speculate.
    if (ctx.deopt_count > 5 && ctx.confidence <= ConfidenceLevel::Low) {
        return SpeculationDecision::DoNotSpeculate;
    }

    // Rule 3: If no profile data, do not speculate.
    if (ctx.confidence == ConfidenceLevel::None) {
        return SpeculationDecision::DoNotSpeculate;
    }

    // Rule 4: If megamorphic (>4 distinct types), use generic fallback.
    if (ctx.distinct_types > 4) {
        return SpeculationDecision::Megamorphic;
    }

    // Rule 5: If polymorphic (2-4 distinct types), generate polymorphic code.
    if (ctx.distinct_types >= 2 && ctx.distinct_types <= 4) {
        if (ctx.confidence >= ConfidenceLevel::Medium) {
            return SpeculationDecision::Polymorphic;
        }
        return SpeculationDecision::DoNotSpeculate;
    }

    // Rule 6: If monomorphic with high confidence, insert a guard.
    if (ctx.is_monomorphic && ctx.confidence >= ConfidenceLevel::High) {
        // Check if we have enough code size budget for the guard.
        if (ctx.guard_cost > 0 && ctx.guard_cost > ctx.code_size_budget_remaining) {
            return SpeculationDecision::UncommonTrap;
        }

        // Check if we should clone (very high confidence + clone budget available).
        if (ctx.confidence == ConfidenceLevel::VeryHigh &&
            ctx.expected_speedup_pct > 30 &&
            ctx.clone_budget_remaining > 0) {
            return SpeculationDecision::Clone;
        }

        return SpeculationDecision::InsertGuard;
    }

    // Rule 7: If monomorphic with medium confidence, use uncommon trap.
    if (ctx.is_monomorphic && ctx.confidence >= ConfidenceLevel::Medium) {
        return SpeculationDecision::UncommonTrap;
    }

    // Default: don't speculate.
    return SpeculationDecision::DoNotSpeculate;
}

std::string_view Governor::decision_name(SpeculationDecision d) noexcept {
    switch (d) {
        case SpeculationDecision::DoNotSpeculate: return "DoNotSpeculate";
        case SpeculationDecision::InsertGuard:    return "InsertGuard";
        case SpeculationDecision::UncommonTrap:   return "UncommonTrap";
        case SpeculationDecision::Polymorphic:    return "Polymorphic";
        case SpeculationDecision::Megamorphic:    return "Megamorphic";
        case SpeculationDecision::Clone:          return "Clone";
        case SpeculationDecision::Recompile:      return "Recompile";
    }
    return "Unknown";
}

std::string Governor::explain(const SpeculationContext& ctx, SpeculationDecision d) {
    std::string reason;
    switch (d) {
        case SpeculationDecision::Recompile:
            reason = "deopt storm detected — recompile with weaker assumptions";
            break;
        case SpeculationDecision::DoNotSpeculate:
            if (ctx.confidence == ConfidenceLevel::None)
                reason = "no profile data available";
            else if (ctx.deopt_count > 5)
                reason = "high deopt count with low confidence";
            else
                reason = "insufficient confidence for speculation";
            break;
        case SpeculationDecision::Megamorphic:
            reason = std::format("{} distinct types — using generic fallback", ctx.distinct_types);
            break;
        case SpeculationDecision::Polymorphic:
            reason = std::format("{} distinct types with medium+ confidence — polymorphic code", ctx.distinct_types);
            break;
        case SpeculationDecision::InsertGuard:
            reason = std::format("monomorphic with {} confidence — inserting guard",
                                 ctx.confidence == ConfidenceLevel::VeryHigh ? "very high" : "high");
            break;
        case SpeculationDecision::UncommonTrap:
            reason = "monomorphic with medium confidence — uncommon trap for slow path";
            break;
        case SpeculationDecision::Clone:
            reason = "very high confidence + high expected speedup + clone budget available — creating specialized clone";
            break;
    }
    return std::format("Governor: {} — {}", decision_name(d), reason);
}

}  // namespace arcjit
