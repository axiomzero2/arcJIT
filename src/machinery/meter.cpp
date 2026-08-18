// SPDX-License-Identifier: MIT
#include "machinery/meter.h"

#include <format>

namespace arcjit {

Meter::Summary Meter::summarize() const {
    Summary s;
    for (const auto& e : entries_) {
        if (e.total_samples == 0) continue;
        s.total_sites++;
        s.total_samples += e.total_samples;
        s.total_deopts += e.deopt_count;

        if (e.is_monomorphic) s.monomorphic++;
        else if (e.distinct_types <= 3) s.polymorphic++;
        else s.megamorphic++;

        if (e.confidence() >= ConfidenceLevel::High) s.high_confidence++;
        else s.low_confidence++;
    }
    return s;
}

std::string Meter::dump() const {
    auto s = summarize();
    std::string out;
    out += std::format("=== Meter ({} sites, {} samples, {} deopts) ===\n",
                       s.total_sites, s.total_samples, s.total_deopts);
    out += std::format("  monomorphic: {}  polymorphic: {}  megamorphic: {}\n",
                       s.monomorphic, s.polymorphic, s.megamorphic);
    out += std::format("  high confidence: {}  low confidence: {}\n",
                       s.high_confidence, s.low_confidence);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        if (e.total_samples == 0) continue;
        out += std::format("  [ip={}] samples={} deopts={} types={} shapes={} mono={} conf={}\n",
                           i, e.total_samples, e.deopt_count,
                           e.distinct_types, e.distinct_shapes,
                           e.is_monomorphic, static_cast<int>(e.confidence()));
    }
    return out;
}

Meter& global_meter() {
    static thread_local Meter m;
    return m;
}

}  // namespace arcjit
