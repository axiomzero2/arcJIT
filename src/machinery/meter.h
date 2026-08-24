// SPDX-License-Identifier: MIT
// arcJIT — Meter: Profile Confidence Engine
//
// Not all profile data is equally trustworthy. Meter tracks sample count,
// stability, deopt history, and variance to compute a confidence score.
//
// Rule 46: No profile data without confidence. Low-confidence profile data
// must not trigger aggressive speculation.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace arcjit {

enum class ConfidenceLevel : uint8_t {
    None      = 0,  // no profile data
    Low       = 1,  // < 100 samples or unstable
    Medium    = 2,  // 100-1000 samples, mostly stable
    High      = 3,  // > 1000 samples, very stable, no recent deopts
    VeryHigh  = 4,  // > 10000 samples, rock-solid, never deopted
};

// Per-call-site profile data with confidence tracking.
struct ProfileEntry {
    uint64_t total_samples = 0;
    uint64_t deopt_count   = 0;      // how many times this site deopted
    uint64_t last_deopt_age = 0;     // invocations since last deopt
    uint32_t distinct_types = 0;     // how many distinct types seen
    uint32_t distinct_shapes = 0;    // how many distinct shapes seen
    bool     is_monomorphic = false;

    // The most-observed type/shape (if monomorphic).
    uint64_t dominant_type  = 0;
    uint64_t dominant_shape = 0;
    double   dominant_ratio = 0.0;  // dominant_count / total_samples

    // Compute the confidence level.
    [[nodiscard]] ConfidenceLevel confidence() const noexcept {
        if (total_samples < 100) return ConfidenceLevel::None;
        if (deopt_count > 0 && last_deopt_age < 1000) return ConfidenceLevel::Low;
        if (distinct_types > 3 || distinct_shapes > 3) return ConfidenceLevel::Low;
        if (total_samples < 1000) return ConfidenceLevel::Medium;
        if (dominant_ratio < 0.9) return ConfidenceLevel::Medium;
        if (deopt_count > 0) return ConfidenceLevel::High;
        if (total_samples < 10000) return ConfidenceLevel::High;
        return ConfidenceLevel::VeryHigh;
    }

    // Is this profile entry confident enough for monomorphic speculation?
    [[nodiscard]] bool can_speculate_monomorphic() const noexcept {
        return confidence() >= ConfidenceLevel::High && is_monomorphic;
    }

    // Is this profile entry confident enough for inlining?
    [[nodiscard]] bool can_inline() const noexcept {
        return confidence() >= ConfidenceLevel::Medium;
    }

    // Is this profile entry confident enough for function cloning?
    [[nodiscard]] bool can_clone() const noexcept {
        return confidence() >= ConfidenceLevel::High;
    }

    // Record a sample.
    void record_sample(uint64_t type, uint64_t shape) {
        total_samples++;
        last_deopt_age++;

        // First sample ever — initialize.
        if (total_samples == 1) {
            dominant_type = type;
            dominant_shape = shape;
            distinct_types = 1;
            distinct_shapes = 1;
            dominant_ratio = 1.0;
            is_monomorphic = true;
            return;
        }

        // Check if this is a new type.
        if (type != dominant_type) {
            distinct_types++;
            is_monomorphic = false;
            dominant_ratio = static_cast<double>(total_samples - distinct_types + 1) /
                             static_cast<double>(total_samples);
        }
        if (shape != dominant_shape) {
            distinct_shapes++;
        }
    }

    // Record a deopt at this site.
    void record_deopt() {
        deopt_count++;
        last_deopt_age = 0;
    }
};

// The Meter holds profile entries for all instrumented call sites,
// property accesses, and type checks.
class Meter {
public:
    // Get or create a profile entry for a bytecode offset.
    ProfileEntry& entry_for(uint32_t bytecode_offset) {
        if (bytecode_offset >= entries_.size()) {
            entries_.resize(bytecode_offset + 1);
        }
        return entries_[bytecode_offset];
    }

    [[nodiscard]] const ProfileEntry& entry_for(uint32_t bytecode_offset) const {
        static ProfileEntry empty;
        if (bytecode_offset >= entries_.size()) return empty;
        return entries_[bytecode_offset];
    }

    // Number of profile entries (highest offset + 1). Useful for iterating
    // all entries when querying the Governor for speculation decisions.
    [[nodiscard]] size_t entry_count() const noexcept { return entries_.size(); }

    // Get the overall confidence summary.
    struct Summary {
        size_t   total_sites    = 0;
        size_t   monomorphic    = 0;
        size_t   polymorphic    = 0;
        size_t   megamorphic    = 0;
        size_t   high_confidence = 0;
        size_t   low_confidence  = 0;
        uint64_t total_samples   = 0;
        uint64_t total_deopts    = 0;
    };

    [[nodiscard]] Summary summarize() const;

    // Dump all profile entries (for debugging).
    [[nodiscard]] std::string dump() const;

private:
    std::vector<ProfileEntry> entries_;
};

// Thread-local global Meter instance.
Meter& global_meter();

}  // namespace arcjit
