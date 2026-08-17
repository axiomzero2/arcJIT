// SPDX-License-Identifier: MIT
// arcJIT — Real heap object allocation.
//
// We provide constructors, destructors, and refcount helpers for every Arc
// object kind. Objects are heap-allocated via `new`/`delete` (with our own
// `Object* alloc_*()` helpers so we can switch to a slab allocator later).
//
// Refcounting matches Arc's `Object::refCount` semantics:
//   - new objects start with refcount = 1
//   - copyValue increments if !is_static
//   - freeValue decrements and frees if it hits 0
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bytecode/object.h"
#include "bytecode/symbol_table.h"
#include "bytecode/value.h"

namespace arcjit {

// --- Allocation helpers ----------------------------------------------------
//
// We deliberately use `new`/`delete` (rather than arena allocation) for heap
// objects because their lifetimes are decoupled from compilation. The Arena
// is for IR nodes; the heap is for Arc values.

template <typename T, typename... Args>
T* alloc_obj(Args&&... args) {
    T* obj = new T(std::forward<Args>(args)...);
    obj->base.ref_count = 1;
    obj->base.is_static = false;
    return obj;
}

// --- Number -----------------------------------------------------------------
inline Object* alloc_int(int64_t v) {
    Number* n = alloc_obj<Number>();
    n->base.type = ObjType::NumberInt;
    n->as.i = v;
    return reinterpret_cast<Object*>(n);
}

inline Object* alloc_float(double v) {
    Number* n = alloc_obj<Number>();
    n->base.type = ObjType::NumberFloat;
    n->as.f = v;
    return reinterpret_cast<Object*>(n);
}

// --- String -----------------------------------------------------------------
inline Object* alloc_string(std::string_view s) {
    String* str = alloc_obj<String>();
    str->base.type = ObjType::String;
    str->len       = s.size();
    str->capacity  = s.size();
    str->value     = static_cast<char*>(std::malloc(s.size() + 1));
    str->hash      = 0;
    str->is_buffer = false;
    str->owns_value = true;
    if (!s.empty()) std::memcpy(str->value, s.data(), s.size());
    str->value[s.size()] = '\0';
    return reinterpret_cast<Object*>(str);
}

// String hash (djb2-style).
[[nodiscard]] inline long string_hash(const char* s, size_t len) noexcept {
    long h = 5381;
    for (size_t i = 0; i < len; ++i) h = ((h << 5) + h) + static_cast<long>(s[i]);
    return h;
}

// --- List -------------------------------------------------------------------
class ListObj {
public:
    static Object* alloc(size_t initial_capacity = 8) {
        List* l = alloc_obj<List>();
        l->base.type = ObjType::List;
        l->size      = 0;
        l->capacity  = initial_capacity;
        l->objects   = initial_capacity > 0
                         ? static_cast<Object**>(std::calloc(initial_capacity, sizeof(Object*)))
                         : nullptr;
        return reinterpret_cast<Object*>(l);
    }

    static void append(Object* list_obj, Object* o) {
        List* l = cast_to<List>(list_obj);
        if (l->size >= l->capacity) {
            size_t new_cap = l->capacity == 0 ? 8 : l->capacity * 2;
            l->objects = static_cast<Object**>(
                std::realloc(l->objects, new_cap * sizeof(Object*)));
            std::memset(l->objects + l->capacity, 0,
                        (new_cap - l->capacity) * sizeof(Object*));
            l->capacity = new_cap;
        }
        l->objects[l->size++] = o;
        if (o) o->ref_count++;
    }

    [[nodiscard]] static Object* at(const Object* list_obj, size_t i) noexcept {
        const List* l = cast_to<List>(list_obj);
        return i < l->size ? l->objects[i] : nullptr;
    }
};

// --- Function ---------------------------------------------------------------
struct ArcFunction {
    Object       base;
    std::string  name;
    std::vector<std::string> params;
    const Chunk* chunk = nullptr;       // borrowed — owned by the runtime
    int          max_locals = 0;

    static Object* alloc(std::string_view name_, const Chunk* chunk_, int max_locals_) {
        ArcFunction* f = alloc_obj<ArcFunction>();
        f->base.type   = ObjType::Function;
        f->name        = name_;
        f->chunk       = chunk_;
        f->max_locals  = max_locals_;
        return reinterpret_cast<Object*>(f);
    }
};

// --- Class ------------------------------------------------------------------
struct ArcClass {
    Object       base;
    std::string  name;
    const Chunk* init_chunk = nullptr;
    int          max_locals = 0;

