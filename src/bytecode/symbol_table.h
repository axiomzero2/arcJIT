// SPDX-License-Identifier: MIT
// arcJIT — Real symbol table.
//
// A hash map from interned string name → Value. Used for both global
// variables and Instance fields. Mirrors Arc's `SymbolTable` from
// `include/symbol-table.h`, but implemented in C++23 with linear probing.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "bytecode/value.h"

namespace arcjit {

// FNV-1a 64-bit hash. Fast, good distribution for short identifiers.
[[nodiscard]] inline uint64_t fnv1a64(std::string_view s) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

// A simple open-addressing hash map from string name → Value.
//
// We do NOT store the key string — the caller is expected to keep the key
// alive (typically as an interned String* in the chunk's constant pool).
// This matches Arc's `SymbolTable` semantics.
class SymbolTable {
public:
    SymbolTable() = default;
    explicit SymbolTable(size_t initial_capacity) {
        buckets_.resize(initial_capacity);
    }

    // Look up a key. Returns VAL_UNDEF if not present.
    [[nodiscard]] Value get(std::string_view key) const noexcept {
        if (buckets_.empty()) return Value::undef();
        size_t idx = find_slot(key);
        if (idx == kNotFound) return Value::undef();
        return buckets_[idx].value;
    }

    [[nodiscard]] bool has(std::string_view key) const noexcept {
        if (buckets_.empty()) return false;
        return find_slot(key) != kNotFound;
    }

    // Insert or update a key.
    void set(std::string_view key, Value v) {
        if (buckets_.empty()) grow();
        size_t idx = find_slot(key);
        if (idx == kNotFound) {
            // Table is too full — grow and re-find.
            grow();
            idx = find_slot(key);
        }
        if (buckets_[idx].hash == kEmptyHash) {
            // New entry — copy the key.
            buckets_[idx].hash = fnv1a64(key);
            buckets_[idx].key  = std::string(key);
            count_++;
        }
        buckets_[idx].value = v;
    }

    // Remove a key (tombstone).
    void remove(std::string_view key) noexcept {
        if (buckets_.empty()) return;
        size_t idx = find_slot(key);
        if (idx == kNotFound) return;
        buckets_[idx].hash = kTombstoneHash;
        buckets_[idx].key.clear();
        buckets_[idx].key.shrink_to_fit();
        count_--;
    }

    [[nodiscard]] size_t size() const noexcept { return count_; }
    [[nodiscard]] bool   empty() const noexcept { return count_ == 0; }

    // Iterate over all entries.
    template <typename F>
    void for_each(F&& f) const {
        for (const auto& b : buckets_) {
            if (b.hash != kEmptyHash && b.hash != kTombstoneHash) {
                f(b.key, b.value);
            }
        }
    }

private:
    static constexpr uint64_t kEmptyHash      = 0;
    static constexpr uint64_t kTombstoneHash  = 1;
    static constexpr size_t   kNotFound       = static_cast<size_t>(-1);

    struct Bucket {
        uint64_t    hash  = kEmptyHash;
        std::string key;
        Value       value = Value::undef();
    };
    static_assert(sizeof(Bucket) > 0);

    std::vector<Bucket> buckets_;
    size_t              count_ = 0;

    [[nodiscard]] size_t find_slot(std::string_view key) const noexcept {
        if (buckets_.empty()) return kNotFound;
        const uint64_t h    = fnv1a64(key);
        const size_t   mask = buckets_.size() - 1;
        size_t         idx  = static_cast<size_t>(h) & mask;
        size_t         first_tomb = kNotFound;

        // Linear probe until we find the key, an empty slot, or have gone
        // all the way around (shouldn't happen if load factor < 1).
        for (size_t probes = 0; probes < buckets_.size(); ++probes) {
            const Bucket& b = buckets_[idx];
            if (b.hash == kEmptyHash) {
                // Empty — key not present.
                return first_tomb != kNotFound ? first_tomb : idx;
            }
            if (b.hash == kTombstoneHash) {
                if (first_tomb == kNotFound) first_tomb = idx;
            } else if (b.hash == h && b.key == key) {
                return idx;
            }
            idx = (idx + 1) & mask;
        }
        return first_tomb != kNotFound ? first_tomb : kNotFound;
    }

    void grow() {
        size_t new_capacity = buckets_.empty() ? 16 : buckets_.size() * 2;
        std::vector<Bucket> old = std::move(buckets_);
        buckets_.assign(new_capacity, Bucket{});
        count_ = 0;
        for (auto& b : old) {
            if (b.hash != kEmptyHash && b.hash != kTombstoneHash) {
                set(b.key, b.value);
            }
        }
    }
};

}  // namespace arcjit
