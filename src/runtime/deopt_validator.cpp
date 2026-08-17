// SPDX-License-Identifier: MIT
#include "runtime/deopt_validator.h"

#include <chrono>
#include <print>

#include "interp/interpreter.h"

namespace arcjit {

DeoptLog& global_deopt_log() {
    static thread_local DeoptLog log;
    return log;
}

[[nodiscard]] std::expected<Value, std::string>
validate_deopt(const DeoptFrameState& state, std::optional<Value> expected) {
    if (!state.chunk) {
        return std::unexpected("validate_deopt: chunk is null");
    }

    // Reconstruct an interpreter and run from the deopt point.
    //
    // For this scaffold, we run the ENTIRE chunk from the start (not from
    // the deopt IP) because the interpreter doesn't yet support resuming
    // from an arbitrary IP with a pre-populated locals/stack state.
    // A full implementation would set up the CallFrame with the captured
    // state and dispatch from `bytecode_ip`.
    Interpreter interp;
    auto result = interp.run(*state.chunk);

    if (!result) {
        return std::unexpected("validate_deopt: interpreter failed: " + result.error());
    }

    // Log the deopt.
    DeoptLogEntry entry;
    entry.reason       = state.reason;
    entry.chunk_name   = state.chunk ? state.chunk->filename() : std::string{};
    entry.bytecode_ip  = state.bytecode_ip;
    entry.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count());
    entry.interpreter_result = *result;
    entry.jit_expected       = expected;
    entry.matched            = expected.has_value() && (*expected == *result);

    global_deopt_log().record(std::move(entry));

    return result;
}

void DeoptLog::dump(std::string_view context) const {
    if (context.empty()) {
        std::println(stderr, "=== Deopt log ({} entries) ===", entries_.size());
    } else {
        std::println(stderr, "=== Deopt log ({}, {} entries) ===", context, entries_.size());
    }

    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        std::println(stderr, "  [{}] reason='{}' chunk='{}' ip={} matched={}",
                     i, e.reason, e.chunk_name, e.bytecode_ip, e.matched);
    }
}

}  // namespace arcjit
