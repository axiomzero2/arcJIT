// SPDX-License-Identifier: MIT
// arcJIT — Core IR identifiers and node shape.
//
// Per docs/ARCHITECTURE.md §3, nodes are compact value objects stored in a
// flat `std::vector<Node>`. Inputs live in a separate edge pool. Stable IDs
// (not raw pointers) are used for long-lived references.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace arcjit {

// --- Stable IDs (wrap a uint32_t index) -------------------------------------
struct NodeId {
    uint32_t value = 0xFFFFFFFF;  // sentinel = invalid

    [[nodiscard]] bool valid() const noexcept { return value != 0xFFFFFFFF; }
    [[nodiscard]] bool operator==(const NodeId& o) const noexcept { return value == o.value; }
    [[nodiscard]] bool operator!=(const NodeId& o) const noexcept { return value != o.value; }
};

struct BlockId {
    uint32_t value = 0xFFFFFFFF;
    [[nodiscard]] bool valid() const noexcept { return value != 0xFFFFFFFF; }
};

// --- Node kinds (Tier-2 Sea of Nodes) ----------------------------------------
enum class NodeKind : uint16_t {
    // Control nodes
    Start = 0,
    Stop,
    Region,
    If,
    IfTrue,
    IfFalse,
    Loop,
    LoopExit,
    Jump,
    Return,
    Branch,
    Unreachable,

    // Constants
    ConstInt,
    ConstFloat,
    ConstNull,
    ConstUndef,
    ConstString,
    ConstFunc,
    ConstClass,

    // Frame state / deopt
    FrameState,
    Deopt,

    // Arithmetic (pure)
    Add, Sub, Mul, Div, Pow, Neg,
    Eq, Ne, Lt, Gt, Lte, Gte,
    And, Or, Not,

    // Bit operations (pure)
    Shl,  // shift left:  x << amount
    Shr,  // shift right (signed): x >> amount

    // Conversions
    ToFloat,
    ToBool,

    // Memory operations
    LoadVar,     // global/symbol-table load
    StoreVar,
    LoadLocal,
    StoreLocal,
    LoadField,   // instance.field
    StoreField,
    LoadIndex,   // list[i] / string[i]
    StoreIndex,

    // Allocation / shape
    Allocate,
    ShapeOf,
    CheckShape,

    // Type guards
    CheckInt,
    CheckFloat,
    CheckNotNull,
    CheckBounds,

    // Calls
    Call,
    CallNative,
    CallKnown,

    // Arc-specific
    ForBegin,
    ForNext,
    TryBegin,
    TryEnd,
    Throw,

    // Phi
    Phi,
    EffectPhi,
    Parameter,

    // Backend (asmjit-emitted)
    MachineOp,

    Count,  // sentinel
};

// --- Node flags --------------------------------------------------------------
enum NodeFlags : uint32_t {
    None          = 0,
    Pure          = 1u << 0,
    CSEable       = 1u << 1,
    GVNable       = 1u << 2,
    Commutative   = 1u << 3,
    NoThrow       = 1u << 4,
    NoDeopt       = 1u << 5,
    HasFrameState = 1u << 6,
    IsAllocated   = 1u << 7,
    IsPinned      = 1u << 8,
    IsGuard       = 1u << 9,
    IsControl     = 1u << 10,
    IsEffect      = 1u << 11,
    IsDead        = 1u << 12,
};

[[nodiscard]] inline NodeFlags operator|(NodeFlags a, NodeFlags b) noexcept {
    return static_cast<NodeFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] inline NodeFlags operator&(NodeFlags a, NodeFlags b) noexcept {
    return static_cast<NodeFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
[[nodiscard]] inline bool has_flag(NodeFlags f, NodeFlags mask) noexcept {
    return (static_cast<uint32_t>(f) & static_cast<uint32_t>(mask)) != 0;
}

// --- Type lattice ------------------------------------------------------------
enum class TypeId : uint16_t {
    Top,            // unknown / any
    Bottom,         // unreachable
    Int,
    Float,
    Bool,
    Null,
    Undef,
    String,
    List,
    Function,
    NativeFunction,
    Class,
    Instance,
    Object,         // any heap object
    IntOrFloat,
    NonNullObj,
};

// --- The Node value object ---------------------------------------------------
//
// 24 bytes. Field order chosen so the struct packs tightly:
//   NodeKind(2) + TypeId(2) + NodeFlags(4) + payload(4)
//   first_input(4) + first_use(4) + input_count(2) + use_count(2)
//
// Inputs come in four flavors (data, control, effect, frame-state). We store
// them as one contiguous slice for cache locality, with per-kind offsets in
// `payload`-side data on the Graph.
//
// Layout (matches docs/ARCHITECTURE.md §3.1):
struct Node {
    NodeKind   kind;            // 2
    TypeId     type;            // 2  (placed adjacent to kind for tight packing)
    NodeFlags  flags;           // 4
    uint32_t   payload;         // 4  — kind-specific small data (const value index, etc.)

    uint32_t   first_input;     // 4
    uint32_t   first_use;       // 4
    uint16_t   input_count;     // 2
    uint16_t   use_count;       // 2
};

static_assert(sizeof(Node) == 24, "Node should be 24 bytes for cache friendliness");

// --- Kind name (for IR dumps) -----------------------------------------------
[[nodiscard]] std::string_view node_kind_name(NodeKind k) noexcept;

}  // namespace arcjit
