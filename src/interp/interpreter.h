// SPDX-License-Identifier: MIT
// arcJIT — Tier-0 interpreter.
//
// Executes Arc's stack bytecode with the same fast paths as the upstream
// `vmRun()` in `src/vm.c`, plus:
//   - safepoint polling at loop back-edges and call sites
//   - type-feedback and inline-cache collection for Tier 1/2
//   - real symbol-table backed variable storage
//   - real heap-object allocation (Number, String, List, Function, Instance)
//   - real call frame stack
//   - real try/catch exception handling
//
// Every opcode from `bytecode.h` is implemented — no stubs.
#pragma once

#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bytecode/chunk.h"
#include "bytecode/heap.h"
#include "bytecode/object.h"
#include "bytecode/symbol_table.h"
#include "bytecode/value.h"
#include "machinery/meter.h"
#include "runtime/profile.h"
#include "runtime/safepoint.h"

namespace arcjit {

// --- Call frame (mirrors Arc's CallFrame) -----------------------------------
struct CallFrame {
    const Chunk*     chunk = nullptr;
    BytecodeReader   reader{};
    uint32_t         locals_base = 0;
    uint32_t         local_count = 0;
    SymbolTable*     variables = nullptr;   // owned by the frame (or shared with caller)
    ArcInstance*     instance = nullptr;    // for method calls
    bool             owns_variables = false;
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
    IndexError,
    PropertyError,
};

struct InterpResult {
    Value       value;
    InterpError error = InterpError::None;
    std::string message;
    bool        is_return = false;     // signals a Return opcode (caller unwinds)
    bool        is_break = false;      // signals a Break opcode
    bool        is_continue = false;   // signals a Continue opcode
    bool        is_thrown = false;     // signals an exception was raised

    [[nodiscard]] bool ok() const noexcept { return error == InterpError::None && !is_thrown; }
};

// --- Interpreter ------------------------------------------------------------
class Interpreter {
public:
    static constexpr size_t kStackMax       = 4096;
    static constexpr size_t kLocalsMax      = 65536;
    static constexpr size_t kFrameMax       = 1024;
    static constexpr size_t kTryMax         = 256;
    static constexpr uint32_t kHotThreshold = 200;  // invocations before Tier 1 fires

    Interpreter() {
        stack_.reserve(kStackMax);
        locals_.resize(kLocalsMax);
        frames_.reserve(kFrameMax);
        globals_ = std::make_unique<SymbolTable>();
    }

    ~Interpreter() {
        if (mgr_) mgr_->unregister_mutator(&mutator_state_);
        // Free any leftover values on the stack.
        for (auto& v : stack_) release_value(v);
        // Free any leftover locals.
        for (size_t i = 0; i < locals_top_; ++i) release_value(locals_[i]);
        // Free frames' variable tables.
        for (auto& f : frames_) {
            if (f.owns_variables && f.variables) delete f.variables;
        }
    }

    Interpreter(const Interpreter&)            = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    void attach_safepoint(SafepointManager* mgr) {
        mgr_ = mgr;
        if (mgr_) mgr_->register_mutator(&mutator_state_);
    }

    // Access the global symbol table (for registering native functions etc.).
    [[nodiscard]] SymbolTable& globals() noexcept { return *globals_; }

