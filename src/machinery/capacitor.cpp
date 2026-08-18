// SPDX-License-Identifier: MIT
#include "machinery/capacitor.h"

#include <algorithm>
#include <format>
#include <vector>

namespace arcjit {

void Capacitor::register_allocation(void* base, uint32_t size, bool evictable) {
    std::lock_guard<std::mutex> g(mu_);
    CodeAllocation a;
    a.base         = base;
    a.size         = size;
    a.age          = 0;
    a.hits         = 0;
    a.is_hot       = false;
    a.is_evictable = evictable;
    allocations_[base] = a;
    total_allocated_.fetch_add(size, std::memory_order_relaxed);
}

void Capacitor::record_hit(void* base) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = allocations_.find(base);
    if (it == allocations_.end()) return;
    it->second.hits++;
    it->second.age = 0;  // reset age on hit
    if (it->second.hits > 100) it->second.is_hot = true;
}

void Capacitor::age_all() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& [_, a] : allocations_) {
        a.age++;
        if (a.age > 10000 && a.hits < 10) {
            a.is_hot = false;
        }
    }
}

uint64_t Capacitor::evict_cold(uint64_t target_bytes) {
    std::lock_guard<std::mutex> g(mu_);
    uint64_t freed = 0;

    // Sort by (hot, hits, age) — coldest first.
    std::vector<CodeAllocation*> candidates;
    for (auto& [_, a] : allocations_) {
        if (a.is_evictable && !a.is_hot) {
            candidates.push_back(&a);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CodeAllocation* a, const CodeAllocation* b) {
                  if (a->hits != b->hits) return a->hits < b->hits;
                  return a->age > b->age;
              });

    for (CodeAllocation* a : candidates) {
        if (freed >= target_bytes) break;
        freed += a->size;
        total_allocated_.fetch_sub(a->size, std::memory_order_relaxed);
        // In a real implementation, we would free the code memory here.
        // asmjit's JitRuntime manages its own memory, so we just track it.
        allocations_.erase(a->base);
        eviction_count_.fetch_add(1, std::memory_order_relaxed);
    }

    return freed;
}

std::string Capacitor::dump() const {
    std::lock_guard<std::mutex> g(mu_);
    return std::format(
        "Capacitor(allocs={} total={}KB evictions={})",
        allocations_.size(),
        total_allocated_.load(std::memory_order_relaxed) / 1024,
        eviction_count_.load(std::memory_order_relaxed));
}

Capacitor& global_capacitor() {
    static thread_local Capacitor c;
    return c;
}

}  // namespace arcjit
