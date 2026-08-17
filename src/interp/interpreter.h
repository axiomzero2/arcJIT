// SPDX-License-Identifier: MIT
// arcJIT — Tier-0 interpreter.
//
// Executes Arc's stack bytecode with the same fast paths as the upstream
// `vmRun()` in `src/vm.c`, plus safepoint polling at loop back-edges and
// call sites, plus type-feedback and inline-cache collection for Tier 1/2.
#pragma once

#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "bytecode/chunk.h"
#include "bytecode/object.h"
#include "bytecode/value.h"
#include "runtime/profile.h"
#include "runtime/safepoint.h"

namespace arcjit {

// --- Call frame (mirrors Arc's CallFrame) -----------------------------------
struct CallFrame {
    const Chunk*    chunk = nullptr;
    BytecodeReader  reader{};
    uint32_t        locals_base = 0;
    uint32_t        local_count = 0;
    Object*         instance = nullptr;
};

// --- Interpreter result -----------------------------------------------------
enum class InterpError {
    None,
    DivByZero,
    TypeError,
    NameError,
    ValueError,
    StackOverflow,
    UnknownOpcode,
};

struct InterpResult {
    Value       value;
    InterpError error = InterpError::None;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return error == InterpError::None; }
};

// --- Interpreter ------------------------------------------------------------
//
// A simple switch-dispatch interpreter that mirrors Arc's `vmRun()` semantics.
// For the scaffold we implement only the opcodes needed for the demo
// (arithmetic, locals, branches, return, halt). The full opcode set is wired
// as no-ops so the dispatch table compiles cleanly.
//
class Interpreter {
public:
    static constexpr size_t kStackMax       = 4096;
    static constexpr size_t kLocalsMax      = 65536;
    static constexpr size_t kFrameMax       = 8192;
    static constexpr size_t kTryMax         = 256;
    static constexpr uint32_t kHotThreshold = 200;  // invocations before Tier 1 fires

    Interpreter() {
        stack_.reserve(kStackMax);
        locals_.resize(kLocalsMax);
        frames_.reserve(kFrameMax);
    }

    ~Interpreter() {
        if (mgr_) mgr_->unregister_mutator(&mutator_state_);
    }

    void attach_safepoint(SafepointManager* mgr) {
        mgr_ = mgr;
        if (mgr_) mgr_->register_mutator(&mutator_state_);
    }

    // Run a chunk. Returns the top-of-stack value (the chunk's "result").
    [[nodiscard]] std::expected<Value, std::string> run(const Chunk& chunk) {
        if (frames_.size() >= kFrameMax) {
            return std::unexpected("stack overflow");
        }
        CallFrame& frame = frames_.emplace_back();
        frame.chunk       = &chunk;
        frame.reader      = BytecodeReader{chunk.code(), 0};
        frame.locals_base = locals_top_;
        frame.local_count = static_cast<uint32_t>(chunk.max_locals());
        locals_top_ += chunk.max_locals();

        for (uint32_t i = 0; i < frame.local_count; ++i) {
            locals_[frame.locals_base + i] = Value::undef();
        }

        if (profile_) profile_->bump_invocation();

        InterpResult r = dispatch_loop_(frame);

        locals_top_ = frame.locals_base;
        frames_.pop_back();

        if (!r.ok()) return std::unexpected(std::move(r.message));
        return r.value;
    }

    [[nodiscard]] const FunctionProfile* profile() const noexcept { return profile_.get(); }
    void attach_profile(std::shared_ptr<FunctionProfile> p) { profile_ = std::move(p); }

private:
    std::vector<Value>      stack_;
    std::vector<Value>      locals_;
    std::vector<CallFrame>  frames_;
    uint32_t                locals_top_ = 0;

    SafepointManager*        mgr_ = nullptr;
    MutatorState             mutator_state_;
    std::shared_ptr<FunctionProfile> profile_;

    // --- Stack helpers ------------------------------------------------------
    void push_(Value v) { stack_.push_back(v); }
    Value pop_() { Value v = stack_.back(); stack_.pop_back(); return v; }
    [[nodiscard]] Value peek_(size_t i = 0) const { return stack_[stack_.size() - 1 - i]; }

    void record_feedback_(uint32_t offset, const Value& v) {
        if (profile_) profile_->at(offset).observe(v);
    }