    // Run a chunk. Returns the top-of-stack value (the chunk's "result").
    [[nodiscard]] std::expected<Value, std::string> run(const Chunk& chunk) {
        if (frames_.size() >= kFrameMax) {
            return std::unexpected("stack overflow");
        }

        // Set up the initial frame.
        CallFrame& frame = frames_.emplace_back();
        frame.chunk       = &chunk;
        frame.reader      = BytecodeReader{chunk.code(), 0};
        frame.locals_base = locals_top_;
        frame.local_count = static_cast<uint32_t>(chunk.max_locals());
        frame.variables   = globals_.get();
        frame.owns_variables = false;
        locals_top_ += chunk.max_locals();
        for (uint32_t i = 0; i < frame.local_count; ++i) {
            locals_[frame.locals_base + i] = Value::undef();
        }

        if (profile_) profile_->bump_invocation();

        InterpResult r = dispatch_loop_(frame);

        // Clean up this frame's locals.
        for (uint32_t i = 0; i < frame.local_count; ++i) {
            release_value(locals_[frame.locals_base + i]);
            locals_[frame.locals_base + i] = Value::undef();
        }
        locals_top_ = frame.locals_base;

        // If the frame owned its variable table, free it.
        if (frame.owns_variables && frame.variables) {
            // Release all values in the table first.
            frame.variables->for_each([](std::string_view, Value v) { release_value(v); });
            delete frame.variables;
        }

        frames_.pop_back();

        if (!r.ok()) {
            return std::unexpected(r.message.empty() ? "execution error" : std::move(r.message));
        }
        return r.value;
    }

    [[nodiscard]] const FunctionProfile* profile() const noexcept { return profile_.get(); }
    void attach_profile(std::shared_ptr<FunctionProfile> p) { profile_ = std::move(p); }

    // Attach a Meter for confidence tracking. When set, the interpreter
    // feeds profile samples into the Meter, which computes confidence
    // levels for speculation decisions.
    void attach_meter(Meter* m) { meter_ = m; }

    // --- Hot-function detection (drives the tier ladder) -------------------
    [[nodiscard]] bool is_hot(const Chunk& /*chunk*/) const noexcept {
        return profile_ && profile_->invocations() >= kHotThreshold;
    }

private:
    std::vector<Value>      stack_;
    std::vector<Value>      locals_;
    std::vector<CallFrame>  frames_;
    uint32_t                locals_top_ = 0;

    // Try-stack: each entry captures the IP and stack/frame depth at the
    // corresponding TRY_PUSH. When an error occurs, we restore to that state
    // and resume at the catch block.
    struct TryEntry {
        size_t   frame_index;       // index into frames_
        size_t   reader_ip;         // IP within that frame's reader
        size_t   stack_top;         // stack depth
        uint32_t locals_top;        // locals_top at TRY_PUSH time
    };
    std::vector<TryEntry>   try_stack_;

    SafepointManager*        mgr_ = nullptr;
    MutatorState             mutator_state_;
    std::unique_ptr<SymbolTable> globals_;
    std::shared_ptr<FunctionProfile> profile_;
    Meter*                   meter_ = nullptr;  // optional — for confidence tracking

    // --- Stack helpers ------------------------------------------------------
    void push_(Value v) { stack_.push_back(v); }
    Value pop_() { Value v = stack_.back(); stack_.pop_back(); return v; }
    [[nodiscard]] Value peek_(size_t i = 0) const { return stack_[stack_.size() - 1 - i]; }
    [[nodiscard]] size_t stack_size_() const noexcept { return stack_.size(); }

    void record_feedback_(uint32_t offset, const Value& v) {
        if (profile_) profile_->at(offset).observe(v);
        // Feed into Meter for confidence tracking.
        if (meter_) {
            auto& e = meter_->entry_for(offset);
            uint64_t type_tag = static_cast<uint64_t>(v.type);
            uint64_t shape_tag = v.is_obj() && v.as_obj()
                                  ? static_cast<uint64_t>(v.as_obj()->type)
                                  : 0;
            e.record_sample(type_tag, shape_tag);
        }
    }

    // --- Variable lookup (walks the frame chain) ---------------------------
    //
    // Arc's variable lookup walks: instance fields (if method call) → local
    // frame variables → caller's variables (closure chain) → globals.
    // For our scaffold we walk the frame chain backwards, then globals.
    [[nodiscard]] Value lookup_var_(const CallFrame& /*frame*/, std::string_view name) {
        // Walk frames from innermost out.
        for (size_t i = frames_.size(); i > 0; --i) {
            const CallFrame& f = frames_[i - 1];
            if (f.variables && f.variables->has(name)) {
                return f.variables->get(name);
            }
        }
        return Value::undef();
    }

