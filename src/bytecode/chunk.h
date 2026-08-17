// SPDX-License-Identifier: MIT
// arcJIT — Arc opcode set, chunk, and call frame.
//
// This is a 1:1 C++23 port of `include/compiler.h` and `include/vm.h` from
// the upstream Arc repo. The enum order must match Arc's `OpCode` exactly
// so we can read Arc-produced chunks without translation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "object.h"
#include "value.h"

namespace arcjit {

// --- Opcode set (matches Arc's OpCode enum) --------------------------------
enum class OpCode : uint8_t {
    LoadConst = 0,
    LoadVar,
    LoadLocal,
    StoreVar,
    StoreLocal,

    Add,
    Sub,
    Mul,
    Div,
    Pow,
    Neg,
    Not,

    Eq,
    Ne,
    Lt,
    Gt,
    Lte,
    Gte,
    And,
    Or,

    Jump,
    JumpIfFalse,

    Call,
    Return,

    Break,
    Continue,

    IndexGet,
    IndexSet,
    BuildList,
    Pop,

    ForPrep,
    ForIter,

    TryPush,
    TryPop,

    Import,

    PropertyAccess,
    PropertySet,

    Halt,

    Count,  // sentinel — number of opcodes
};

// Source-location entry (matches Arc's PosEntry). Used for error reporting.
struct Position {
    uint32_t line;
    uint32_t col;
    uint32_t offset;
};

struct PosEntry {
    uint32_t offset;  // first bytecode offset where this span applies
    Position start;
    Position end;
};

// --- Chunk (matches Arc's Chunk) --------------------------------------------
//
// A chunk is a single function's worth of bytecode plus its constant pool
// and source map. The bytecode is a raw byte stream; operands are encoded
// per the rules in docs/BYTECODE.md.
//
class Chunk {
public:
    Chunk() = default;

    // --- Construction (used by the test harness when synthesizing chunks) -
    void emit_byte(uint8_t b) { code_.push_back(b); }
    void emit_op(OpCode op) { emit_byte(static_cast<uint8_t>(op)); }

    // 3-byte big-endian constant index
    void emit_const_idx(uint32_t idx) {
        code_.push_back(static_cast<uint8_t>((idx >> 16) & 0xFF));
        code_.push_back(static_cast<uint8_t>((idx >> 8) & 0xFF));
        code_.push_back(static_cast<uint8_t>(idx & 0xFF));
    }

    // 2-byte big-endian signed jump offset
    void emit_short(int16_t off) {
        uint16_t u = static_cast<uint16_t>(off);
        code_.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
        code_.push_back(static_cast<uint8_t>(u & 0xFF));
    }

    uint32_t add_const(Object* obj) {
        constants_.push_back(obj);
        return static_cast<uint32_t>(constants_.size() - 1);
    }

    // --- Read-only view -----------------------------------------------------
    [[nodiscard]] std::span<const uint8_t> code() const noexcept { return code_; }
    [[nodiscard]] std::span<Object* const>  constants() const noexcept { return constants_; }
    [[nodiscard]] std::span<const PosEntry> positions() const noexcept { return positions_; }

    // --- Mutation (used by patch-up passes) --------------------------------
    // Patch a single byte at the given offset. Used by the lowering passes
    // to fix forward jumps after their target offset becomes known.
    void patch_byte(size_t offset, uint8_t b) {
        if (offset < code_.size()) code_[offset] = b;
    }
    void patch_short(size_t offset, int16_t v) {
        if (offset + 1 < code_.size()) {
            uint16_t u = static_cast<uint16_t>(v);
            code_[offset]     = static_cast<uint8_t>((u >> 8) & 0xFF);
            code_[offset + 1] = static_cast<uint8_t>(u & 0xFF);
        }
    }

    [[nodiscard]] size_t code_size() const noexcept { return code_.size(); }
    [[nodiscard]] int    max_locals() const noexcept { return max_locals_; }

    void set_max_locals(int n) noexcept { max_locals_ = n; }
    void set_filename(std::string_view f) { filename_ = f; }
    [[nodiscard]] std::string_view filename() const noexcept { return filename_; }

private:
    std::vector<uint8_t>  code_;
    std::vector<Object*>  constants_;
    std::vector<PosEntry> positions_;
    std::string           filename_;
    int                   max_locals_ = 0;
};

// --- Operand decoders --------------------------------------------------------
// These match the macros in Arc's vm.c:
//   READ_BYTE      — single byte
//   READ_CONST_IDX — 3 bytes big-endian (24-bit)
//   READ_SHORT     — 2 bytes big-endian signed
//
struct BytecodeReader {
    std::span<const uint8_t> bytes;
    size_t                   ip = 0;

    [[nodiscard]] bool at_end() const noexcept { return ip >= bytes.size(); }

    uint8_t read_byte() { return bytes[ip++]; }

    OpCode read_op() { return static_cast<OpCode>(read_byte()); }

    uint32_t read_const_idx() {
        uint32_t v = (static_cast<uint32_t>(bytes[ip]) << 16)
                    | (static_cast<uint32_t>(bytes[ip + 1]) << 8)
                    |  static_cast<uint32_t>(bytes[ip + 2]);
        ip += 3;
        return v;
    }

    int16_t read_short() {
        uint16_t u = (static_cast<uint16_t>(bytes[ip]) << 8)
                   |  static_cast<uint16_t>(bytes[ip + 1]);
        ip += 2;
        return static_cast<int16_t>(u);
    }
};

// --- Helper: opcode name (for IR dumps) -------------------------------------
[[nodiscard]] std::string_view opcode_name(OpCode op) noexcept;

}  // namespace arcjit
