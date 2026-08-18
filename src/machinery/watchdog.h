// SPDX-License-Identifier: MIT
// arcJIT — Watchdog: Assumption Registry
//
// Every speculative optimization depends on assumptions (shape stability,
// function not redefined, global is constant, etc.). Watchdog tracks these
// assumptions and provides invalidation when they break.
//
// Rule 44: No assumption without invalidation. Every speculative assumption
// must have a registry entry, an invalidation path, dependent code tracking,
// and a fallback tier.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arcjit {

enum class AssumptionKind : uint8_t {
    ShapeStable       = 0,  // object shape hasn't changed
    FunctionNotRedefined = 1,  // function hasn't been redefined
    GlobalConstant    = 2,  // global variable is constant
    FFIPointerStable  = 3,  // FFI symbol address is stable
    ListNotMutated    = 4,  // list hasn't been mutated during iteration
    MethodTableUnchanged = 5,  // class hierarchy / method table is stable
    FieldOffsetUnchanged = 6,  // field offset hasn't changed
    ModuleFrozen      = 7,  // module is frozen (no more modifications)
    TypeStable        = 8,  // a value's type is stable at a call site
    MonomorphicCall   = 9,  // call site is monomorphic
};

// An invalidation callback — called when the assumption breaks.
// The callback receives the AssumptionId and should:
//   1. Mark dependent compiled code as invalid
//   2. Patch entry points to redirect to fallback
//   3. Schedule recompilation if needed
using InvalidationCallback = std::function<void(uint32_t)>;

struct Assumption {
    AssumptionKind       kind;
    uint64_t             payload;        // kind-specific data (shape ID, function ID, etc.)
    bool                 valid = true;
    uint32_t             dependent_count = 0;
    InvalidationCallback callback;
    std::string          description;    // for debugging / logging
};

class Watchdog {
public:
    // Register a new assumption. Returns the AssumptionId.
    uint32_t register_assumption(AssumptionKind kind, uint64_t payload,
                                  InvalidationCallback cb,
                                  std::string_view desc = {});

    // Invalidate an assumption. Calls the invalidation callback and marks
    // all dependent code for recompilation.
    void invalidate(uint32_t id);

    // Invalidate all assumptions of a given kind with matching payload.
    void invalidate_by_kind(AssumptionKind kind, uint64_t payload);

    // Check if an assumption is still valid.
    [[nodiscard]] bool is_valid(uint32_t id) const;

    // Get the number of active (valid) assumptions.
    [[nodiscard]] size_t active_count() const noexcept;

    // Get the total number of registered assumptions (including invalidated).
    [[nodiscard]] size_t total_count() const noexcept { return assumptions_.size(); }

    // Get the number of invalidation events.
    [[nodiscard]] uint64_t invalidation_count() const noexcept {
        return invalidation_count_.load(std::memory_order_relaxed);
    }

    // Dump all assumptions to a string (for debugging).
    [[nodiscard]] std::string dump() const;

private:
    mutable std::mutex                          mu_;
    std::vector<Assumption>                     assumptions_;
    std::unordered_map<uint64_t, std::unordered_set<uint32_t>> kind_payload_index_;
    std::atomic<uint64_t>                       invalidation_count_{0};

    [[nodiscard]] static std::string_view kind_name(AssumptionKind k) noexcept;
};

// Thread-local global Watchdog instance.
Watchdog& global_watchdog();

}  // namespace arcjit
