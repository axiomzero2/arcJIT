// SPDX-License-Identifier: MIT
// arcJIT — Deopt validator (Rule 39).
//
// After every deopt in debug builds, reconstruct the interpreter state from
// the frame state, run N steps in the interpreter, and compare against the
// expected output. This catches silent miscompilations that produce wrong
// results without crashing.
//
// The validator is triggered automatically when a deopt occurs. It:
//   1. Extracts the frame state (locals, operand stack, IP) from the deopt
//      metadata.
//   2. Reconstructs an interpreter CallFrame from that state.
//   3. Runs the remaining bytecode in the interpreter.
//   4. Compares the result against the expected value (if known).
//
// If the values differ, the validator logs a mismatch and (in debug builds)
// aborts.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bytecode/chunk.h"
#include "bytecode/value.h"

namespace arcjit {

// A captured frame state at the point of deopt.
struct DeoptFrameState {
    const Chunk*    chunk       = nullptr;
    uint32_t        bytecode_ip = 0;
    std::vector<Value> locals;
    std::vector<Value> operand_stack;
    std::string     reason;
};

// Validate a deopt: reconstruct the interpreter state and run from the
// deopt point. Returns the value the interpreter produces.
//
// `expected` is the value the JIT-compiled code would have produced if the
// speculation had succeeded. If the interpreter produces a different value,
// that's expected (the speculation failed) — but the difference is logged
// for analysis.
[[nodiscard]] std::expected<Value, std::string>
validate_deopt(const DeoptFrameState& state, std::optional<Value> expected = std::nullopt);

// Deopt log entry — one per deopt event.
struct DeoptLogEntry {
    std::string reason;
    std::string chunk_name;
    uint32_t    bytecode_ip;
    uint64_t    timestamp_ns;
    Value       interpreter_result;
    std::optional<Value> jit_expected;
    bool        matched;  // true if interpreter_result == jit_expected
};

// Global deopt log (thread-local). Cleared per compilation.
class DeoptLog {
public:
    void record(DeoptLogEntry entry) {
        entries_.push_back(std::move(entry));
    }

    [[nodiscard]] const std::vector<DeoptLogEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] size_t count() const noexcept { return entries_.size(); }
    [[nodiscard]] size_t mismatch_count() const noexcept {
        size_t n = 0;
        for (const auto& e : entries_) if (!e.matched) ++n;
        return n;
    }

    void clear() noexcept { entries_.clear(); }

    // Dump the log to stderr for debugging.
    void dump(std::string_view context = "") const;

private:
    std::vector<DeoptLogEntry> entries_;
};

// Thread-local global deopt log.
DeoptLog& global_deopt_log();

}  // namespace arcjit
