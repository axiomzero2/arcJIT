// SPDX-License-Identifier: MIT
// arcJIT — Type-safe bitmask utilities.
//
// Rule 51: All orthogonal boolean state must be bitmasked with type-safe
// wrappers. Raw integers are forbidden for flag-like state.
//
// This file provides:
//   - Flags<E>: a type-safe wrapper around bitflag enums
//   - NodeBitSet: a bitvector for worklist/visited sets
//   - AnalysisInvalidSet: bitmask for pass invalidation
//   - CompileOptions: bitmask for pass gating
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace arcjit {

// ============================================================================
// Flags<E> — Type-safe bitmask wrapper
// ============================================================================
//
// Usage:
//   enum class MyFlags : uint32_t { A = 1<<0, B = 1<<1, C = 1<<2 };
//   using MyFlagSet = Flags<MyFlags>;
//
//   MyFlagSet flags = MyFlags::A | MyFlags::B;
//   if (flags.has(MyFlags::A)) { ... }
//   if (flags.any(MyFlags::A | MyFlags::C)) { ... }
//   flags |= MyFlags::C;
//
// Type safety:
//   flags + 1;       // COMPILE ERROR
//   flags & 0x42;    // COMPILE ERROR
//   int x = flags;   // COMPILE ERROR (use .raw())
//
template <typename E>
class Flags {
public:
    using Underlying = std::underlying_type_t<E>;

    constexpr Flags() : value_(0) {}
    constexpr Flags(E e) : value_(static_cast<Underlying>(e)) {}
    constexpr explicit Flags(Underlying v) : value_(v) {}

    // --- Combinators (accept raw enum or Flags) ---
    constexpr Flags operator|(Flags o) const { return Flags{static_cast<E>(value_ | o.value_)}; }
    constexpr Flags operator|(E e) const { return Flags{static_cast<E>(value_ | static_cast<Underlying>(e))}; }
    constexpr Flags operator&(Flags o) const { return Flags{static_cast<E>(value_ & o.value_)}; }
    constexpr Flags operator&(E e) const { return Flags{static_cast<E>(value_ & static_cast<Underlying>(e))}; }
    constexpr Flags operator~() const { return Flags{static_cast<E>(~value_)}; }
    constexpr Flags operator^(Flags o) const { return Flags{static_cast<E>(value_ ^ o.value_)}; }

    constexpr Flags& operator|=(Flags o) { value_ |= o.value_; return *this; }
    constexpr Flags& operator|=(E e) { value_ |= static_cast<Underlying>(e); return *this; }
    constexpr Flags& operator&=(Flags o) { value_ &= o.value_; return *this; }
    constexpr Flags& operator&=(E e) { value_ &= static_cast<Underlying>(e); return *this; }
    constexpr Flags& operator^=(Flags o) { value_ ^= o.value_; return *this; }

    // --- Queries ---
    constexpr bool has(E e) const { return (value_ & static_cast<Underlying>(e)) != 0; }
    constexpr bool any(Flags mask) const { return (value_ & mask.value_) != 0; }
    constexpr bool all(Flags mask) const { return (value_ & mask.value_) == mask.value_; }
    constexpr bool none() const { return value_ == 0; }
    constexpr explicit operator bool() const { return value_ != 0; }

    // --- Raw access (use sparingly) ---
    constexpr Underlying raw() const { return value_; }

    // --- Equality ---
    constexpr bool operator==(Flags o) const { return value_ == o.value_; }
    constexpr bool operator!=(Flags o) const { return value_ != o.value_; }

private:
    Underlying value_;
};

// Free-function operators so `EnumA | EnumB` works directly.
template <typename E>
constexpr Flags<E> operator|(E a, E b) {
    return Flags<E>(a) | b;
}

// ============================================================================
// NodeBitSet — Bitvector for worklist/visited sets
// ============================================================================
//
// For graphs under ~4096 nodes, this fits in 64 bytes (one cache line).
// For larger graphs, it's still dramatically faster than std::unordered_set
// due to cache locality and no hashing.
//
class NodeBitSet {
public:
    NodeBitSet() = default;
    explicit NodeBitSet(size_t max_nodes) { resize(max_nodes); }

    void resize(size_t max_nodes) {
        bits_.assign((max_nodes + 63) / 64, 0);
    }

    void set(uint32_t id) {
        size_t idx = id >> 6;
        if (idx >= bits_.size()) bits_.resize(idx + 1, 0);
        bits_[idx] |= (1ULL << (id & 63));
    }

    void clear(uint32_t id) {
        size_t idx = id >> 6;
        if (idx < bits_.size()) bits_[idx] &= ~(1ULL << (id & 63));
    }

    [[nodiscard]] bool test(uint32_t id) const {
        size_t idx = id >> 6;
        return idx < bits_.size() && (bits_[idx] & (1ULL << (id & 63)));
    }

    void clear_all() {
        std::memset(bits_.data(), 0, bits_.size() * sizeof(uint64_t));
    }

    // Set union: this |= other
    void operator|=(const NodeBitSet& other) {
        if (other.bits_.size() > bits_.size()) bits_.resize(other.bits_.size(), 0);
        for (size_t i = 0; i < other.bits_.size(); ++i) bits_[i] |= other.bits_[i];
    }

    // Set intersection: this &= other
    void operator&=(const NodeBitSet& other) {
        size_t n = std::min(bits_.size(), other.bits_.size());
        for (size_t i = 0; i < n; ++i) bits_[i] &= other.bits_[i];
        for (size_t i = n; i < bits_.size(); ++i) bits_[i] = 0;
    }

    [[nodiscard]] size_t popcount() const {
        size_t count = 0;
        for (uint64_t w : bits_) {
            count += __builtin_popcountll(w);
        }
        return count;
    }

    [[nodiscard]] bool empty() const {
        for (uint64_t w : bits_) if (w) return false;
        return true;
    }

private:
    std::vector<uint64_t> bits_;
};

// ============================================================================
// AnalysisInvalidSet — Bitmask for pass invalidation
// ============================================================================
//
// A pass declares what analyses it invalidates in one word.
// The analysis manager checks with a single bitwise AND.
//
enum class AnalysisKind : uint32_t {
    DominatorTree      = 1u << 0,
    LoopTree           = 1u << 1,
    TypeInference      = 1u << 2,
    RangeAnalysis      = 1u << 3,
    AliasAnalysis      = 1u << 4,
    MemoryDependence   = 1u << 5,
    BranchProbability  = 1u << 6,
    Liveness           = 1u << 7,
    FrameStateLiveness = 1u << 8,
    ScalarEvolution    = 1u << 9,
    EscapeState        = 1u << 10,
    ValueNumbering     = 1u << 11,
};

using AnalysisInvalidSet = Flags<AnalysisKind>;

// ============================================================================
// CompileOptions — Bitmask for pass gating
// ============================================================================
//
enum class CompileOption : uint64_t {
    EnableInlining          = 1ULL << 0,
    EnablePEA               = 1ULL << 1,
    EnableLoadElimination   = 1ULL << 2,
    EnableLoopUnrolling     = 1ULL << 3,
    EnableVectorization     = 1ULL << 4,
    EnableFastMath          = 1ULL << 5,
    EnableGuardSinking      = 1ULL << 6,
    EnableFunctionCloning   = 1ULL << 7,
    EnableFFISpecialization = 1ULL << 8,
    StressDeopt             = 1ULL << 9,
    Deterministic           = 1ULL << 10,
    VerifyGraph             = 1ULL << 11,
    TracePasses             = 1ULL << 12,
};

using CompileOptionSet = Flags<CompileOption>;

// Default options for the Gigavolt pipeline.
inline constexpr CompileOptionSet kDefaultGigavoltOptions =
    CompileOption::EnableInlining |
    CompileOption::EnablePEA |
    CompileOption::EnableLoadElimination |
    CompileOption::EnableLoopUnrolling |
    CompileOption::VerifyGraph;

// ============================================================================
// Symbolic flag printing
// ============================================================================
//
// Converts a Flags<E> value to a human-readable string like "Pure | Foldable".
// Each flag type must provide its own printer.
//
[[nodiscard]] std::string format_node_flags(uint32_t raw_flags);
[[nodiscard]] std::string format_analysis_invalid(uint32_t raw);
[[nodiscard]] std::string format_compile_options(uint64_t raw);

}  // namespace arcjit
