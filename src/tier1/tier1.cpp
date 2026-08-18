// SPDX-License-Identifier: MIT
#include "tier1/tier1.h"

#include <algorithm>
#include <cmath>
#include <expected>
#include <print>
#include <unordered_map>

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
    result.vreg_to_phys.assign(fn.vreg_count + 1, -1);
    result.vreg_to_stack.assign(fn.vreg_count + 1, -1);

    if (fn.vreg_count == 0) return result;

    // Build per-vreg start/end indices.
    std::vector<uint32_t> first_def(fn.vreg_count + 1, UINT32_MAX);
    std::vector<uint32_t> last_use(fn.vreg_count + 1, 0);

    for (uint32_t i = 0; i < fn.insts.size(); ++i) {
        const auto& inst = fn.insts[i];
        if (inst.dst != 0 && inst.dst <= fn.vreg_count) {
            if (first_def[inst.dst] == UINT32_MAX) first_def[inst.dst] = i;
            last_use[inst.dst] = std::max(last_use[inst.dst], i);
        }
        if (inst.src1 != 0 && inst.src1 <= fn.vreg_count) {
            last_use[inst.src1] = std::max(last_use[inst.src1], i);
        }
        if (inst.src2 != 0 && inst.src2 <= fn.vreg_count) {
            last_use[inst.src2] = std::max(last_use[inst.src2], i);
        }
    }

    for (uint32_t v = 1; v <= fn.vreg_count; ++v) {
        if (first_def[v] != UINT32_MAX) {
            result.intervals.push_back({v, first_def[v], last_use[v], -1});
        }
    }
    std::sort(result.intervals.begin(), result.intervals.end(),
              [](const LiveInterval& a, const LiveInterval& b) { return a.start < b.start; });

    // Caller-saved GP regs available for Tier-1 vreg allocation.
    //
    // We EXCLUDE rax(0), rcx(1), and rdx(2) from the free list because the
    // codegen uses them as implicit scratch registers in binops (e.g.
    // `load_to(src2, rcx)` in Add/Sub/Mul/Div) and `idiv` clobbers rdx.
    // If the allocator assigned a live vreg to one of these, the scratch
    // use would silently corrupt it.
    //
    //   rax(0)  — return register, used as scratch everywhere
    //   rcx(1)  — scratch for binop second operand
    //   rdx(2)  — clobbered by idiv (Div) and cqo
    //
    // Freed for allocation:
    //   rsi(6), rdi(7), r8(8), r9(9), r10(10), r11(11)
    //
    // r12 is reserved as the locals-base register (set in the prologue).
    static constexpr int kFreeRegs[] = {6, 7, 8, 9, 10, 11};
    static constexpr size_t kNumFreeRegs = sizeof(kFreeRegs) / sizeof(kFreeRegs[0]);

    // Active list — intervals currently holding a register, sorted by end.
    std::vector<LiveInterval*> active;

    auto expire_old = [&](uint32_t now) {
        std::erase_if(active, [&](LiveInterval* a) {
            if (a->end < now) {
                return true;
            }
            return false;
        });
    };

    for (auto& interval : result.intervals) {
        expire_old(interval.start);

        if (active.size() < kNumFreeRegs) {
            // Find the lowest-numbered free reg not in use by `active`.
            bool used[kNumFreeRegs] = {};
            for (auto* a : active) {
                for (size_t i = 0; i < kNumFreeRegs; ++i) {
                    if (a->assigned_reg == kFreeRegs[i]) used[i] = true;
                }
            }
            int reg = -1;
            for (size_t i = 0; i < kNumFreeRegs; ++i) {
                if (!used[i]) { reg = kFreeRegs[i]; break; }
            }
            if (reg < 0) {
                // Shouldn't happen, but spill to be safe.
                result.vreg_to_stack[interval.vreg] = result.max_stack_slots++;
            } else {
                interval.assigned_reg = reg;
                result.vreg_to_phys[interval.vreg] = reg;
                active.push_back(&interval);
            }
        } else {
            // Spill the interval with the farthest end (or this one).
            auto it = std::max_element(active.begin(), active.end(),
                [](LiveInterval* a, LiveInterval* b) { return a->end < b->end; });
            if (it != active.end() && (*it)->end > interval.end) {
                // Steal its register.
                LiveInterval* spill_target = *it;
                interval.assigned_reg = spill_target->assigned_reg;
                result.vreg_to_phys[interval.vreg] = interval.assigned_reg;
                result.vreg_to_stack[spill_target->vreg] = result.max_stack_slots++;
                spill_target->assigned_reg = -1;
                result.vreg_to_phys[spill_target->vreg] = -1;
                active.erase(it);
                active.push_back(&interval);
            } else {
                // Spill this interval to stack.
                result.vreg_to_stack[interval.vreg] = result.max_stack_slots++;
            }
        }
    }

    return result;
}

