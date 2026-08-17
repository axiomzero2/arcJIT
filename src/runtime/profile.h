// SPDX-License-Identifier: MIT
// arcJIT — Type feedback vectors and inline caches.
//
// The Tier-0 interpreter collects runtime profiles that drive Tier-1 and
// Tier-2 speculation. Each profile slot is small (typically 8 bytes) and
// lives in a side table keyed by bytecode offset.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "bytecode/chunk.h"
#include "bytecode/object.h"
#include "bytecode/value.h"

namespace arcjit {

// --- Type Feedback Vector (TFV) --------------------------------------------
//
// For each bytecode offset that produces a value, the TFV records the
// observed type tag(s). We use a 4-entry bitset because most sites see at
// most 4 distinct types before becoming "megamorphic".
//
enum class FeedbackKind : uint8_t {
    None        = 0,
    Int         = 1u << 0,
    Float       = 1u << 1,
    String      = 1u << 2,
    List        = 1u << 3,
    Instance    = 1u << 4,
    Function    = 1u << 5,
    Null        = 1u << 6,
    OtherObj    = 1u << 7,
};

[[nodiscard]] inline FeedbackKind operator|(FeedbackKind a, FeedbackKind b) noexcept {
    return static_cast<FeedbackKind>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
[[nodiscard]] inline bool operator&(FeedbackKind a, FeedbackKind b) noexcept {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

inline FeedbackKind kind_of(const Value& v) noexcept {
    switch (v.type) {
        case ValueType::Int:   return FeedbackKind::Int;
        case ValueType::Float: return FeedbackKind::Float;
        case ValueType::Null:  return FeedbackKind::Null;
        case ValueType::Undef: return FeedbackKind::None;
        case ValueType::Obj:
            if (!v.as.obj) return FeedbackKind::Null;
            switch (v.as.obj->type) {
                case ObjType::String:           return FeedbackKind::String;
                case ObjType::List:              return FeedbackKind::List;
                case ObjType::Instance:          return FeedbackKind::Instance;
                case ObjType::Function:          return FeedbackKind::Function;
                case ObjType::NativeFunction:    return FeedbackKind::Function;
                default:                         return FeedbackKind::OtherObj;
            }
    }
    return FeedbackKind::None;
}

// Number of distinct shapes recorded before we declare a site megamorphic.
inline constexpr uint8_t kMegamorphicThreshold = 4;

// One per bytecode offset that produces a value (arithmetic, loads, calls).
struct TypeFeedback {
    uint8_t      seen = 0;            // bitset of FeedbackKind
    uint8_t      count = 0;           // number of distinct types seen
    uint32_t     invocations = 0;     // how many times this site executed

    void observe(const Value& v) {
        invocations++;
        FeedbackKind k = kind_of(v);
        uint8_t bit = static_cast<uint8_t>(k);
        if ((seen & bit) == 0) {
            seen |= bit;
            count++;
        }
    }
};

// --- Inline Cache (IC) -------------------------------------------------------
//
// Each property access / call site / index op has an IC slot. The slot records
// the observed "shape" — for an Instance this is the Class* pointer, for a
// String/List it's just the ObjType. If the next execution matches the cached
// shape, the JIT-compiled stub takes the fast path; otherwise it falls back
// to the runtime.
//
struct InlineCache {
    uintptr_t shape = 0;     // Class* or ObjType tag
    uint32_t  hits   = 0;
    uint32_t  misses = 0;

    bool matches(uintptr_t s) const noexcept { return shape == s && shape != 0; }

    void record(uintptr_t s) {
        if (shape == 0) {
            shape = s;
            hits  = 1;
        } else if (shape == s) {
            hits++;
        } else {
            misses++;
            // Megamorphic — invalidate.
            shape = 0;
            hits = 0;
        }
    }
};

// --- Per-function profile data ---------------------------------------------
class FunctionProfile {
public:
    explicit FunctionProfile(size_t bytecode_size)
        : tfv_(bytecode_size) {}

    TypeFeedback& at(uint32_t offset) { return tfv_[offset]; }
    [[nodiscard]] const TypeFeedback& at(uint32_t offset) const { return tfv_[offset]; }

    // IC slot per bytecode offset. We use a hash map indexed by offset to
    // avoid allocating 8 bytes per opcode.
    InlineCache& ic_for(uint32_t offset) {
        for (auto& [k, v] : ics_) {
            if (k == offset) return v;
        }
        ics_.emplace_back(offset, InlineCache{});
        return ics_.back().second;
    }

    [[nodiscard]] uint64_t invocations() const noexcept { return invocations_; }
    void bump_invocation() noexcept { invocations_++; }

private:
    std::vector<TypeFeedback>               tfv_;
    std::vector<std::pair<uint32_t, InlineCache>> ics_;
    uint64_t                                 invocations_ = 0;
};

}  // namespace arcjit
