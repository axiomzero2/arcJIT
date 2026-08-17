// SPDX-License-Identifier: MIT
// arcJIT — Arc heap object header.
//
// Mirrors `include/object.h` from the upstream Arc repo. We only declare the
// pieces the JIT actually needs to inspect or mutate (shape, type tag,
// refcount). Full object kinds (String, List, Function, ...) are accessible
// via downcast through the `ObjType` enum.
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

// --- Concrete object shapes --------------------------------------------------
//
// We mirror Arc's concrete types so the JIT can inline field loads/stores.
// All layouts must match the upstream `object.h` exactly.

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

struct Function {
    Object   base;
    char*    name;
    char**   params;
    size_t   param_count;
    void*    body;        // ASTNode* (opaque to the JIT)
    void*    chunk;       // Chunk* (opaque to the JIT)
    int      max_locals;
};

struct NativeFunction {
    Object   base;
    char*    name;
    Object* (*function)(Object** args, size_t arg_count);
    bool     is_variadic;
    size_t   required_arg_count;
};

struct Class {
    Object   base;
    char*    name;
    void*    chunk;       // Chunk* (opaque)
    int      max_locals;
};

struct SymbolTable;  // opaque forward decl from Arc; we only use it as void*

struct Instance {
    Object         base;
    Class*         klass;
    SymbolTable*   fields;
};

struct ProgramError {
    Object   base;
    char*    details;
};

// Note: `Value::is_truthy()` is declared in value.h but defined in value.cpp
// (because it depends on the Object/String/List/Number layouts defined here).

}  // namespace arcjit
