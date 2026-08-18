// SPDX-License-Identifier: MIT
#include "machinery/watchdog.h"

#include <format>
#include <print>

namespace arcjit {

std::string_view Watchdog::kind_name(AssumptionKind k) noexcept {
    switch (k) {
        case AssumptionKind::ShapeStable:          return "ShapeStable";
        case AssumptionKind::FunctionNotRedefined: return "FunctionNotRedefined";
        case AssumptionKind::GlobalConstant:       return "GlobalConstant";
        case AssumptionKind::FFIPointerStable:     return "FFIPointerStable";
        case AssumptionKind::ListNotMutated:       return "ListNotMutated";
        case AssumptionKind::MethodTableUnchanged: return "MethodTableUnchanged";
        case AssumptionKind::FieldOffsetUnchanged: return "FieldOffsetUnchanged";
        case AssumptionKind::ModuleFrozen:         return "ModuleFrozen";
        case AssumptionKind::TypeStable:           return "TypeStable";
        case AssumptionKind::MonomorphicCall:      return "MonomorphicCall";
    }
    return "Unknown";
}

uint32_t Watchdog::register_assumption(AssumptionKind kind, uint64_t payload,
                                        InvalidationCallback cb,
                                        std::string_view desc) {
    std::lock_guard<std::mutex> g(mu_);
    uint32_t id = static_cast<uint32_t>(assumptions_.size());
    Assumption a;
    a.kind          = kind;
    a.payload       = payload;
    a.valid         = true;
    a.callback      = std::move(cb);
    a.description   = std::string(desc);
    assumptions_.push_back(std::move(a));

    // Index by (kind, payload) for fast lookup.
    uint64_t key = (static_cast<uint64_t>(kind) << 56) | (payload & 0x00FFFFFFFFFFFFFF);
    kind_payload_index_[key].insert(id);

    return id;
}

void Watchdog::invalidate(uint32_t id) {
    std::lock_guard<std::mutex> g(mu_);
    if (id >= assumptions_.size()) return;
    Assumption& a = assumptions_[id];
    if (!a.valid) return;  // already invalidated

    a.valid = false;
    invalidation_count_.fetch_add(1, std::memory_order_relaxed);

    // Call the invalidation callback (if any).
    if (a.callback) {
        a.callback(id);
    }

    // Remove from the index.
    uint64_t key = (static_cast<uint64_t>(a.kind) << 56) | (a.payload & 0x00FFFFFFFFFFFFFF);
    auto it = kind_payload_index_.find(key);
    if (it != kind_payload_index_.end()) {
        it->second.erase(id);
    }
}

void Watchdog::invalidate_by_kind(AssumptionKind kind, uint64_t payload) {
    std::lock_guard<std::mutex> g(mu_);
    uint64_t key = (static_cast<uint64_t>(kind) << 56) | (payload & 0x00FFFFFFFFFFFFFF);
    auto it = kind_payload_index_.find(key);
    if (it == kind_payload_index_.end()) return;

    // Copy the IDs because invalidate() modifies the set.
    auto ids = std::vector<uint32_t>(it->second.begin(), it->second.end());
    // Unlock temporarily — invalidate() takes the lock.
    // Actually we can't unlock/relock safely here. Let's inline the logic.
    for (uint32_t id : ids) {
        if (id >= assumptions_.size()) continue;
        Assumption& a = assumptions_[id];
        if (!a.valid) continue;
        a.valid = false;
        invalidation_count_.fetch_add(1, std::memory_order_relaxed);
        if (a.callback) a.callback(id);
    }
    it->second.clear();
}

bool Watchdog::is_valid(uint32_t id) const {
    std::lock_guard<std::mutex> g(mu_);
    if (id >= assumptions_.size()) return false;
    return assumptions_[id].valid;
}

size_t Watchdog::active_count() const noexcept {
    // Approximate — doesn't take the lock for speed.
    return assumptions_.size() - invalidation_count_.load(std::memory_order_relaxed);
}

std::string Watchdog::dump() const {
    std::lock_guard<std::mutex> g(mu_);
    std::string out;
    out += std::format("=== Watchdog ({} assumptions, {} invalidated) ===\n",
                       assumptions_.size(),
                       invalidation_count_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < assumptions_.size(); ++i) {
        const Assumption& a = assumptions_[i];
        out += std::format("  [{}] {} payload={:#x} valid={} desc='{}'\n",
                           i, kind_name(a.kind), a.payload, a.valid, a.description);
    }
    return out;
}

Watchdog& global_watchdog() {
    static thread_local Watchdog w;
    return w;
}

}  // namespace arcjit
