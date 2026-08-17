// SPDX-License-Identifier: MIT
// arcJIT — Tier-1 baseline SSA JIT IR.
//
// A flat list of instructions with explicit basic-block boundaries via Labels.
// Each instruction has up to 3 virtual register operands (dst, src1, src2)
// plus a payload (for constants, local slots, jump targets, etc.).
//
// The IR is intentionally low-level — close enough to x86-64 that asmjit
// emission is mechanical, but high enough that the linear-scan allocator
// has good live-range granularity.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "bytecode/chunk.h"
#include "bytecode/value.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <asmjit/core.h>
#include <asmjit/x86.h>
#pragma GCC diagnostic pop

namespace arcjit {

// --- IR opcodes -------------------------------------------------------------
//
// Every Arc opcode maps to one or more Tier1Ops. Some (Call, Branch) require
// multiple machine instructions to lower.
enum class Tier1Op : uint8_t {
    // Constants / moves
    LoadConst,        // dst = constants[payload]
    LoadConstImm,     // dst = payload (sign-extended to 64-bit)
    LoadLocal,        // dst = locals[payload]
    StoreLocal,       // locals[payload] = src1
    LoadVar,          // dst = globals[payload]  (string name index)
    StoreVar,         // globals[payload] = src1
    Mov,              // dst = src1

    // Arithmetic (int fast path)
    Add,              // dst = src1 + src2
    Sub, Mul,
    Div,              // dst = src1 / src2 (float result)
    Pow,              // dst = pow(src1, src2)
    Neg,              // dst = -src1

    // Comparisons (produce 0 or 1)
    Eq, Ne, Lt, Gt, Lte, Gte,

    // Logical
    And, Or, Not,     // Not: dst = !src1

    // Type checks / conversions
    IsTruthy,         // dst = is_truthy(src1)
    ToFloat,          // dst = (double)src1

    // Memory (heap objects)
    AllocList,        // dst = new List, capacity = src1
    ListAppend,       // list_obj = src1, elem = src2
    ListGet,          // dst = src1[src2]
    ListSet,          // src1[src2] = src3 (no dst)
    AllocInstance,    // dst = new Instance(klass = payload)
    GetField,         // dst = src1.fields[payload]
    SetField,         // src1.fields[payload] = src2

    // Calls
    Call,             // dst = call(callee=src1, args=locals[payload..payload+n])
    CallNative,       // dst = call native fn at payload
    Return,           // return src1

    // Control flow
    Label,            // marker — payload is label ID
    Jump,             // jump to label payload
    BranchIfFalse,    // if (!src1) jump to label payload
    BranchIfTrue,     // if (src1) jump to label payload
    Halt,             // stop execution
};

// A single IR instruction. 20 bytes (enum + 3-byte pad + 4×uint32).
struct Tier1Inst {
    Tier1Op   op;
    uint8_t   _pad[3];
    uint32_t  dst;        // destination vreg (0 = no destination)
    uint32_t  src1;       // first source vreg
    uint32_t  src2;       // second source vreg
    uint32_t  payload;    // kind-specific data (const idx, slot, label, imm)
};
static_assert(sizeof(Tier1Inst) == 20);

// A function in Tier-1 IR form.
struct Tier1Function {
    std::vector<Tier1Inst>  insts;
    std::vector<std::string> label_names;  // for debugging
    uint32_t                vreg_count   = 0;
    uint32_t                label_count  = 0;
    int                     max_locals   = 0;
    int                     num_params   = 0;
    std::string             name;
    const Chunk*            source_chunk = nullptr;

    // Allocate a new virtual register.
    uint32_t alloc_vreg() { return ++vreg_count; }

    // Allocate a new label ID.
    uint32_t alloc_label() { return label_count++; }

    // Emit an instruction.
    void emit(Tier1Op op, uint32_t dst = 0, uint32_t src1 = 0, uint32_t src2 = 0,
              uint32_t payload = 0) {
        insts.push_back({op, {0, 0, 0}, dst, src1, src2, payload});
    }
};

// --- Linear Scan register allocator ----------------------------------------
struct LiveInterval {
    uint32_t vreg;
    uint32_t start;  // first instruction index where vreg is live
    uint32_t end;    // last instruction index (inclusive)
    int      assigned_reg = -1;  // physical reg (-1 = stack slot)
};

struct RegAllocResult {
    std::vector<LiveInterval> intervals;
    std::vector<int>          vreg_to_phys;     // size = vreg_count + 1
    std::vector<int>          vreg_to_stack;    // size = vreg_count + 1, -1 if in reg
    int                       max_stack_slots = 0;
};

// Run linear-scan allocation on a Tier1Function. Uses x86-64 GP regs.
RegAllocResult linear_scan(const Tier1Function& fn);

// --- asmjit emission --------------------------------------------------------
class Tier1Compiler {
public:
    Tier1Compiler();

    // Compile a Tier1Function to executable machine code.
    // The entry point takes a pointer to a "Tier1Context" struct (passed in
    // rdi) holding locals array, globals table, and constants pool.
    [[nodiscard]] std::expected<int64_t (*)(void*), std::string>
    compile(const Tier1Function& fn);

    void release() { runtime_.reset(); }

private:
    std::unique_ptr<asmjit::JitRuntime> runtime_;
    asmjit::Environment                  env_;
};

// --- Convenience: synthesize a Tier1Function that computes `a + b + c` ------
Tier1Function make_demo_add3();

// --- Lowering: Arc Chunk → Tier1Function -----------------------------------
//
// Walks the bytecode, maintains a virtual stack of vreg IDs, and emits one
// or more Tier1Inst per Arc opcode. Resolves forward jump targets via a
// fixup pass at the end.
[[nodiscard]] std::expected<Tier1Function, std::string>
lower_chunk_to_tier1(const Chunk& chunk, std::string_view name);

}  // namespace arcjit
