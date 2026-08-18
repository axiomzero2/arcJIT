// SPDX-License-Identifier: MIT
// arcJIT — GroundFault: Deopt Analytics
//
// Records and analyzes every deoptimization event. Uses this data to:
//   - disable bad speculation
//   - recompile with polymorphic code
//   - blacklist unstable functions
//   - tune thresholds
//   - detect deopt storms
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace arcjit {

enum class DeoptReason : uint8_t {
    ShapeMismatch    = 0,
    TypeMismatch     = 1,
    NullCheck        = 2,
    BoundsCheck      = 3,
    DivisionByZero   = 4,
    MonomorphicCallMiss = 5,
    InlineCacheMiss  = 6,
    StackOverflow    = 7,
    OSRFailure       = 8,
    AssumptionInvalidated = 9,
};

struct DeoptEvent {
    DeoptReason  reason;
    uint32_t     chunk_offset;       // bytecode offset where deopt occurred
    uint32_t     code_id;            // Trip code ID that deopted
    uint64_t     timestamp_ns;
    uint64_t     expected_value;     // what the JIT assumed
    uint64_t     actual_value;       // what actually happened
    std::string  function_name;
    uint8_t      from_tier;          // which tier deopted (0=Spark, 1=Jolt, 2=Surge)
    bool         recompiled;         // was it recompiled after?
};

class GroundFault {
public:
    void record(DeoptEvent event) {
        std::lock_guard<std::mutex> g(mu_);
        events_.push_back(std::move(event));
        total_count_.fetch_add(1, std::memory_order_relaxed);

        // Track per-chunk deopt count.
        auto& count = per_chunk_count_[events_.back().chunk_offset];
        count++;
    }

    [[nodiscard]] size_t total_deopts() const noexcept {
        return total_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t deopts_for_chunk(uint32_t chunk_offset) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = per_chunk_count_.find(chunk_offset);
        return it != per_chunk_count_.end() ? it->second : 0;
    }

    // Detect deopt storms — same chunk deopting > threshold times.
    [[nodiscard]] bool is_storm(uint32_t chunk_offset, size_t threshold = 10) const {
        return deopts_for_chunk(chunk_offset) >= threshold;
    }

    // Get all deopt events (for analysis).
    [[nodiscard]] const std::vector<DeoptEvent>& events() const noexcept { return events_; }

    // Dump all deopt events (for debugging).
    [[nodiscard]] std::string dump() const;

    // Clear all events (for reset between runs).
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        events_.clear();
        per_chunk_count_.clear();
        total_count_.store(0, std::memory_order_relaxed);
    }

private:
    mutable std::mutex mu_;
    std::vector<DeoptEvent> events_;
    std::unordered_map<uint32_t, size_t> per_chunk_count_;
    std::atomic<size_t> total_count_{0};

    [[nodiscard]] static std::string_view reason_name(DeoptReason r) noexcept;
};

// Thread-local global GroundFault instance.
GroundFault& global_ground_fault();

}  // namespace arcjit
