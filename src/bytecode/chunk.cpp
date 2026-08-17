// SPDX-License-Identifier: MIT
// arcJIT — Bytecode helpers implementation.
#include "chunk.h"

namespace arcjit {

std::string_view opcode_name(OpCode op) noexcept {
    switch (op) {
        case OpCode::LoadConst:      return "LoadConst";
        case OpCode::LoadVar:        return "LoadVar";
        case OpCode::LoadLocal:      return "LoadLocal";
        case OpCode::StoreVar:       return "StoreVar";
        case OpCode::StoreLocal:     return "StoreLocal";
        case OpCode::Add:            return "Add";
        case OpCode::Sub:            return "Sub";
        case OpCode::Mul:            return "Mul";
        case OpCode::Div:            return "Div";
        case OpCode::Pow:            return "Pow";
        case OpCode::Neg:            return "Neg";
        case OpCode::Not:            return "Not";
        case OpCode::Eq:             return "Eq";
        case OpCode::Ne:             return "Ne";
        case OpCode::Lt:             return "Lt";
        case OpCode::Gt:             return "Gt";
        case OpCode::Lte:            return "Lte";
        case OpCode::Gte:            return "Gte";
        case OpCode::And:            return "And";
        case OpCode::Or:             return "Or";
        case OpCode::Jump:           return "Jump";
        case OpCode::JumpIfFalse:    return "JumpIfFalse";
        case OpCode::Call:           return "Call";
        case OpCode::Return:         return "Return";
        case OpCode::Break:          return "Break";
        case OpCode::Continue:       return "Continue";
        case OpCode::IndexGet:       return "IndexGet";
        case OpCode::IndexSet:       return "IndexSet";
        case OpCode::BuildList:      return "BuildList";
        case OpCode::Pop:            return "Pop";
        case OpCode::ForPrep:        return "ForPrep";
        case OpCode::ForIter:        return "ForIter";
        case OpCode::TryPush:        return "TryPush";
        case OpCode::TryPop:         return "TryPop";
        case OpCode::Import:         return "Import";
        case OpCode::PropertyAccess: return "PropertyAccess";
        case OpCode::PropertySet:    return "PropertySet";
        case OpCode::Halt:           return "Halt";
        case OpCode::Count:          return "<count>";
    }
    return "<unknown>";
}

}  // namespace arcjit
