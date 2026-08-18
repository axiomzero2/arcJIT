// SPDX-License-Identifier: MIT
// arcJIT — Governor: Speculation Policy Engine
//
// Decides what kind of speculation to use based on:
//   - profile confidence (from Meter)
//   - deopt history (from GroundFault)
//   - guard cost (from Regulator)
//   - code size budget (from Fuse)
//
// Outputs:
//   - InsertGuard: insert a guard + deopt path
//   - UncommonTrap: outline the slow path
//   - Polymorphic: generate polymorphic code (multiple shapes)
//   - Megamorphic: use a generic fallback
//   - DoNotSpeculate: leave it generic
//   - Clone: create a specialized variant
//   - Recompile: recompile with weaker assumptions
#pragma once

#include <cstdint>
#include <string>

#include "machinery/meter.h"
#include "machinery/ground_fault.h"
#include "machinery/regulator.h"
#include "machinery/fuse.h"

namespace arcjit {

enum class SpeculationDecision : uint8_t {
    DoNotSpeculate  = 0,  // leave it generic, no guard
    InsertGuard     = 1,  // insert a guard + deopt path
    UncommonTrap    = 2,  // outline the slow path as an uncommon trap
    Polymorphic     = 3,  // generate polymorphic code (2-4 shapes)
    Megamorphic     = 4,  // use a generic fallback (dict lookup)
    Clone           = 5,  // create a specialized function clone
    Recompile       = 6,  // recompile with weaker assumptions
};

struct SpeculationContext {
    // Profile data.
    ConfidenceLevel confidence = ConfidenceLevel::None;
    bool            is_monomorphic = false;
    uint32_t        distinct_types = 0;

    // Deopt history.
    uint32_t        deopt_count = 0;
    bool            is_storm = false;  // deopt storm detected

    // Cost constraints.
    uint32_t        guard_cost = 0;     // estimated cost of inserting a guard
    uint32_t        code_size_budget_remaining = 0;
    uint32_t        clone_budget_remaining = 0;

    // Benefit estimate.
    uint32_t        expected_speedup_pct = 0;
};

class Governor {
public:
    // Decide what to do for a given speculation context.
    [[nodiscard]] static SpeculationDecision decide(const SpeculationContext& ctx);

    // Get a human-readable name for a decision.
    [[nodiscard]] static std::string_view decision_name(SpeculationDecision d) noexcept;

    // Get a human-readable explanation for a decision.
    [[nodiscard]] static std::string explain(const SpeculationContext& ctx, SpeculationDecision d);
};

}  // namespace arcjit
