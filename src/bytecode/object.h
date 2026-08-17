// SPDX-License-Identifier: MIT
// arcJIT — Arc heap object header.
//
// Mirrors `include/object.h` from the upstream Arc repo. We declare only the
// C-layout-compatible headers (Object, Number, String, List) that the JIT
// needs to inspect from C-layout memory. Higher-level Arc types (Function,
// Class, Instance, NativeFunction) are defined in `heap.h` because they own
// C++ resources (std::string, std::vector, SymbolTable).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "value.h"

namespace arcjit {

// Heap object kind — matches Arc's `ObjType`.
enum class ObjType : uint8_t {
    NumberInt        = 0,
    NumberFloat      = 1,
    String           = 2,
    List             = 3,
    Function         = 4,
    NativeFunction   = 5,
    FunctionCall     = 6,
    Module           = 7,
    Error            = 8,
    Return           = 9,
    File             = 10,
    Break            = 11,
    Continue         = 12,
    Class            = 13,
    Instance         = 14,
    Null             = 15,
};

// Common header. This is the layout of Arc's `Object` struct, so the JIT
// can read it from Arc-allocated memory without translation.
struct Object {
    ObjType type;
    int     ref_count;
    bool    is_static;
};

static_assert(std::is_standard_layout_v<Object>);

// --- C-layout-compatible structs (used when we need stable offsetof) --------
//
// These match Arc's `include/object.h` layouts exactly. The higher-level
// C++ wrappers (ArcFunction, ArcClass, ArcInstance, ArcNative) live in
// `heap.h` because they own C++ resources.

struct Number {
    Object base;
    union {
        int64_t i;
        double  f;
    } as;
};

struct String {
    Object   base;
    char*    value;
    uint64_t len;
    uint64_t capacity;
    long     hash;
    bool     is_buffer;
    bool     owns_value;
};

struct List {
    Object     base;
    Object**   objects;
    uint64_t   size;
    uint64_t   capacity;
};

// --- Helpers for downcasting from Object* to a specific kind ---------------
// These reinterpret_casts are safe because the C-layout structs above have
// `Object base;` as their first member (standard-layout guarantee).
//
// We use the name `cast_to<T>` rather than `as<T>` to avoid clashing with
// `Value::as` (the union member accessor).

template <typename T>
[[nodiscard]] inline T* cast_to(Object* o) noexcept {
    return reinterpret_cast<T*>(o);
}

template <typename T>
[[nodiscard]] inline const T* cast_to(const Object* o) noexcept {
    return reinterpret_cast<const T*>(o);
}

// Note: `Value::is_truthy()` is declared in value.h but defined in value.cpp
// (because it depends on the Object/String/List/Number layouts defined here).

}  // namespace arcjit