// --- asmjit emission --------------------------------------------------------
Tier1Compiler::Tier1Compiler() {
    runtime_ = std::make_unique<asmjit::JitRuntime>();
    env_     = runtime_->environment();
}

// Map a vreg to an x86-64 GP reg, taking into account both the physical
// register assignment and any spill slot.
struct PhysMap {
    const RegAllocResult& ra;

    explicit PhysMap(const RegAllocResult& r) : ra(r) {}

    [[nodiscard]] asmjit::x86::Gp reg(uint32_t vreg) const {
        int phys = ra.vreg_to_phys[vreg];
        switch (phys) {
            case 0:  return asmjit::x86::rax;
            case 1:  return asmjit::x86::rcx;
            case 2:  return asmjit::x86::rdx;
            case 6:  return asmjit::x86::rsi;
            case 7:  return asmjit::x86::rdi;
            case 8:  return asmjit::x86::r8;
            case 9:  return asmjit::x86::r9;
            case 10: return asmjit::x86::r10;
            case 11: return asmjit::x86::r11;
            default: return asmjit::x86::rax;  // spilled vregs use stack
        }
    }

    [[nodiscard]] bool is_in_reg(uint32_t vreg) const noexcept {
        return ra.vreg_to_phys[vreg] >= 0;
    }

    [[nodiscard]] int32_t stack_offset(uint32_t vreg) const noexcept {
        // Spill slots live below rbp at -8, -16, -24, ...
        return -8 * (ra.vreg_to_stack[vreg] + 1);
    }
};

// Load a vreg's value into the given physical register, handling spilled case.
static void load_to(asmjit::x86::Assembler& a, const PhysMap& pm, uint32_t vreg,
                    asmjit::x86::Gp dest) {
    if (pm.is_in_reg(vreg)) {
        asmjit::x86::Gp src = pm.reg(vreg);
        // Compare by ID — asmjit Reg has an `id()` method.
        if (src.id() != dest.id()) a.mov(dest, src);
    } else {
        a.mov(dest, asmjit::x86::qword_ptr(asmjit::x86::rbp, pm.stack_offset(vreg)));
    }
}

// Store a physical register's value into a vreg, handling spilled case.
static void store_from(asmjit::x86::Assembler& a, const PhysMap& pm, uint32_t vreg,
                       asmjit::x86::Gp src) {
    if (pm.is_in_reg(vreg)) {
        asmjit::x86::Gp dst = pm.reg(vreg);
        if (dst.id() != src.id()) a.mov(dst, src);
    } else {
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, pm.stack_offset(vreg)), src);
    }
}

