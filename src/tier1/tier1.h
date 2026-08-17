// SPDX-License-Identifier: MIT
// arcJIT — Tier-1 baseline SSA JIT.
//
// Per docs/ARCHITECTURE.md §1.2, Tier 1 is the "fast path" generator. It
// takes a hot Arc chunk + Tier-0 profiles, lowers the stack bytecode to a
// small SSA-style IR, runs Linear Scan register allocation, and emits x86-64
// machine code via asmjit. No heavy optimizations.
//
// For the scaffold, we implement a single example pattern: an "add two locals"
// function that returns `a + b`. This demonstrates the full pipeline:
//   bytecode → SSA IR → linear scan → asmjit → execute.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bytecode/chunk.h"
#include "bytecode/value.h"

// We include asmjit here directly. The build system links asmjit::asmjit.
//
// We use the modern per-target headers (`asmjit/x86.h` + `asmjit/core.h`) and
// wrap the include with pragma push/pop because asmjit uses anonymous structs
// that trip -Wpedantic. The rest of our code keeps -Wpedantic enabled.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <asmjit/core.h>
#include <asmjit/x86.h>
#pragma GCC diagnostic pop

namespace arcjit {

// --- SSA-style IR for Tier 1 ------------------------------------------------
//
// A simple linear SSA IR: one instruction per line, virtual registers as IDs.
// This is *not* the Sea of Nodes IR — Tier 1 uses a flat list with explicit
// basic-block boundaries. SoN is reserved for Tier 2.
//
enum class Tier1Op {
    LoadConst,    // dst = constants[payload]
    LoadLocal,    // dst = locals[payload]
    StoreLocal,   // locals[payload] = src1
    Add,          // dst = src1 + src2  (int fast path)
    Sub,
    Mul,
    Cmp,          // dst = compare(src1, src2, mode)
    Return,       // return src1
    Jump,
    BranchIfFalse,
};

struct Tier1Inst {
    Tier1Op op;
    uint32_t dst      = 0;       // virtual register ID for the destination
    uint32_t src1     = 0;
    uint32_t src2     = 0;
    uint32_t payload   = 0;       // const/local index
};

struct Tier1Function {
    std::vector<Tier1Inst> insts;
    uint32_t                vreg_count = 0;
    int                     max_locals = 0;
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
    std::vector<int>          vreg_to_phys;     // size = vreg_count
    std::vector<int>          vreg_to_stack;    // size = vreg_count, -1 if in reg
};

// Run linear-scan allocation on a Tier1Function. Uses x86-64 GP regs:
//   rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11 (caller-saved, scratch)
//   rbx, r12, r13, r14, r15 (callee-saved, for spilled)
RegAllocResult linear_scan(const Tier1Function& fn);

// --- asmjit emission --------------------------------------------------------
class Tier1Compiler {
public:
    Tier1Compiler();

    // Compile a Tier1Function to executable machine code.
    // Returns a pointer to the entry point, or an error string.
    [[nodiscard]] std::expected<void(*)(), std::string> compile(const Tier1Function& fn);

    // Release the code memory (called automatically by destructor).
    void release() { runtime_.reset(); }

private:
    std::unique_ptr<asmjit::JitRuntime> runtime_;
    asmjit::Environment                  env_;
};

// --- Convenience: synthesize a Tier1Function that computes `a + b + c` ------
//
// This is the demo case for end-to-end Tier-1 execution. Real lowering from
// Arc bytecode would walk the Chunk and emit Tier1Inst per opcode; that
// machinery is sketched but not wired in this initial milestone.
//
Tier1Function make_demo_add3();

}  // namespace arcjit