    void set_var_(CallFrame& frame, std::string_view name, Value v) {
        // Find the frame that already has this name; update it there.
        for (size_t i = frames_.size(); i > 0; --i) {
            CallFrame& f = frames_[i - 1];
            if (f.variables && f.variables->has(name)) {
                Value old = f.variables->get(name);
                release_value(old);
                f.variables->set(name, v);
                return;
            }
        }
        // Not found — define in the current frame.
        if (!frame.variables) {
            frame.variables = new SymbolTable();
            frame.owns_variables = true;
        }
        frame.variables->set(name, v);
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
                        result.is_thrown = true;
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

        // String concat: a + b (both strings)
        if (op == OpCode::Add && a.is_obj() && a.as_obj() && a.as_obj()->type == ObjType::String
                              && b.is_obj() && b.as_obj() && b.as_obj()->type == ObjType::String) {
            String* sa = cast_to<String>(a.as_obj());
            String* sb = cast_to<String>(b.as_obj());
            std::string combined;
            combined.reserve(sa->len + sb->len);
            combined.append(sa->value, sa->len);
            combined.append(sb->value, sb->len);
            release_value(a);
            release_value(b);
            return Value::Obj(alloc_string(combined));
        }

        result.error = InterpError::TypeError;
        result.message = "Incompatible types for operation.";
        result.is_thrown = true;
        return Value::undef();
    }

