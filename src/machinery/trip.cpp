// SPDX-License-Identifier: MIT
#include "machinery/trip.h"

#include <format>

namespace arcjit {

uint32_t Trip::register_code(void* entry_point, uint32_t code_size, std::string_view name) {
    std::lock_guard<std::mutex> g(mu_);
    uint32_t id = static_cast<uint32_t>(entries_.size());
    CodeEntry e;
    e.id           = id;
    e.entry_point  = entry_point;
    e.code_size    = code_size;
    e.state        = CodeState::Valid;
    e.function_name = std::string(name);
    entries_.push_back(std::move(e));
    return id;
}

void Trip::add_dependency(uint32_t code_id, uint32_t assumption_id) {
    std::lock_guard<std::mutex> g(mu_);
    if (code_id >= entries_.size()) return;
    entries_[code_id].dependent_assumptions.push_back(assumption_id);
    assumption_to_code_[assumption_id].push_back(code_id);
}

void Trip::invalidate(uint32_t code_id) {
    std::lock_guard<std::mutex> g(mu_);
    if (code_id >= entries_.size()) return;
    CodeEntry& e = entries_[code_id];
    if (e.state != CodeState::Valid) return;

    e.state = CodeState::Invalidated;
    invalidation_count_.fetch_add(1, std::memory_order_relaxed);

    // In a full implementation, we would:
    //   1. Patch the entry point to jump to a deopt/interpreter stub.
    //   2. Wait for all threads to safepoint.
    //   3. Mark the code as Uninstalled.
    //   4. Tell Capacitor to free the code memory.
    // For now, we just mark it invalid.
}

void Trip::invalidate_by_assumption(uint32_t assumption_id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = assumption_to_code_.find(assumption_id);
    if (it == assumption_to_code_.end()) return;
    for (uint32_t code_id : it->second) {
        if (code_id < entries_.size() && entries_[code_id].state == CodeState::Valid) {
            entries_[code_id].state = CodeState::Invalidated;
            invalidation_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool Trip::is_valid(uint32_t code_id) const {
    std::lock_guard<std::mutex> g(mu_);
    if (code_id >= entries_.size()) return false;
    return entries_[code_id].state == CodeState::Valid;
}

size_t Trip::active_count() const noexcept {
    return entries_.size() - invalidation_count_.load(std::memory_order_relaxed);
}

std::string Trip::dump() const {
    std::lock_guard<std::mutex> g(mu_);
    std::string out;
    out += std::format("=== Trip ({} entries, {} invalidated) ===\n",
                       entries_.size(),
                       invalidation_count_.load(std::memory_order_relaxed));
    for (const auto& e : entries_) {
        const char* state_str = e.state == CodeState::Valid       ? "Valid"
                              : e.state == CodeState::Invalidated ? "Invalidated"
                              :                                      "Uninstalled";
        out += std::format("  [{}] {} {}B state={} deps={}\n",
                           e.id, e.function_name, e.code_size,
                           state_str, e.dependent_assumptions.size());
    }
    return out;
}

Trip& global_trip() {
    static thread_local Trip t;
    return t;
}

}  // namespace arcjit