    // --- Slow-path arithmetic (matches Arc's doArith) ----------------------
    Value do_arith_slow_(OpCode op, Value a, Value b, InterpResult& result) {
        if (a.is_number() && b.is_number()) {
            double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
            double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
            switch (op) {
                case OpCode::Add: return Value::Float(na + nb);
                case OpCode::Sub: return Value::Float(na - nb);
                case OpCode::Mul: return Value::Float(na * nb);
                case OpCode::Div:
                    if (nb == 0.0) {
                        result.error = InterpError::DivByZero;
                        result.message = "Division by zero.";
                        return Value::undef();
                    }
                    return Value::Float(na / nb);
                case OpCode::Pow: return Value::Float(std::pow(na, nb));
                case OpCode::Eq:  return Value::Int(na == nb);
                case OpCode::Ne:  return Value::Int(na != nb);
                case OpCode::Lt:  return Value::Int(na < nb);
                case OpCode::Gt:  return Value::Int(na > nb);
                case OpCode::Lte: return Value::Int(na <= nb);
                case OpCode::Gte: return Value::Int(na >= nb);
                case OpCode::And: return Value::Int(static_cast<int64_t>(na) && static_cast<int64_t>(nb));
                case OpCode::Or:  return Value::Int(static_cast<int64_t>(na) || static_cast<int64_t>(nb));
                default: break;
            }
        }
        result.error = InterpError::TypeError;
        result.message = "Incompatible types for operation.";
        return Value::undef();
    }

