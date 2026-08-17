// SPDX-License-Identifier: MIT
// arcJIT — Value::is_truthy out-of-line definition.
//
// `Value::is_truthy()` is declared in value.h but depends on the Object/String/
// List/Number layouts defined in object.h. To avoid a circular include
// (value.h ↔ object.h), we keep the definition here in this single TU.
#include "bytecode/object.h"

namespace arcjit {

bool Value::is_truthy() const noexcept {
    switch (type) {
        case ValueType::Int:   return as.i != 0;
        case ValueType::Float: return as.f != 0.0;
        case ValueType::Undef:
        case ValueType::Null:  return false;
        case ValueType::Obj: {
            if (!as.obj) return false;
            switch (as.obj->type) {
                case ObjType::String:      return cast_to<String>(as.obj)->len > 0;
                case ObjType::List:        return cast_to<List>(as.obj)->size > 0;
                case ObjType::NumberInt:   return cast_to<Number>(as.obj)->as.i != 0;
                case ObjType::NumberFloat: return cast_to<Number>(as.obj)->as.f != 0.0;
                default:                   return true;
            }
        }
    }
    return true;
}

}  // namespace arcjit