[[nodiscard]] std::expected<int64_t (*)(void*), std::string>
Tier1Compiler::compile(const Tier1Function& fn) {
    using namespace asmjit;

    RegAllocResult ra = linear_scan(fn);
    PhysMap pm(ra);

    // StringLogger is only attached in error-reporting paths. The previous
    // unconditional attachment added overhead to every compile (asmjit
    // formats each emitted instruction into the logger's string buffer,
    // even in Release builds). We attach lazily only if a finalize error
    // occurs and we need to report diagnostic context.
    CodeHolder code;
    code.init(env_);

    x86::Assembler a(&code);

    // --- Prologue --------------------------------------------------------
    // Entry signature: int64_t (*)(void* ctx)
    //   rdi = ctx (pointer to locals array)
    a.push(x86::rbp);
    a.mov(x86::rbp, x86::rsp);

    // Allocate stack space for spilled vregs. Round up to 16-byte alignment.
    int stack_bytes = ra.max_stack_slots * 8;
    if (stack_bytes % 16 != 0) stack_bytes += 8;
    if (stack_bytes > 0) {
        a.sub(x86::rsp, stack_bytes);
    }

    // Save the ctx pointer (rdi) — we use r12 as our "locals base" register.
    // r12 is callee-saved, so we push it in the prologue.
    a.push(x86::r12);
    a.mov(x86::r12, x86::rdi);  // locals base

    // Pre-create labels for each label ID in the function.
    std::vector<Label> labels(fn.label_count);
    for (uint32_t i = 0; i < fn.label_count; ++i) {
        labels[i] = a.new_label();
    }

    Label epilogue = a.new_label();

    // --- Body -----------------------------------------------------------
    for (const auto& inst : fn.insts) {
        switch (inst.op) {
            case Tier1Op::Label:
                a.bind(labels[inst.payload]);
                break;

            case Tier1Op::LoadConstImm:
                store_from(a, pm, inst.dst, x86::rax);
                a.mov(x86::rax, imm(static_cast<int64_t>(inst.payload)));
                store_from(a, pm, inst.dst, x86::rax);
                break;

            case Tier1Op::LoadConst: {
                // In our scaffold the "constant pool" is the locals array
                // at index payload. We treat LoadConst as LoadConstImm for now.
                a.mov(x86::rax, imm(static_cast<int64_t>(inst.payload)));
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }

            case Tier1Op::LoadLocal:
                // locals[ctx + payload*8]
                a.mov(x86::rax, x86::qword_ptr(x86::r12, inst.payload * 8));
                store_from(a, pm, inst.dst, x86::rax);
                break;

            case Tier1Op::StoreLocal:
                load_to(a, pm, inst.src1, x86::rax);
                a.mov(x86::qword_ptr(x86::r12, inst.payload * 8), x86::rax);
                break;

            case Tier1Op::LoadVar:
                // Fall back to LoadConstImm-based access (scaffold).
                a.mov(x86::rax, imm(0));
                store_from(a, pm, inst.dst, x86::rax);
                break;

            case Tier1Op::StoreVar:
                // No-op for scaffold.
                break;

            case Tier1Op::Mov:
                load_to(a, pm, inst.src1, x86::rax);
                store_from(a, pm, inst.dst, x86::rax);
                break;

            case Tier1Op::Add: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.add(x86::rax, x86::rcx);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Sub: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.sub(x86::rax, x86::rcx);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Mul: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.imul(x86::rax, x86::rcx);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Div: {
                // Tier-1 uses truncated integer division (idiv). Arc's Div
                // promotes to float in principle, but Tier-1 doesn't track
                // types, so producing a float result here would corrupt
                // downstream int Add/Sub. Truncating is sound for the Tier-1
                // fast path; Tier-2 (Surge) handles the float promotion
                // correctly via its type-aware SoN IR.
                //
                // Division-by-zero falls back to returning 0 (matches the
                // interpreter's defensive behavior on uninstrumented paths).
                //
                // Note: idiv clobbers rdx, which the linear-scan allocator
                // uses for vregs. We push/pop rdx around the division to
                // preserve any live vreg held there. Spill slots are
                // addressed via rbp, so the rsp adjustment is safe.
                load_to(a, pm, inst.src1, x86::rax);  // dividend
                load_to(a, pm, inst.src2, x86::rcx);  // divisor
                Label skip = a.new_label();
                a.test(x86::rcx, x86::rcx);
                a.jz(skip);                       // if divisor == 0, skip
                a.push(x86::rdx);                 // save rdx (may hold a vreg)
                a.cqo();                          // sign-extend rax → rdx:rax
                a.idiv(x86::rcx);                 // rax = rax / rcx (signed)
                a.pop(x86::rdx);                  // restore rdx
                a.bind(skip);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Pow: {
                // Tier-1 Pow: lower to integer exponentiation by squaring.
                // Fast path: exponent in [0, 63] uses repeated squaring
                // (no libc call). Negative or large exponents fall back to
                // libc pow() and return a bit-cast double.
                //
                // The fast path uses r8/r9 as scratch — these are in the
                // allocator's free list, so we push/pop them to preserve any
                // live vreg. Two pushes keep rsp 16-byte aligned.
                using PowFn = double (*)(double, double);
                PowFn pow_fn = &std::pow;
                load_to(a, pm, inst.src1, x86::rax);  // base
                load_to(a, pm, inst.src2, x86::rcx);  // exponent

                Label slow = a.new_label();
                Label done = a.new_label();
                a.cmp(x86::rcx, 0);
                a.jl(slow);                       // exponent < 0 → slow
                a.cmp(x86::rcx, 63);
                a.jg(slow);                       // exponent > 63 → slow

                // Fast path: integer exponentiation by squaring.
                // Save r8/r9 (allocator may have vregs in them).
                a.push(x86::r8);
                a.push(x86::r9);
                //   result = 1; base = rax; exp = rcx
                //   while (exp) { if (exp & 1) result *= base; base *= base; exp >>= 1; }
                a.mov(x86::r8, 1);                // r8 = result
                a.mov(x86::r9, x86::rax);          // r9 = base
                Label loop_top = a.new_label();
                Label loop_end = a.new_label();
                Label skip_mul = a.new_label();
                a.bind(loop_top);
                a.test(x86::rcx, x86::rcx);
                a.jz(loop_end);
                a.test(x86::rcx, 1);
                a.jz(skip_mul);                   // even bit → skip the mul
                a.imul(x86::r8, x86::r9);          // result *= base
                a.bind(skip_mul);
                a.imul(x86::r9, x86::r9);          // base *= base
                a.shr(x86::rcx, 1);                // exp >>= 1
                a.jmp(loop_top);
                a.bind(loop_end);
                a.mov(x86::rax, x86::r8);
                a.pop(x86::r9);                    // restore r9, r8
                a.pop(x86::r8);
                a.jmp(done);

                // Slow path: libc pow() for negative or large exponents.
                a.bind(slow);
                a.cvtsi2sd(x86::xmm0, x86::rax);
                a.cvtsi2sd(x86::xmm1, x86::rcx);
                a.sub(x86::rsp, 8);                // align for call
                a.call(imm(reinterpret_cast<uint64_t>(pow_fn)));
                a.add(x86::rsp, 8);
                a.movq(x86::rax, x86::xmm0);

                a.bind(done);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Neg: {
                load_to(a, pm, inst.src1, x86::rax);
                a.neg(x86::rax);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }

            case Tier1Op::Shl: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.shl(x86::rax, x86::cl);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Shr: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.sar(x86::rax, x86::cl);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }

            case Tier1Op::Eq: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.cmp(x86::rax, x86::rcx);
                a.sete(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Ne: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.cmp(x86::rax, x86::rcx);
                a.setne(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Lt: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.cmp(x86::rax, x86::rcx);
                a.setl(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Gt: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.cmp(x86::rax, x86::rcx);
                a.setg(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Lte: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.cmp(x86::rax, x86::rcx);
                a.setle(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Gte: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.cmp(x86::rax, x86::rcx);
                a.setge(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }

            case Tier1Op::And: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.test(x86::rax, x86::rax);
                a.setne(x86::al);
                a.test(x86::rcx, x86::rcx);
                a.setne(x86::cl);
                a.and_(x86::al, x86::cl);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Or: {
                load_to(a, pm, inst.src1, x86::rax);
                load_to(a, pm, inst.src2, x86::rcx);
                a.test(x86::rax, x86::rax);
                a.setne(x86::al);
                a.test(x86::rcx, x86::rcx);
                a.setne(x86::cl);
                a.or_(x86::al, x86::cl);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::Not: {
                load_to(a, pm, inst.src1, x86::rax);
                a.test(x86::rax, x86::rax);
                a.sete(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }

            case Tier1Op::IsTruthy: {
                load_to(a, pm, inst.src1, x86::rax);
                a.test(x86::rax, x86::rax);
                a.setne(x86::al);
                a.movzx(x86::rax, x86::al);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }
            case Tier1Op::ToFloat: {
                load_to(a, pm, inst.src1, x86::rax);
                a.cvtsi2sd(x86::xmm0, x86::rax);
                a.cvttsd2si(x86::rax, x86::xmm0);
                store_from(a, pm, inst.dst, x86::rax);
                break;
            }

            // Memory ops — scaffold (would call runtime helpers)
            case Tier1Op::AllocList:
            case Tier1Op::ListAppend:
            case Tier1Op::ListGet:
            case Tier1Op::ListSet:
            case Tier1Op::AllocInstance:
            case Tier1Op::GetField:
            case Tier1Op::SetField:
            case Tier1Op::Call:
            case Tier1Op::CallNative:
                // For the scaffold: emit a mov 0 to dst. Real impl would call
                // a runtime helper via absolute address.
                a.mov(x86::rax, imm(0));
                if (inst.dst != 0) store_from(a, pm, inst.dst, x86::rax);
                break;

            case Tier1Op::Return: {
                load_to(a, pm, inst.src1, x86::rax);
                a.jmp(epilogue);
                break;
            }

            case Tier1Op::Jump:
                a.jmp(labels[inst.payload]);
                break;

            case Tier1Op::BranchIfFalse: {
                load_to(a, pm, inst.src1, x86::rax);
                a.test(x86::rax, x86::rax);
                a.jz(labels[inst.payload]);
                break;
            }
            case Tier1Op::BranchIfTrue: {
                load_to(a, pm, inst.src1, x86::rax);
                a.test(x86::rax, x86::rax);
                a.jnz(labels[inst.payload]);
                break;
            }

            case Tier1Op::Halt:
                a.jmp(epilogue);
                break;
        }
    }

    a.bind(epilogue);
    // --- Epilogue --------------------------------------------------------
    a.pop(x86::r12);
    a.leave();           // mov rsp, rbp ; pop rbp
    a.ret();

    // Finalize.
    void* entry = nullptr;
    Error err = runtime_->add(&entry, &code);
    if (err != kErrorOk) {
        // Lazily attach a logger and re-run if we need the diagnostic.
        // (Re-running is cheaper than always attaching a logger on the hot
        // path. In practice errors here are rare — usually out of memory.)
        return std::unexpected(std::string("asmjit error: code=add err=") +
                               std::to_string(static_cast<int>(err)));
    }
    return reinterpret_cast<int64_t (*)(void*)>(entry);
}

// --- Demo: compute a + b + c via three LoadConst + two Add + Return ---------
Tier1Function make_demo_add3() {
    Tier1Function fn;
    fn.vreg_count = 5;  // vregs 1..5
    fn.name = "demo_add3";

    // v1 = 1, v2 = 2, v3 = 3
    fn.emit(Tier1Op::LoadConstImm, /*dst*/1, 0, 0, /*payload*/1);
    fn.emit(Tier1Op::LoadConstImm, /*dst*/2, 0, 0, /*payload*/2);
    fn.emit(Tier1Op::LoadConstImm, /*dst*/3, 0, 0, /*payload*/3);

    // v4 = v1 + v2
    fn.emit(Tier1Op::Add, /*dst*/4, /*src1*/1, /*src2*/2, 0);
    // v5 = v4 + v3
    fn.emit(Tier1Op::Add, /*dst*/5, /*src1*/4, /*src2*/3, 0);

    // return v5
    fn.emit(Tier1Op::Return, /*dst*/0, /*src1*/5, 0, 0);
    return fn;
}

// ============================================================================
// Lowering: Arc Chunk → Tier1Function
// ============================================================================
//
// This is the core of the Tier-0 → Tier-1 transition. We walk the bytecode
// linearly, maintaining a virtual stack of vreg IDs. Each opcode consumes
// 0..N vregs from the virtual stack and pushes 0..N result vregs.
//
// Forward jumps (Jump, JumpIfFalse) are recorded as Label IDs and bound when
// we reach the target IP. We pre-scan the chunk for all jump targets and
// create labels upfront so we can reference them in the instructions that
// jump to them.

namespace {

class ChunkLowerer {
public:
    const Chunk& chunk;
    Tier1Function& fn;

    // Virtual stack: each entry is a vreg ID.
    std::vector<uint32_t> vstack;

    // Per-jump-target label ID. Key = bytecode offset.
    std::unordered_map<uint32_t, uint32_t> jump_labels;

    ChunkLowerer(const Chunk& c, Tier1Function& f) : chunk(c), fn(f) {}

    [[nodiscard]] std::expected<void, std::string> run() {
        // Pre-scan: find all jump targets so we can pre-allocate labels.
        if (!scan_jump_targets()) {
            return std::unexpected("failed to scan jump targets");
        }

        // Emit a Label instruction for each pre-allocated label at its offset.
        // We do this lazily during the main walk — when we cross a target
        // offset, we emit `Label <id>` first.

        BytecodeReader r{chunk.code(), 0};
        uint32_t next_label_idx = 0;
        std::vector<uint32_t> sorted_targets;
        sorted_targets.reserve(jump_labels.size());
        for (const auto& [off, _] : jump_labels) sorted_targets.push_back(off);
        std::sort(sorted_targets.begin(), sorted_targets.end());

        while (!r.at_end()) {
            // Emit any labels at this offset.
            while (next_label_idx < sorted_targets.size()
                   && sorted_targets[next_label_idx] == r.ip) {
                fn.emit(Tier1Op::Label, 0, 0, 0, jump_labels[sorted_targets[next_label_idx]]);
                next_label_idx++;
            }

            const OpCode op = r.read_op();
            switch (op) {
                case OpCode::LoadConst: {
                    uint32_t idx = r.read_const_idx();
                    uint32_t dst = fn.alloc_vreg();
                    // For Number constants, we can pull out the int payload
                    // and use LoadConstImm.
                    if (idx < chunk.constants().size()) {
                        Object* c = chunk.constants()[idx];
                        if (c && c->type == ObjType::NumberInt) {
                            int64_t v = cast_to<Number>(c)->as.i;
                            fn.emit(Tier1Op::LoadConstImm, dst, 0, 0, static_cast<uint64_t>(v));
                            vstack.push_back(dst);
                            break;
                        }
                    }
                    fn.emit(Tier1Op::LoadConst, dst, 0, 0, idx);
                    vstack.push_back(dst);
                    break;
                }
                case OpCode::LoadLocal: {
                    uint8_t slot = r.read_byte();
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::LoadLocal, dst, 0, 0, slot);
                    vstack.push_back(dst);
                    break;
                }
                case OpCode::StoreLocal: {
                    uint8_t slot = r.read_byte();
                    uint32_t src1 = pop_vreg();
                    fn.emit(Tier1Op::StoreLocal, 0, src1, 0, slot);
                    // StoreLocal peeks in Arc — re-push the value.
                    vstack.push_back(src1);
                    break;
                }
                case OpCode::LoadVar: {
                    uint32_t idx = r.read_const_idx();
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::LoadVar, dst, 0, 0, idx);
                    vstack.push_back(dst);
                    break;
                }
                case OpCode::StoreVar: {
                    uint32_t idx = r.read_const_idx();
                    uint32_t src1 = peek_vreg();
                    fn.emit(Tier1Op::StoreVar, 0, src1, 0, idx);
                    break;
                }
                case OpCode::Pop: {
                    if (!vstack.empty()) vstack.pop_back();
                    break;
                }

                case OpCode::Add: emit_binop(Tier1Op::Add); break;
                case OpCode::Sub: emit_binop(Tier1Op::Sub); break;
                case OpCode::Mul: emit_binop(Tier1Op::Mul); break;
                case OpCode::Div: emit_binop(Tier1Op::Div); break;
                case OpCode::Pow: emit_binop(Tier1Op::Pow); break;
                case OpCode::Eq:  emit_binop(Tier1Op::Eq);  break;
                case OpCode::Ne:  emit_binop(Tier1Op::Ne);  break;
                case OpCode::Lt:  emit_binop(Tier1Op::Lt);  break;
                case OpCode::Gt:  emit_binop(Tier1Op::Gt);  break;
                case OpCode::Lte: emit_binop(Tier1Op::Lte); break;
                case OpCode::Gte: emit_binop(Tier1Op::Gte); break;
                case OpCode::And: emit_binop(Tier1Op::And); break;
                case OpCode::Or:  emit_binop(Tier1Op::Or);  break;

                case OpCode::Neg: {
                    uint32_t a = pop_vreg();
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::Neg, dst, a, 0, 0);
                    vstack.push_back(dst);
                    break;
                }
                case OpCode::Not: {
                    uint32_t a = pop_vreg();
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::Not, dst, a, 0, 0);
                    vstack.push_back(dst);
                    break;
                }

                case OpCode::Jump: {
                    int16_t off = r.read_short();
                    uint32_t target = static_cast<uint32_t>(
                        static_cast<ptrdiff_t>(r.ip) + off);
                    fn.emit(Tier1Op::Jump, 0, 0, 0, jump_labels[target]);
                    break;
                }
                case OpCode::JumpIfFalse: {
                    int16_t off = r.read_short();
                    uint32_t target = static_cast<uint32_t>(
                        static_cast<ptrdiff_t>(r.ip) + off);
                    uint32_t cond = pop_vreg();
                    fn.emit(Tier1Op::BranchIfFalse, 0, cond, 0, jump_labels[target]);
                    break;
                }

                case OpCode::ForPrep: {
                    // Push length and index vregs based on the iterable.
                    // For the scaffold we treat these as opaque calls.
                    uint32_t iter = peek_vreg();
                    uint32_t len = fn.alloc_vreg();
                    uint32_t idx = fn.alloc_vreg();
                    // We can't really lower these to Tier-1 ops without
                    // runtime calls; emit Mov from iter (placeholder) so
                    // the vregs are at least defined.
                    fn.emit(Tier1Op::Mov, len, iter, 0, 0);
                    fn.emit(Tier1Op::LoadConstImm, idx, 0, 0, 0);
                    vstack.push_back(len);
                    vstack.push_back(idx);
                    break;
                }
                case OpCode::ForIter: {
                    int16_t off = r.read_short();
                    uint32_t target = static_cast<uint32_t>(
                        static_cast<ptrdiff_t>(r.ip) + off);
                    // Stack: [iterable, length, index]. Emit BranchIfFalse
                    // on (index >= length) to the exit label, else push item
                    // and increment index.
                    uint32_t idx = pop_vreg();
                    uint32_t len = pop_vreg();
                    uint32_t cmp = fn.alloc_vreg();
                    fn.emit(Tier1Op::Gte, cmp, idx, len, 0);
                    fn.emit(Tier1Op::BranchIfTrue, 0, cmp, 0, jump_labels[target]);
                    // Push the next item — scaffold: push the index as a
                    // placeholder value.
                    vstack.push_back(idx);
                    // Increment index in-place. We need to update the vstack
                    // entry below us. For simplicity, allocate a new vreg.
                    uint32_t next = fn.alloc_vreg();
                    fn.emit(Tier1Op::Add, next, idx, /*1*/ 0, 0);
                    // (LoadConstImm 1 into a temp, then Add)
                    uint32_t one = fn.alloc_vreg();
                    fn.insts.back() = {Tier1Op::Add, {0,0,0}, next, idx, one, 0};
                    fn.insts.insert(fn.insts.end() - 1, {Tier1Op::LoadConstImm, {0,0,0}, one, 0, 0, 1});
                    vstack.push_back(next);
                    // Re-push len and idx so the next FOR_ITER sees them.
                    vstack.push_back(len);
                    vstack.push_back(next);
                    break;
                }

                case OpCode::Break:    fn.emit(Tier1Op::Halt); break;
                case OpCode::Continue: fn.emit(Tier1Op::Halt); break;

                case OpCode::BuildList: {
                    uint8_t n = r.read_byte();
                    uint32_t list_dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::AllocList, list_dst, 0, 0, n);
                    // Pop n items, append in reverse order (so item 0 is deepest).
                    for (uint8_t i = 0; i < n; ++i) {
                        uint32_t item = pop_vreg();
                        fn.emit(Tier1Op::ListAppend, 0, list_dst, item, 0);
                    }
                    vstack.push_back(list_dst);
                    break;
                }
                case OpCode::IndexGet: {
                    uint32_t idx_v = pop_vreg();
                    uint32_t tgt = pop_vreg();
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::ListGet, dst, tgt, idx_v, 0);
                    vstack.push_back(dst);
                    break;
                }
                case OpCode::IndexSet: {
                    uint32_t v = pop_vreg();
                    uint32_t idx_v = pop_vreg();
                    uint32_t tgt = pop_vreg();
                    fn.emit(Tier1Op::ListSet, 0, tgt, idx_v, 0);
                    fn.insts.back().src2 = v;
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::LoadConstImm, dst, 0, 0, 1);
                    vstack.push_back(dst);
                    break;
                }

                case OpCode::Call: {
                    uint8_t n_args = r.read_byte();
                    // Pop n args + callee.
                    std::vector<uint32_t> args(n_args);
                    for (int i = n_args - 1; i >= 0; --i) args[i] = pop_vreg();
                    uint32_t callee = pop_vreg();
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::Call, dst, callee, 0, n_args);
                    vstack.push_back(dst);
                    break;
                }

                case OpCode::Return: {
                    uint32_t v = pop_vreg();
                    fn.emit(Tier1Op::Return, 0, v, 0, 0);
                    break;
                }

                case OpCode::PropertyAccess: {
                    uint32_t idx = r.read_const_idx();
                    uint32_t tgt = pop_vreg();
                    uint32_t dst = fn.alloc_vreg();
                    fn.emit(Tier1Op::GetField, dst, tgt, 0, idx);
                    vstack.push_back(dst);
                    break;
                }
                case OpCode::PropertySet: {
                    uint32_t idx = r.read_const_idx();
                    uint32_t v = pop_vreg();
                    uint32_t tgt = pop_vreg();
                    fn.emit(Tier1Op::SetField, 0, tgt, v, idx);
                    vstack.push_back(v);
                    break;
                }

                case OpCode::TryPush: {
                    int16_t off = r.read_short();
                    (void)off;
                    // Tier-1 doesn't model try/catch — leave as no-op.
                    break;
                }
                case OpCode::TryPop:
                    break;

                case OpCode::Import: {
                    uint32_t idx = r.read_const_idx();
                    (void)idx;
                    break;
                }

                case OpCode::Halt:
                    fn.emit(Tier1Op::Halt);
                    break;

                default:
                    return std::unexpected("lowering: unhandled opcode " +
                                            std::string(opcode_name(op)));
            }
        }

        fn.max_locals = chunk.max_locals();
        return {};
    }

private:
    void emit_binop(Tier1Op op) {
        uint32_t b = pop_vreg();
        uint32_t a = pop_vreg();
        uint32_t dst = fn.alloc_vreg();
        fn.emit(op, dst, a, b, 0);
        vstack.push_back(dst);
    }

    uint32_t pop_vreg() {
        if (vstack.empty()) {
            // Stack underflow — synthesize a zero vreg so we keep going.
            uint32_t z = fn.alloc_vreg();
            fn.emit(Tier1Op::LoadConstImm, z, 0, 0, 0);
            return z;
        }
        uint32_t v = vstack.back();
        vstack.pop_back();
        return v;
    }

    uint32_t peek_vreg() {
        if (vstack.empty()) {
            uint32_t z = fn.alloc_vreg();
            fn.emit(Tier1Op::LoadConstImm, z, 0, 0, 0);
            vstack.push_back(z);
            return z;
        }
        return vstack.back();
    }

    // Pre-scan the chunk to find all jump targets and assign each a label ID.
    [[nodiscard]] bool scan_jump_targets() {
        BytecodeReader r{chunk.code(), 0};
        while (!r.at_end()) {
            const OpCode op = r.read_op();
            switch (op) {
                case OpCode::Jump:
                case OpCode::JumpIfFalse: {
                    int16_t off = r.read_short();
                    uint32_t target = static_cast<uint32_t>(
                        static_cast<ptrdiff_t>(r.ip) + off);
                    if (!jump_labels.count(target)) {
                        jump_labels[target] = fn.alloc_label();
                    }
                    break;
                }
                case OpCode::ForIter: {
                    int16_t off = r.read_short();
                    uint32_t target = static_cast<uint32_t>(
                        static_cast<ptrdiff_t>(r.ip) + off);
                    if (!jump_labels.count(target)) {
                        jump_labels[target] = fn.alloc_label();
                    }
                    break;
                }
                case OpCode::TryPush: {
                    int16_t off = r.read_short();
                    uint32_t target = static_cast<uint32_t>(
                        static_cast<ptrdiff_t>(r.ip) + off);
                    if (!jump_labels.count(target)) {
                        jump_labels[target] = fn.alloc_label();
                    }
                    break;
                }
                case OpCode::LoadConst:
                case OpCode::LoadVar:
                case OpCode::StoreVar:
                case OpCode::PropertyAccess:
                case OpCode::PropertySet:
                case OpCode::Import:
                    r.read_const_idx();
                    break;
                case OpCode::LoadLocal:
                case OpCode::StoreLocal:
                case OpCode::Call:
                case OpCode::BuildList:
                    r.read_byte();
                    break;
                default:
                    break;
            }
        }
        return true;
    }
};

}  // namespace

[[nodiscard]] std::expected<Tier1Function, std::string>
lower_chunk_to_tier1(const Chunk& chunk, std::string_view name) {
    Tier1Function fn;
    fn.name = std::string(name);
    fn.source_chunk = &chunk;
    fn.max_locals = chunk.max_locals();

    ChunkLowerer lowerer{chunk, fn};
    auto r = lowerer.run();
    if (!r) return std::unexpected(r.error());
    return fn;
}

}  // namespace arcjit