    // --- Dispatch loop -----------------------------------------------------
    InterpResult dispatch_loop_(CallFrame& frame) {
        InterpResult result;
        result.value = Value::undef();
        BytecodeReader& r = frame.reader;

        while (!r.at_end()) {
            // Safepoint poll — checked on every iteration (real impls only do this at loop
            // back-edges and call sites; we do it every iter for simplicity and correctness).
            mutator_state_.check_safepoint();

            const OpCode op = r.read_op();
            const uint32_t offset_before = static_cast<uint32_t>(r.ip - 1);

            switch (op) {
                case OpCode::LoadConst: {
                    uint32_t idx = r.read_const_idx();
                    Object* c = idx < frame.chunk->constants().size()
                                  ? frame.chunk->constants()[idx] : nullptr;
                    Value v;
                    if (!c)                     v = Value::null();
                    else if (c->type == ObjType::NumberInt)   v = Value::Int(reinterpret_cast<Number*>(c)->as.i);
                    else if (c->type == ObjType::NumberFloat) v = Value::Float(reinterpret_cast<Number*>(c)->as.f);
                    else if (c->type == ObjType::Null)         v = Value::null();
                    else                                       v = Value::Obj(c);
                    push_(v);
                    record_feedback_(offset_before, v);
                    break;
                }

                case OpCode::LoadLocal: {
                    uint8_t slot = r.read_byte();
                    if (slot >= frame.local_count) {
                        result.error = InterpError::NameError;
                        result.message = "Local slot out of range.";
                        return result;
                    }
                    Value v = locals_[frame.locals_base + slot];
                    if (v.is_undef()) {
                        result.error = InterpError::NameError;
                        result.message = "Variable used before assignment.";
                        return result;
                    }
                    push_(v);
                    record_feedback_(offset_before, v);
                    break;
                }

                case OpCode::StoreLocal: {
                    uint8_t slot = r.read_byte();
                    if (slot >= frame.local_count) {
                        result.error = InterpError::NameError;
                        result.message = "Local slot out of range.";
                        return result;
                    }
                    locals_[frame.locals_base + slot] = peek_();
                    break;
                }

                case OpCode::LoadVar: {
                    uint32_t idx = r.read_const_idx();
                    (void)idx;
                    result.error = InterpError::NameError;
                    result.message = "LoadVar not wired to symbol table (scaffold).";
                    return result;
                }
                case OpCode::StoreVar: {
                    uint32_t idx = r.read_const_idx();
                    (void)idx;
                    break;
                }

                case OpCode::Pop: {
                    pop_();
                    break;
                }

                // --- Arithmetic ---
                case OpCode::Add: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) {
                        Value v = Value::Int(a.as_int() + b.as_int());
                        push_(v); record_feedback_(offset_before, v);
                    } else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        Value v = Value::Float(na + nb);
                        push_(v); record_feedback_(offset_before, v);
                    } else {
                        Value v = do_arith_slow_(op, a, b, result);
                        if (!result.ok()) return result;
                        push_(v);
                    }
                    break;
                }
                case OpCode::Sub: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) {
                        push_(Value::Int(a.as_int() - b.as_int()));
                    } else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Float(na - nb));
                    } else { push_(do_arith_slow_(op, a, b, result)); if (!result.ok()) return result; }
                    break;
                }
                case OpCode::Mul: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) {
                        push_(Value::Int(a.as_int() * b.as_int()));
                    } else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Float(na * nb));
                    } else { push_(do_arith_slow_(op, a, b, result)); if (!result.ok()) return result; }
                    break;
                }
                case OpCode::Div: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) {
                        if (b.as_int() == 0) {
                            result.error = InterpError::DivByZero;
                            result.message = "Division by zero.";
                            return result;
                        }
                        // Arc promotes to float on division.
                        push_(Value::Float(static_cast<double>(a.as_int())
                                          / static_cast<double>(b.as_int())));
                    } else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        if (nb == 0.0) {
                            result.error = InterpError::DivByZero;
                            result.message = "Division by zero.";
                            return result;
                        }
                        push_(Value::Float(na / nb));
                    } else { push_(do_arith_slow_(op, a, b, result)); if (!result.ok()) return result; }
                    break;
                }
                case OpCode::Pow: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) {
                        push_(Value::Float(std::pow(static_cast<double>(a.as_int()),
                                                    static_cast<double>(b.as_int()))));
                    } else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Float(std::pow(na, nb)));
                    } else { push_(do_arith_slow_(op, a, b, result)); if (!result.ok()) return result; }
                    break;
                }
                case OpCode::Neg: {
                    Value a = pop_();
                    if (a.is_int())        push_(Value::Int(-a.as_int()));
                    else if (a.is_float()) push_(Value::Float(-a.as_float()));
                    else {
                        result.error = InterpError::TypeError;
                        result.message = "Cannot negate non-number.";
                        return result;
                    }
                    break;
                }
                case OpCode::Not: {
                    Value a = pop_();
                    push_(Value::Int(a.is_truthy() ? 0 : 1));
                    break;
                }

                case OpCode::Eq: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() == b.as_int()));
                    else if (a.is_null() || b.is_null()) push_(Value::Int(a.is_null() && b.is_null()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na == nb));
                    } else push_(Value::Int(0));
                    break;
                }
                case OpCode::Ne: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() != b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na != nb));
                    } else push_(Value::Int(1));
                    break;
                }
                case OpCode::Lt: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() < b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na < nb));
                    } else { result.error = InterpError::TypeError; return result; }
                    break;
                }
                case OpCode::Gt: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() > b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na > nb));
                    } else { result.error = InterpError::TypeError; return result; }
                    break;
                }
                case OpCode::Lte: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() <= b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na <= nb));
                    } else { result.error = InterpError::TypeError; return result; }
                    break;
                }
                case OpCode::Gte: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() >= b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na >= nb));
                    } else { result.error = InterpError::TypeError; return result; }
                    break;
                }
                case OpCode::And: {
                    Value b = pop_(); Value a = pop_();
                    push_(Value::Int(a.is_truthy() && b.is_truthy()));
                    break;
                }
                case OpCode::Or: {
                    Value b = pop_(); Value a = pop_();
                    push_(Value::Int(a.is_truthy() || b.is_truthy()));
                    break;
                }

                // --- Control flow ---
                case OpCode::Jump: {
                    int16_t off = r.read_short();
                    r.ip = static_cast<size_t>(static_cast<ptrdiff_t>(r.ip) + off);
                    break;
                }
                case OpCode::JumpIfFalse: {
                    int16_t off = r.read_short();
                    Value cond = pop_();
                    if (!cond.is_truthy()) {
                        r.ip = static_cast<size_t>(static_cast<ptrdiff_t>(r.ip) + off);
                    }
                    break;
                }
                case OpCode::ForPrep: {
                    // Scaffold: not implemented (would push length + index).
                    break;
                }
                case OpCode::ForIter: {
                    int16_t off = r.read_short();
                    (void)off;
                    break;
                }
                case OpCode::Break:    break;
                case OpCode::Continue: break;

                // --- Functions ---
                case OpCode::Call: {
                    uint8_t n = r.read_byte();
                    (void)n;
                    result.error = InterpError::UnknownOpcode;
                    result.message = "Call opcode not yet wired (scaffold).";
                    return result;
                }
                case OpCode::Return: {
                    result.value = pop_();
                    return result;
                }

                // --- Collections ---
                case OpCode::BuildList: {
                    uint8_t n = r.read_byte();
                    (void)n;
                    // Scaffold: no list allocation in the no-heap interpreter.
                    push_(Value::null());
                    break;
                }
                case OpCode::IndexGet: {
                    Value idx = pop_(); Value tgt = pop_();
                    (void)tgt; (void)idx;
                    push_(Value::null());
                    break;
                }
                case OpCode::IndexSet: {
                    Value v = pop_(); Value i = pop_(); Value t = pop_();
                    (void)v; (void)i; (void)t;
                    push_(Value::Int(1));
                    break;
                }

                // --- OOP ---
                case OpCode::PropertyAccess: {
                    uint32_t idx = r.read_const_idx();
                    (void)idx;
                    Value tgt = pop_();
                    (void)tgt;
                    push_(Value::null());
                    break;
                }
                case OpCode::PropertySet: {
                    uint32_t idx = r.read_const_idx();
                    (void)idx;
                    Value v = pop_(); Value t = pop_();
                    (void)t;
                    push_(v);
                    break;
                }

                // --- Exceptions ---
                case OpCode::TryPush: {
                    int16_t off = r.read_short();
                    (void)off;
                    break;
                }
                case OpCode::TryPop: break;

                case OpCode::Import: {
                    uint32_t idx = r.read_const_idx();
                    (void)idx;
                    break;
                }

                case OpCode::Halt: {
                    if (!stack_.empty()) result.value = stack_.back();
                    return result;
                }

                default: {
                    result.error = InterpError::UnknownOpcode;
                    result.message = "Unknown opcode";
                    return result;
                }
            }
        }

        // Fell off the end without HALT/RETURN — return top of stack.
        if (!stack_.empty()) result.value = stack_.back();
        return result;
    }
};

}  // namespace arcjit
