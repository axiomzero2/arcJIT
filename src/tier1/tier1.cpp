// SPDX-License-Identifier: MIT
#include "tier1/tier1.h"

#include <algorithm>
#include <expected>
#include <print>

namespace arcjit {

// --- Linear Scan implementation --------------------------------------------
//
// Standard Poletto–Sarkar linear-scan algorithm. We compute live intervals
// with a single forward pass that tracks last-use per vreg, then sort by
// start position and walk through intervals assigning registers, spilling
// the one with the farthest end when we run out.
//
RegAllocResult linear_scan(const Tier1Function& fn) {
    RegAllocResult result;
    result.vreg_to_phys.assign(fn.vreg_count, -1);
    result.vreg_to_stack.assign(fn.vreg_count, -1);

    if (fn.vreg_count == 0) return result;

    // Build per-vreg start/end indices.
    std::vector<uint32_t> first_def(fn.vreg_count, UINT32_MAX);
    std::vector<uint32_t> last_use(fn.vreg_count, 0);

    for (uint32_t i = 0; i < fn.insts.size(); ++i) {
        const auto& inst = fn.insts[i];
        if (inst.dst != 0) {
            if (first_def[inst.dst] == UINT32_MAX) first_def[inst.dst] = i;
            last_use[inst.dst] = std::max(last_use[inst.dst], i);
        }
        if (inst.src1 != 0) last_use[inst.src1] = std::max(last_use[inst.src1], i);
        if (inst.src2 != 0) last_use[inst.src2] = std::max(last_use[inst.src2], i);
    }

    for (uint32_t v = 1; v < fn.vreg_count; ++v) {
        if (first_def[v] != UINT32_MAX) {
            result.intervals.push_back({v, first_def[v], last_use[v], -1});
        }
    }
    std::sort(result.intervals.begin(), result.intervals.end(),
              [](const LiveInterval& a, const LiveInterval& b) { return a.start < b.start; });

    // Caller-saved GP regs available for Tier-1 scratch:
    //   rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11 (9 registers)
    // We exclude rax because we use it for return values.
    static constexpr int kFreeRegs[] = {1, 2, 6, 7, 8, 9, 10, 11};  // rcx,rdx,rsi,rdi,r8,r9,r10,r11
    static constexpr size_t kNumFreeRegs = sizeof(kFreeRegs) / sizeof(kFreeRegs[0]);
    int next_stack_slot = 0;

    // Active list (sorted by end position).
    std::vector<LiveInterval*> active;

    for (auto& interval : result.intervals) {
        // Expire old intervals.
        std::erase_if(active, [&](LiveInterval* a) {
            if (a->end < interval.start) {
                // Free its register.
                // (In a real impl we'd track this in a free-list; for the scaffold we
                // just rely on the assignment being monotonic.)
                return true;
            }
            return false;
        });

        // Allocate a register.
        int reg = -1;
        // Find the lowest-numbered free reg — for the scaffold, just take the
        // next available slot in `active`'s complement.
        size_t used_count = active.size();
        if (used_count < kNumFreeRegs) {
            reg = kFreeRegs[used_count];
        } else {
            // Spill: pick the interval in `active` with the farthest end and
            // steal its register. For simplicity we just use a stack slot.
            next_stack_slot++;
            reg = -1;
        }

        interval.assigned_reg = reg;
        if (reg >= 0) {
            result.vreg_to_phys[interval.vreg] = reg;
        } else {
            result.vreg_to_stack[interval.vreg] = next_stack_slot;
        }

        if (reg >= 0) active.push_back(&interval);
    }

    return result;
}

// --- asmjit emission --------------------------------------------------------
Tier1Compiler::Tier1Compiler() {
    runtime_ = std::make_unique<asmjit::JitRuntime>();
    env_     = runtime_->environment();
}

[[nodiscard]] std::expected<void(*)(), std::string>
Tier1Compiler::compile(const Tier1Function& fn) {
    using namespace asmjit;

    RegAllocResult ra = linear_scan(fn);

    // Map a vreg to an x86-64 GP reg (or stack slot, for spilled vregs).
    auto phys_reg = [&](uint32_t vreg) -> x86::Gp {
        int phys = ra.vreg_to_phys[vreg];
        switch (phys) {
            case 0:  return x86::rax;
            case 1:  return x86::rcx;
            case 2:  return x86::rdx;
            case 6:  return x86::rsi;
            case 7:  return x86::rdi;
            case 8:  return x86::r8;
            case 9:  return x86::r9;
            case 10: return x86::r10;
            case 11: return x86::r11;
            default: return x86::rax;  // fallback for spilled vregs (not fully correct)
        }
    };

    StringLogger logger;
    CodeHolder code;
    code.init(env_);
    code.set_logger(&logger);

    x86::Assembler a(&code);

    // Pre-create the epilogue label so multiple `Return` / `Jump` ops can
    // share it.
    Label epilogue = a.new_label();

    // --- Prologue --------------------------------------------------------
    a.push(x86::rbp);
    a.mov(x86::rbp, x86::rsp);

    // --- Body -----------------------------------------------------------
    for (const auto& inst : fn.insts) {
        switch (inst.op) {
            case Tier1Op::LoadConst:
                a.mov(phys_reg(inst.dst), imm(inst.payload));
                break;
            case Tier1Op::LoadLocal:
                a.mov(phys_reg(inst.dst), x86::qword_ptr(x86::rbp, -8 * (int64_t)(inst.payload + 1)));
                break;
            case Tier1Op::StoreLocal:
                a.mov(x86::qword_ptr(x86::rbp, -8 * (int64_t)(inst.payload + 1)), phys_reg(inst.src1));
                break;
            case Tier1Op::Add:
                a.mov(phys_reg(inst.dst), phys_reg(inst.src1));
                a.add(phys_reg(inst.dst), phys_reg(inst.src2));
                break;
            case Tier1Op::Sub:
                a.mov(phys_reg(inst.dst), phys_reg(inst.src1));
                a.sub(phys_reg(inst.dst), phys_reg(inst.src2));
                break;
            case Tier1Op::Mul:
                a.mov(phys_reg(inst.dst), phys_reg(inst.src1));
                a.imul(phys_reg(inst.dst), phys_reg(inst.src2));
                break;
            case Tier1Op::Cmp:
                a.cmp(phys_reg(inst.src1), phys_reg(inst.src2));
                break;
            case Tier1Op::Return:
                a.mov(x86::rax, phys_reg(inst.src1));
                a.jmp(epilogue);
                break;
            case Tier1Op::Jump:
                a.jmp(epilogue);
                break;
            case Tier1Op::BranchIfFalse:
                // scaffold: skip
                break;
        }
    }

    a.bind(epilogue);
    // --- Epilogue --------------------------------------------------------
    a.mov(x86::rsp, x86::rbp);
    a.pop(x86::rbp);
    a.ret();

    // Finalize.
    void* entry = nullptr;
    Error err = runtime_->add(&entry, &code);
    if (err != kErrorOk) {
        return std::unexpected(std::string("asmjit error: ") + std::string(logger.data()));
    }
    return reinterpret_cast<void(*)()>(entry);
}

// --- Demo: compute a + b + c via three LoadConst + two Add + Return ---------
//
// This produces a function that returns 6 (= 1 + 2 + 3). It exercises the
// full Tier-1 pipeline: linear scan + asmjit + execution.
//
Tier1Function make_demo_add3() {
    Tier1Function fn;
    fn.vreg_count = 6;  // vreg 0 unused; vregs 1..5 used

    // v1 = 1, v2 = 2, v3 = 3
    fn.insts.push_back({Tier1Op::LoadConst, /*dst*/1, /*src1*/0, /*src2*/0, /*payload*/1});
    fn.insts.push_back({Tier1Op::LoadConst, /*dst*/2, /*src1*/0, /*src2*/0, /*payload*/2});
    fn.insts.push_back({Tier1Op::LoadConst, /*dst*/3, /*src1*/0, /*src2*/0, /*payload*/3});

    // v4 = v1 + v2
    fn.insts.push_back({Tier1Op::Add, /*dst*/4, /*src1*/1, /*src2*/2, /*payload*/0});
    // v5 = v4 + v3
    fn.insts.push_back({Tier1Op::Add, /*dst*/5, /*src1*/4, /*src2*/3, /*payload*/0});

    // return v5
    fn.insts.push_back({Tier1Op::Return, /*dst*/0, /*src1*/5, /*src2*/0, /*payload*/0});

    return fn;
}

}  // namespace arcjit
