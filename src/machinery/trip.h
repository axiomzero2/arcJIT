// SPDX-License-Identifier: MIT
// arcJIT — Trip: Code Invalidation Engine
//
// When assumptions break (via Watchdog), compiled code must be invalidated
// safely. Trip handles:
//   - marking code invalid
//   - patching entry points to redirect to fallback (interpreter/baseline)
//   - waiting for safepoints (all mutator threads must be stopped)
//   - uninstalling code (freeing code memory via Capacitor)
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace arcjit {

enum class CodeState : uint8_t {
    Valid       = 0,  // code is active and can be called
    Invalidated = 1,  // code is marked invalid but may still be executing
    Uninstalled = 2,  // code memory has been freed
};

struct CodeEntry {
    uint32_t                id;
    void*                   entry_point = nullptr;
    uint32_t                code_size   = 0;
    CodeState               state       = CodeState::Valid;
    std::vector<uint32_t>   dependent_assumptions;  // Watchdog assumption IDs
    std::string             function_name;
};

class Trip {
public:
    // Register a new compiled code entry. Returns the code ID.
    uint32_t register_code(void* entry_point, uint32_t code_size,
                            std::string_view name);

    // Add a dependency on a Watchdog assumption. When the assumption is
    // invalidated, this code will be invalidated too.
    void add_dependency(uint32_t code_id, uint32_t assumption_id);

    // Invalidate a code entry. Marks it as invalid and patches the entry
    // point to redirect to the fallback (interpreter). The actual code
    // memory is freed later by Capacitor after all threads have safepointed.
    void invalidate(uint32_t code_id);

    // Invalidate all code that depends on a given assumption.
    void invalidate_by_assumption(uint32_t assumption_id);

    // Check if a code entry is still valid.
    [[nodiscard]] bool is_valid(uint32_t code_id) const;

    // Get the number of active (valid) code entries.
    [[nodiscard]] size_t active_count() const noexcept;

    // Get the total invalidation count.
    [[nodiscard]] uint64_t invalidation_count() const noexcept {
        return invalidation_count_.load(std::memory_order_relaxed);
    }

    // Dump all code entries (for debugging).
    [[nodiscard]] std::string dump() const;

private:
    mutable std::mutex                    mu_;
    std::vector<CodeEntry>                entries_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> assumption_to_code_;
    std::atomic<uint64_t>                 invalidation_count_{0};
};

Trip& global_trip();

}  // namespace arcjit