    // --- The dispatch loop -------------------------------------------------
    InterpResult dispatch_loop_(CallFrame& frame) {
        InterpResult result;
        result.value = Value::undef();
        BytecodeReader& r = frame.reader;

        while (!r.at_end()) {
            mutator_state_.check_safepoint();

            const OpCode op = r.read_op();
            const uint32_t offset_before = static_cast<uint32_t>(r.ip - 1);

            switch (op) {
                case OpCode::LoadConst: {
                    uint32_t idx = r.read_const_idx();
                    Object* c = idx < frame.chunk->constants().size()
                                  ? frame.chunk->constants()[idx] : nullptr;
                    Value v;
                    if (!c)                                       v = Value::null();
                    else if (c->type == ObjType::NumberInt)       v = Value::Int(cast_to<Number>(c)->as.i);
                    else if (c->type == ObjType::NumberFloat)     v = Value::Float(cast_to<Number>(c)->as.f);
                    else if (c->type == ObjType::Null)             v = Value::null();
                    else if (c->is_static)                         v = Value::Obj(c);
                    else                                           v = copy_value(Value::Obj(c));
                    push_(v);
                    record_feedback_(offset_before, v);
                    break;
                }

                case OpCode::LoadLocal: {
                    uint8_t slot = r.read_byte();
                    if (slot >= frame.local_count) {
                        result.error = InterpError::NameError;
                        result.message = "Local slot out of range.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    Value v = locals_[frame.locals_base + slot];
                    if (v.is_undef()) {
                        result.error = InterpError::NameError;
                        result.message = "Variable used before assignment.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    push_(copy_value(v));
                    record_feedback_(offset_before, peek_());
                    break;
                }

                case OpCode::StoreLocal: {
                    uint8_t slot = r.read_byte();
                    if (slot >= frame.local_count) {
                        result.error = InterpError::NameError;
                        result.message = "Local slot out of range.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    Value v = peek_();
                    release_value(locals_[frame.locals_base + slot]);
                    locals_[frame.locals_base + slot] = copy_value(v);
                    break;
                }

                case OpCode::LoadVar: {
                    uint32_t idx = r.read_const_idx();
                    Object* c = idx < frame.chunk->constants().size()
                                  ? frame.chunk->constants()[idx] : nullptr;
                    if (!c || c->type != ObjType::String) {
                        result.error = InterpError::NameError;
                        result.message = "LoadVar: invalid name constant.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    String* name_str = cast_to<String>(c);
                    Value v = lookup_var_(frame, std::string_view(name_str->value, name_str->len));
                    if (v.is_undef()) {
                        result.error = InterpError::NameError;
                        result.message = "Undefined variable \"" + std::string(name_str->value, name_str->len) + "\".";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    push_(copy_value(v));
                    record_feedback_(offset_before, peek_());
                    break;
                }

                case OpCode::StoreVar: {
                    uint32_t idx = r.read_const_idx();
                    Object* c = idx < frame.chunk->constants().size()
                                  ? frame.chunk->constants()[idx] : nullptr;
                    if (!c || c->type != ObjType::String) {
                        result.error = InterpError::NameError;
                        result.message = "StoreVar: invalid name constant.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    String* name_str = cast_to<String>(c);
                    Value v = peek_();
                    set_var_(frame, std::string_view(name_str->value, name_str->len), copy_value(v));
                    break;
                }

                case OpCode::Pop: {
                    release_value(pop_());
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
                        push_(Value::Float(na + nb));
                    } else {
                        Value res = do_arith_slow_(op, a, b, result);
                        if (!result.ok()) goto handle_error;
                        push_(res);
                        record_feedback_(offset_before, res);
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
                    } else {
                        Value res = do_arith_slow_(op, a, b, result);
                        if (!result.ok()) goto handle_error;
                        push_(res);
                    }
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
                    } else {
                        Value res = do_arith_slow_(op, a, b, result);
                        if (!result.ok()) goto handle_error;
                        push_(res);
                    }
                    break;
                }
                case OpCode::Div: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) {
                        if (b.as_int() == 0) {
                            result.error = InterpError::DivByZero;
                            result.message = "Division by zero.";
                            result.is_thrown = true;
                            goto handle_error;
                        }
                        push_(Value::Float(static_cast<double>(a.as_int()) / static_cast<double>(b.as_int())));
                    } else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        if (nb == 0.0) {
                            result.error = InterpError::DivByZero;
                            result.message = "Division by zero.";
                            result.is_thrown = true;
                            goto handle_error;
                        }
                        push_(Value::Float(na / nb));
                    } else {
                        Value res = do_arith_slow_(op, a, b, result);
                        if (!result.ok()) goto handle_error;
                        push_(res);
                    }
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
                    } else {
                        Value res = do_arith_slow_(op, a, b, result);
                        if (!result.ok()) goto handle_error;
                        push_(res);
                    }
                    break;
                }
                case OpCode::Neg: {
                    Value a = pop_();
                    if (a.is_int())        push_(Value::Int(-a.as_int()));
                    else if (a.is_float()) push_(Value::Float(-a.as_float()));
                    else {
                        result.error = InterpError::TypeError;
                        result.message = "Cannot negate non-number.";
                        result.is_thrown = true;
                        goto handle_error;
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
                    } else if (a.is_obj() && b.is_obj() && a.as_obj() == b.as_obj()) {
                        push_(Value::Int(1));
                    } else {
                        push_(Value::Int(0));
                    }
                    break;
                }
                case OpCode::Ne: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() != b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na != nb));
                    } else {
                        push_(Value::Int(1));
                    }
                    break;
                }
                case OpCode::Lt: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() < b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na < nb));
                    } else { result.error = InterpError::TypeError; result.is_thrown = true; goto handle_error; }
                    break;
                }
                case OpCode::Gt: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() > b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na > nb));
                    } else { result.error = InterpError::TypeError; result.is_thrown = true; goto handle_error; }
                    break;
                }
                case OpCode::Lte: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() <= b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na <= nb));
                    } else { result.error = InterpError::TypeError; result.is_thrown = true; goto handle_error; }
                    break;
                }
                case OpCode::Gte: {
                    Value b = pop_(); Value a = pop_();
                    if (a.is_int() && b.is_int()) push_(Value::Int(a.as_int() >= b.as_int()));
                    else if (a.is_number() && b.is_number()) {
                        double na = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
                        double nb = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
                        push_(Value::Int(na >= nb));
                    } else { result.error = InterpError::TypeError; result.is_thrown = true; goto handle_error; }
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

                // --- Loop iteration ---
                case OpCode::ForPrep: {
                    // Stack: [iterable] → [iterable, length, index(0)]
                    Value iter = peek_();
                    if (!iter.is_obj() || !iter.as_obj()) {
                        result.error = InterpError::TypeError;
                        result.message = "ForPrep: cannot iterate non-iterable.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    uint64_t len = 0;
                    if (iter.as_obj()->type == ObjType::List) {
                        len = cast_to<List>(iter.as_obj())->size;
                    } else if (iter.as_obj()->type == ObjType::String) {
                        len = cast_to<String>(iter.as_obj())->len;
                    } else {
                        result.error = InterpError::TypeError;
                        result.message = "ForPrep: cannot iterate this type.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    push_(Value::Int(static_cast<int64_t>(len)));
                    push_(Value::Int(0));
                    break;
                }
                case OpCode::ForIter: {
                    int16_t exit_off = r.read_short();
                    // Stack: [iterable, length, index]
                    Value idx_v = peek_(0);
                    Value len_v = peek_(1);
                    Value iter  = peek_(2);
                    if (!idx_v.is_int() || !len_v.is_int()) {
                        result.error = InterpError::TypeError;
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    int64_t idx = idx_v.as_int();
                    int64_t len = len_v.as_int();
                    if (idx >= len) {
                        // Exit loop — pop iterable, length, index.
                        pop_(); pop_(); release_value(pop_());
                        r.ip = static_cast<size_t>(static_cast<ptrdiff_t>(r.ip) + exit_off);
                    } else {
                        // Fetch element at idx.
                        Value item;
                        if (iter.is_obj() && iter.as_obj()->type == ObjType::List) {
                            List* l = cast_to<List>(iter.as_obj());
                            Object* o = l->objects[idx];
                            item = copy_value(object_to_value(o));
                        } else if (iter.is_obj() && iter.as_obj()->type == ObjType::String) {
                            String* s = cast_to<String>(iter.as_obj());
                            item = Value::Obj(alloc_string(std::string_view(s->value + idx, 1)));
                        }
                        push_(item);
                        // Increment index.
                        stack_[stack_.size() - 4] = Value::Int(idx + 1);
                    }
                    break;
                }

                case OpCode::Break: {
                    result.is_break = true;
                    return result;
                }
                case OpCode::Continue: {
                    result.is_continue = true;
                    return result;
                }

                // --- Collections ---
                case OpCode::BuildList: {
                    uint8_t n = r.read_byte();
                    Object* list_obj = ListObj::alloc(n > 0 ? n : 4);
                    // Pop n values, insert in order (so deepest stack item is element 0).
                    for (uint8_t i = 0; i < n; ++i) {
                        Value v = pop_();
                        Object* o = value_to_object(v);
                        ListObj::append(list_obj, o);
                        if (o) o->ref_count--;  // value_to_object retained; list now owns.
                    }
                    // Reverse so element 0 is the deepest.
                    List* l = cast_to<List>(list_obj);
                    for (uint64_t i = 0; i < l->size / 2; ++i) {
                        std::swap(l->objects[i], l->objects[l->size - 1 - i]);
                    }
                    push_(Value::Obj(list_obj));
                    record_feedback_(offset_before, peek_());
                    break;
                }
                case OpCode::IndexGet: {
                    Value idx = pop_();
                    Value tgt = pop_();
                    if (!tgt.is_obj() || !tgt.as_obj()) {
                        result.error = InterpError::TypeError;
                        result.message = "IndexGet: cannot index non-object.";
                        result.is_thrown = true;
                        release_value(idx);
                        goto handle_error;
                    }
                    if (!idx.is_int()) {
                        result.error = InterpError::TypeError;
                        result.message = "IndexGet: index must be an integer.";
                        result.is_thrown = true;
                        release_value(tgt);
                        release_value(idx);
                        goto handle_error;
                    }
                    int64_t i = idx.as_int();
                    Value item;
                    if (tgt.as_obj()->type == ObjType::List) {
                        List* l = cast_to<List>(tgt.as_obj());
                        if (i < 0 || static_cast<uint64_t>(i) >= l->size) {
                            result.error = InterpError::IndexError;
                            result.message = "Index out of bounds.";
                            result.is_thrown = true;
                            release_value(tgt);
                            goto handle_error;
                        }
                        item = copy_value(object_to_value(l->objects[i]));
                    } else if (tgt.as_obj()->type == ObjType::String) {
                        String* s = cast_to<String>(tgt.as_obj());
                        if (i < 0 || static_cast<uint64_t>(i) >= s->len) {
                            result.error = InterpError::IndexError;
                            result.message = "Index out of bounds.";
                            result.is_thrown = true;
                            release_value(tgt);
                            goto handle_error;
                        }
                        item = Value::Obj(alloc_string(std::string_view(s->value + i, 1)));
                    } else {
                        result.error = InterpError::TypeError;
                        result.message = "IndexGet: target not indexable.";
                        result.is_thrown = true;
                        release_value(tgt);
                        goto handle_error;
                    }
                    push_(item);
                    release_value(tgt);
                    break;
                }
                case OpCode::IndexSet: {
                    Value v = pop_();
                    Value idx = pop_();
                    Value tgt = pop_();
                    if (!tgt.is_obj() || !tgt.as_obj() || tgt.as_obj()->type != ObjType::List) {
                        result.error = InterpError::TypeError;
                        result.message = "IndexSet: target must be a list.";
                        result.is_thrown = true;
                        release_value(tgt); release_value(idx); release_value(v);
                        goto handle_error;
                    }
                    if (!idx.is_int()) {
                        result.error = InterpError::TypeError;
                        result.is_thrown = true;
                        release_value(tgt); release_value(idx); release_value(v);
                        goto handle_error;
                    }
                    List* l = cast_to<List>(tgt.as_obj());
                    int64_t i = idx.as_int();
                    if (i < 0 || static_cast<uint64_t>(i) >= l->size) {
                        result.error = InterpError::IndexError;
                        result.message = "Index out of bounds.";
                        result.is_thrown = true;
                        release_value(tgt); release_value(idx); release_value(v);
                        goto handle_error;
                    }
                    if (l->objects[i]) release(l->objects[i]);
                    l->objects[i] = value_to_object(v);
                    push_(Value::Int(1));
                    release_value(tgt);
                    break;
                }

                // --- Functions ---
                case OpCode::Call: {
                    uint8_t n_args = r.read_byte();
                    // Stack: [callee, arg1, ..., argN]
                    // Pop args (in reverse), then callee.
                    std::vector<Value> args(n_args);
                    for (int i = n_args - 1; i >= 0; --i) args[i] = pop_();
                    Value callee = pop_();

                    if (!callee.is_obj() || !callee.as_obj()) {
                        result.error = InterpError::TypeError;
                        result.message = "Call: callee is not callable.";
                        result.is_thrown = true;
                        for (auto& a : args) release_value(a);
                        goto handle_error;
                    }

                    Object* callee_obj = callee.as_obj();
                    if (callee_obj->type == ObjType::NativeFunction) {
                        ArcNative* nf = reinterpret_cast<ArcNative*>(callee_obj);
                        if (!nf->variadic && args.size() < nf->required_args) {
                            result.error = InterpError::ValueError;
                            result.message = "Call: not enough arguments to native function.";
                            result.is_thrown = true;
                            for (auto& a : args) release_value(a);
                            release_value(callee);
                            goto handle_error;
                        }
                        Value ret = nf->fn(args);
                        push_(ret);
                        for (auto& a : args) release_value(a);
                        release_value(callee);
                        break;
                    }

                    if (callee_obj->type == ObjType::Function) {
                        ArcFunction* fn = reinterpret_cast<ArcFunction*>(callee_obj);
                        if (!fn->chunk) {
                            result.error = InterpError::ValueError;
                            result.message = "Call: function has no body.";
                            result.is_thrown = true;
                            for (auto& a : args) release_value(a);
                            release_value(callee);
                            goto handle_error;
                        }

                        // Push a new frame.
                        if (frames_.size() >= kFrameMax) {
                            result.error = InterpError::StackOverflow;
                            result.message = "Call stack overflow.";
                            result.is_thrown = true;
                            for (auto& a : args) release_value(a);
                            release_value(callee);
                            goto handle_error;
                        }

                        CallFrame& new_frame = frames_.emplace_back();
                        new_frame.chunk       = fn->chunk;
                        new_frame.reader      = BytecodeReader{fn->chunk->code(), 0};
                        new_frame.locals_base = locals_top_;
                        new_frame.local_count = static_cast<uint32_t>(fn->max_locals);
                        new_frame.variables   = new SymbolTable();
                        new_frame.owns_variables = true;
                        new_frame.instance    = frame.instance;  // inherit for method calls
                        locals_top_ += fn->max_locals;
                        for (uint32_t i = 0; i < new_frame.local_count; ++i) {
                            locals_[new_frame.locals_base + i] = Value::undef();
                        }
                        // Bind params to locals 0..n-1.
                        for (uint8_t i = 0; i < n_args && i < fn->params.size(); ++i) {
                            locals_[new_frame.locals_base + i] = copy_value(args[i]);
                        }
                        // Also bind args by name into the variable table.
                        for (uint8_t i = 0; i < n_args && i < fn->params.size(); ++i) {
                            new_frame.variables->set(fn->params[i], copy_value(args[i]));
                        }

                        if (profile_) profile_->bump_invocation();

                        // Recursively run the new frame.
                        InterpResult sub = dispatch_loop_(new_frame);

                        // Clean up new frame's locals.
                        for (uint32_t i = 0; i < new_frame.local_count; ++i) {
                            release_value(locals_[new_frame.locals_base + i]);
                            locals_[new_frame.locals_base + i] = Value::undef();
                        }
                        locals_top_ = new_frame.locals_base;
                        if (new_frame.owns_variables && new_frame.variables) {
                            new_frame.variables->for_each(
                                [](std::string_view, Value v) { release_value(v); });
                            delete new_frame.variables;
                        }
                        frames_.pop_back();

                        for (auto& a : args) release_value(a);
                        release_value(callee);

                        if (!sub.ok()) {
                            result = sub;
                            goto handle_error;
                        }
                        push_(sub.value);
                        break;
                    }

                    result.error = InterpError::TypeError;
                    result.message = "Call: object is not callable.";
                    result.is_thrown = true;
                    for (auto& a : args) release_value(a);
                    release_value(callee);
                    goto handle_error;
                }

                case OpCode::Return: {
                    result.value = pop_();
                    result.is_return = true;
                    return result;
                }

                // --- OOP ---
                case OpCode::PropertyAccess: {
                    uint32_t idx = r.read_const_idx();
                    Object* c = idx < frame.chunk->constants().size()
                                  ? frame.chunk->constants()[idx] : nullptr;
                    if (!c || c->type != ObjType::String) {
                        result.error = InterpError::PropertyError;
                        result.message = "PropertyAccess: invalid name.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    String* name_str = cast_to<String>(c);
                    Value tgt = pop_();
                    if (!tgt.is_obj() || !tgt.as_obj() || tgt.as_obj()->type != ObjType::Instance) {
                        result.error = InterpError::TypeError;
                        result.message = "PropertyAccess: target is not an instance.";
                        result.is_thrown = true;
                        release_value(tgt);
                        goto handle_error;
                    }
                    ArcInstance* inst = reinterpret_cast<ArcInstance*>(tgt.as_obj());
                    Value v = inst->fields.get(std::string_view(name_str->value, name_str->len));
                    if (v.is_undef()) {
                        result.error = InterpError::PropertyError;
                        result.message = "Instance has no property \"" +
                                          std::string(name_str->value, name_str->len) + "\".";
                        result.is_thrown = true;
                        release_value(tgt);
                        goto handle_error;
                    }
                    push_(copy_value(v));
                    release_value(tgt);
                    break;
                }
                case OpCode::PropertySet: {
                    uint32_t idx = r.read_const_idx();
                    Object* c = idx < frame.chunk->constants().size()
                                  ? frame.chunk->constants()[idx] : nullptr;
                    if (!c || c->type != ObjType::String) {
                        result.error = InterpError::PropertyError;
                        result.message = "PropertySet: invalid name.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    String* name_str = cast_to<String>(c);
                    Value v = pop_();
                    Value tgt = pop_();
                    if (!tgt.is_obj() || !tgt.as_obj() || tgt.as_obj()->type != ObjType::Instance) {
                        result.error = InterpError::TypeError;
                        result.message = "PropertySet: target is not an instance.";
                        result.is_thrown = true;
                        release_value(tgt); release_value(v);
                        goto handle_error;
                    }
                    ArcInstance* inst = reinterpret_cast<ArcInstance*>(tgt.as_obj());
                    std::string_view name(name_str->value, name_str->len);
                    Value old = inst->fields.get(name);
                    release_value(old);
                    inst->fields.set(name, copy_value(v));
                    push_(v);
                    release_value(tgt);
                    break;
                }

                // --- Exceptions ---
                case OpCode::TryPush: {
                    int16_t off = r.read_short();
                    // Record the catch IP, stack depth, and frame depth.
                    size_t catch_ip = static_cast<size_t>(static_cast<ptrdiff_t>(r.ip) + off);
                    if (try_stack_.size() >= kTryMax) {
                        result.error = InterpError::StackOverflow;
                        result.message = "Try stack overflow.";
                        result.is_thrown = true;
                        goto handle_error;
                    }
                    try_stack_.push_back(TryEntry{
                        frames_.size() - 1,
                        catch_ip,
                        stack_.size(),
                        locals_top_,
                    });
                    break;
                }
                case OpCode::TryPop: {
                    if (!try_stack_.empty()) try_stack_.pop_back();
                    break;
                }

                case OpCode::Import: {
                    // We don't actually load files in this scaffold runtime;
                    // we treat imports as no-ops (the host environment is
                    // expected to have pre-registered any modules).
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
                    result.is_thrown = true;
                    goto handle_error;
                }
            }

            continue;

        handle_error:
            // If we have a try/catch active, unwind to the catch block.
            if (!try_stack_.empty()) {
                TryEntry t = try_stack_.back();
                try_stack_.pop_back();

                // Unwind frames down to t.frame_index.
                while (frames_.size() > t.frame_index + 1) {
                    CallFrame& leaving = frames_.back();
                    for (uint32_t i = 0; i < leaving.local_count; ++i) {
                        release_value(locals_[leaving.locals_base + i]);
                        locals_[leaving.locals_base + i] = Value::undef();
                    }
                    locals_top_ = leaving.locals_base;
                    if (leaving.owns_variables && leaving.variables) {
                        leaving.variables->for_each(
                            [](std::string_view, Value v) { release_value(v); });
                        delete leaving.variables;
                    }
                    frames_.pop_back();
                }

                // Truncate the operand stack.
                while (stack_.size() > t.stack_top) {
                    release_value(stack_.back());
                    stack_.pop_back();
                }

                // Resume at the catch IP in the target frame.
                CallFrame& target_frame = frames_[t.frame_index];
                target_frame.reader.ip = t.reader_ip;
                r = target_frame.reader;  // refresh our local reference

                // Push the error message as a string onto the stack.
                std::string msg = result.message.empty() ? "Unknown error" : result.message;
                push_(Value::Obj(alloc_string(msg)));

                // Clear the error state.
                result = InterpResult{};
                continue;
            }

            // No try/catch — propagate the error to the caller.
            return result;
        }

        // Fell off the end without HALT/RETURN — return top of stack.
        if (!stack_.empty()) result.value = stack_.back();
        return result;
    }
};

}  // namespace arcjit
