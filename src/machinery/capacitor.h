// SPDX-License-Identifier: MIT
// arcJIT — Capacitor: Code Cache Manager
//
// Compiled code is a resource. Capacitor manages code memory, tracks
// allocations, handles eviction, and monitors fragmentation.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace arcjit {

struct CodeAllocation {
    void*    base       = nullptr;
    uint32_t size       = 0;
    uint32_t age        = 0;      // invocations since allocation
    uint32_t hits       = 0;      // how many times this code was called
    bool     is_hot     = false;
    bool     is_evictable = true;  // can be freed if under memory pressure
};

class Capacitor {
public:
    // Register a code allocation.
    void register_allocation(void* base, uint32_t size, bool evictable = true);

    // Record a hit (code was called).
    void record_hit(void* base);

    // Age all allocations by one tick.
    void age_all();

    // Evict cold allocations to free up memory.
    // Returns the number of bytes freed.
    uint64_t evict_cold(uint64_t target_bytes_to_free);

    // Get total allocated code memory.
    [[nodiscard]] uint64_t total_allocated() const noexcept {
        return total_allocated_.load(std::memory_order_relaxed);
    }

    // Get number of allocations.
    [[nodiscard]] size_t allocation_count() const noexcept {
        return allocations_.size();
    }

    // Get the eviction count.
    [[nodiscard]] uint64_t eviction_count() const noexcept {
        return eviction_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string dump() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<void*, CodeAllocation> allocations_;
    std::atomic<uint64_t> total_allocated_{0};
    std::atomic<uint64_t> eviction_count_{0};
};

Capacitor& global_capacitor();

}  // namespace arcjit
