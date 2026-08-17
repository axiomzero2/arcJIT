// SPDX-License-Identifier: MIT
#include "node.h"

namespace arcjit {

std::string_view node_kind_name(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Start:           return "Start";
        case NodeKind::Stop:            return "Stop";
        case NodeKind::Region:         return "Region";
        case NodeKind::If:              return "If";
        case NodeKind::IfTrue:          return "IfTrue";
        case NodeKind::IfFalse:         return "IfFalse";
        case NodeKind::Loop:            return "Loop";
        case NodeKind::LoopExit:       return "LoopExit";
        case NodeKind::Jump:            return "Jump";
        case NodeKind::Return:         return "Return";
        case NodeKind::Branch:         return "Branch";
        case NodeKind::Unreachable:    return "Unreachable";
        case NodeKind::ConstInt:        return "ConstInt";
        case NodeKind::ConstFloat:     return "ConstFloat";
        case NodeKind::ConstNull:       return "ConstNull";
        case NodeKind::ConstUndef:     return "ConstUndef";
        case NodeKind::ConstString:    return "ConstString";
        case NodeKind::ConstFunc:      return "ConstFunc";
        case NodeKind::ConstClass:     return "ConstClass";
        case NodeKind::FrameState:     return "FrameState";
        case NodeKind::Deopt:          return "Deopt";
        case NodeKind::Add:             return "Add";
        case NodeKind::Sub:             return "Sub";
        case NodeKind::Mul:             return "Mul";
        case NodeKind::Div:             return "Div";
        case NodeKind::Pow:             return "Pow";
        case NodeKind::Neg:             return "Neg";
        case NodeKind::Eq:              return "Eq";
        case NodeKind::Ne:              return "Ne";
        case NodeKind::Lt:              return "Lt";
        case NodeKind::Gt:              return "Gt";
        case NodeKind::Lte:             return "Lte";
        case NodeKind::Gte:             return "Gte";
        case NodeKind::And:             return "And";
        case NodeKind::Or:              return "Or";
        case NodeKind::Not:             return "Not";
        case NodeKind::ToFloat:         return "ToFloat";
        case NodeKind::ToBool:          return "ToBool";
        case NodeKind::LoadVar:         return "LoadVar";
        case NodeKind::StoreVar:        return "StoreVar";
        case NodeKind::LoadLocal:       return "LoadLocal";
        case NodeKind::StoreLocal:      return "StoreLocal";
        case NodeKind::LoadField:       return "LoadField";
        case NodeKind::StoreField:      return "StoreField";
        case NodeKind::LoadIndex:       return "LoadIndex";
        case NodeKind::StoreIndex:      return "StoreIndex";
        case NodeKind::Allocate:        return "Allocate";
        case NodeKind::ShapeOf:         return "ShapeOf";
        case NodeKind::CheckShape:      return "CheckShape";
        case NodeKind::CheckInt:        return "CheckInt";
        case NodeKind::CheckFloat:      return "CheckFloat";
        case NodeKind::CheckNotNull:    return "CheckNotNull";
        case NodeKind::CheckBounds:     return "CheckBounds";
        case NodeKind::Call:            return "Call";
        case NodeKind::CallNative:      return "CallNative";
        case NodeKind::CallKnown:       return "CallKnown";
        case NodeKind::ForBegin:        return "ForBegin";
        case NodeKind::ForNext:         return "ForNext";
        case NodeKind::TryBegin:        return "TryBegin";
        case NodeKind::TryEnd:          return "TryEnd";
        case NodeKind::Throw:           return "Throw";
        case NodeKind::Phi:             return "Phi";
        case NodeKind::EffectPhi:       return "EffectPhi";
        case NodeKind::Parameter:       return "Parameter";
        case NodeKind::MachineOp:       return "MachineOp";
        case NodeKind::Count:           return "<count>";
    }
    return "<unknown>";
}

}  // namespace arcjit