    static Object* alloc(std::string_view name_, const Chunk* init_chunk_, int max_locals_) {
        ArcClass* c = alloc_obj<ArcClass>();
        c->base.type = ObjType::Class;
        c->name      = name_;
        c->init_chunk = init_chunk_;
        c->max_locals = max_locals_;
        return reinterpret_cast<Object*>(c);
    }
};

// --- Instance ---------------------------------------------------------------
struct ArcInstance {
    Object         base;
    ArcClass*      klass = nullptr;
    SymbolTable    fields;

    static Object* alloc(ArcClass* k) {
        ArcInstance* i = alloc_obj<ArcInstance>();
        i->base.type = ObjType::Instance;
        i->klass     = k;
        if (k) k->base.ref_count++;
        return reinterpret_cast<Object*>(i);
    }
};

// --- NativeFunction ---------------------------------------------------------
struct ArcNative {
    Object       base;
    std::string  name;
    Value (*fn)(std::span<Value> args);
    size_t      required_args = 0;
    bool        variadic       = false;

    static Object* alloc(std::string_view name_,
                         Value (*fn_)(std::span<Value> args),
                         size_t required_args_, bool variadic_) {
        ArcNative* n = alloc_obj<ArcNative>();
        n->base.type      = ObjType::NativeFunction;
        n->name           = name_;
        n->fn             = fn_;
        n->required_args  = required_args_;
        n->variadic       = variadic_;
        return reinterpret_cast<Object*>(n);
    }
};

// --- Refcount helpers -------------------------------------------------------
inline void retain(Object* o) {
    if (o && !o->is_static) o->ref_count++;
}

inline void release(Object* o);
inline void release_value(Value v);

// Free an object's owned resources.
inline void free_object(Object* o) {
    if (!o) return;
    switch (o->type) {
        case ObjType::String: {
            String* s = reinterpret_cast<String*>(o);
            if (s->owns_value && s->value) std::free(s->value);
            delete s;
            break;
        }
        case ObjType::List: {
            List* l = reinterpret_cast<List*>(o);
            for (size_t i = 0; i < l->size; ++i) {
                if (l->objects[i]) release(l->objects[i]);
            }
            if (l->objects) std::free(l->objects);
            delete l;
            break;
        }
        case ObjType::Function:       delete reinterpret_cast<ArcFunction*>(o); break;
        case ObjType::NativeFunction: delete reinterpret_cast<ArcNative*>(o);   break;
        case ObjType::Class:          delete reinterpret_cast<ArcClass*>(o);    break;
        case ObjType::Instance: {
            ArcInstance* inst = reinterpret_cast<ArcInstance*>(o);
            inst->fields.for_each([](std::string_view, Value v) { release_value(v); });
            if (inst->klass) release(reinterpret_cast<Object*>(inst->klass));
            delete inst;
            break;
        }
        case ObjType::NumberInt:
        case ObjType::NumberFloat:    delete reinterpret_cast<Number*>(o);     break;
        case ObjType::Null:           delete o;                                 break;
        default:                      delete o;                                 break;
    }
}

inline void release(Object* o) {
    if (!o || o->is_static) return;
    if (--o->ref_count <= 0) {
        free_object(o);
    }
}

// Box a scalar Value into a heap Object (for storing inside List / Instance).
[[nodiscard]] inline Object* value_to_object(Value v) {
    if (v.is_int())       return alloc_int(v.as_int());
    if (v.is_float())     return alloc_float(v.as_float());
    if (v.is_null())      return nullptr;
    if (v.is_obj())       { retain(v.as_obj()); return v.as_obj(); }
    return nullptr;
}

// Unbox a heap Object into a scalar Value (used by LOAD_CONST etc.).
[[nodiscard]] inline Value object_to_value(Object* o) {
    if (!o) return Value::null();
    if (o->type == ObjType::NumberInt)   return Value::Int(reinterpret_cast<Number*>(o)->as.i);
    if (o->type == ObjType::NumberFloat) return Value::Float(reinterpret_cast<Number*>(o)->as.f);
    if (o->type == ObjType::Null)        return Value::null();
    return Value::Obj(o);
}

// Copy a Value — bump refcount if it owns a heap object.
[[nodiscard]] inline Value copy_value(Value v) {
    if (v.is_obj() && v.as_obj() && !v.as_obj()->is_static) {
        retain(v.as_obj());
    }
    return v;
}

// Release a Value — decrement refcount if owned.
inline void release_value(Value v) {
    if (v.is_obj() && v.as_obj()) release(v.as_obj());
}

}  // namespace arcjit
