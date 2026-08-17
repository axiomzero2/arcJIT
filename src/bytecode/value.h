// SPDX-License-Identifier: MIT
// arcJIT — Value type matching Arc's runtime exactly.
//
// Mirrors `include/value.h` from the upstream Arc repo. The interpreter and
// all JIT tiers MUST produce values of this exact shape so that compiled code
// and interpreted code can exchange state without translation.
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace arcjit {

// --- Value type tag (matches Arc's ValueType) -------------------------------
enum class ValueType : uint8_t {
    Undef = 0,  // raises NameError on read
    Null  = 1,
    Int   = 2,  // unboxed int64_t
    Float = 3,  // unboxed double
    Obj   = 4,  // heap Object*
};

// Forward declaration of the Arc heap-object header.
struct Object;

// --- Value (matches Arc's Value) --------------------------------------------
//
// Layout-identical to Arc's `Value` so the JIT can pass values back and forth
// to the existing Arc runtime without boxing. The tag sits in the low byte;
// the union is either an int64_t, a double, or an Object*.
//
struct Value {
    ValueType type;
    union {
        int64_t  i;
        double   f;
        Object*  obj;
    } as;

    // --- Constructors -------------------------------------------------------
    static constexpr Value undef() noexcept { return {ValueType::Undef, {.i = 0}}; }
    static constexpr Value null()  noexcept { return {ValueType::Null,  {.i = 0}}; }
    static constexpr Value Int(int64_t v) noexcept { return {ValueType::Int,   {.i = v}}; }
    static constexpr Value Float(double v)  noexcept { return {ValueType::Float, {.f = v}}; }
    static constexpr Value Obj(Object* o)  noexcept { return {ValueType::Obj,   {.obj = o}}; }

    // --- Type queries -------------------------------------------------------
    [[nodiscard]] constexpr bool is_undef() const noexcept { return type == ValueType::Undef; }
    [[nodiscard]] constexpr bool is_null()  const noexcept { return type == ValueType::Null; }
    [[nodiscard]] constexpr bool is_int()   const noexcept { return type == ValueType::Int; }
    [[nodiscard]] constexpr bool is_float() const noexcept { return type == ValueType::Float; }
    [[nodiscard]] constexpr bool is_obj()   const noexcept { return type == ValueType::Obj; }
    [[nodiscard]] constexpr bool is_number() const noexcept { return is_int() || is_float(); }

    // --- Accessors (unchecked) ----------------------------------------------
    [[nodiscard]] constexpr int64_t as_int()   const noexcept { return as.i; }
    [[nodiscard]] constexpr double  as_float() const noexcept { return as.f; }
    [[nodiscard]] constexpr Object* as_obj()  const noexcept { return as.obj; }

    // --- Truthiness (matches `isTruthy` in Arc's vm.c) ----------------------
    [[nodiscard]] bool is_truthy() const noexcept;

    // --- Equality (for tests / deopt validation) ---------------------------
    [[nodiscard]] bool operator==(const Value& o) const noexcept {
        if (type != o.type) return false;
        switch (type) {
            case ValueType::Undef:
            case ValueType::Null:  return true;
            case ValueType::Int:   return as.i == o.as.i;
            case ValueType::Float: return as.f == o.as.f;
            case ValueType::Obj:   return as.obj == o.as.obj;
        }
        return false;
    }
};

static_assert(std::is_trivially_copyable_v<Value>);
static_assert(sizeof(Value) == 16);

// --- Convenience constructors (mirroring VAL_INT / VAL_FLOAT / VAL_OBJ macros)
inline constexpr Value VAL_UNDEF = Value::undef();
inline constexpr Value VAL_NULL  = Value::null();

inline constexpr Value VAL_INT(int64_t v)   noexcept { return Value::Int(v); }
inline constexpr Value VAL_FLOAT(double v)  noexcept { return Value::Float(v); }
inline constexpr Value VAL_OBJ(Object* o)   noexcept { return Value::Obj(o); }

}  // namespace arcjit
