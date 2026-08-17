// SPDX-License-Identifier: MIT
// arcJIT — Runtime orchestrator.
//
// Ties the three tiers together. Decides when to compile a chunk up the
// tier ladder, manages the compiler thread pool (enkiTS), and exposes the
// `run(chunk)` API used by the CLI.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "bytecode/chunk.h"
#include "bytecode/value.h"
#include "interp/interpreter.h"
#include "runtime/safepoint.h"
#include "tier1/tier1.h"
#include "tier2/tier2.h"

#include <TaskScheduler.h>

namespace arcjit {

// The tier ladder.
enum class Tier : uint8_t {
    Interpreter = 0,
    Tier1Baseline,
    Tier2Optimizing,
};

struct CompilationStats {
    uint64_t interp_invocations = 0;
    uint64_t tier1_compiles      = 0;
    uint64_t tier2_compiles      = 0;
    uint64_t deopts              = 0;
    uint64_t total_compilation_time_us = 0;
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    // Run a chunk through the tier ladder.
    [[nodiscard]] std::expected<Value, std::string> run(const Chunk& chunk);

    // Force-compile a chunk up to a given tier (for benchmarks / testing).
    [[nodiscard]] std::expected<Value, std::string> run_at_tier(const Chunk& chunk, Tier t);

    // Run the demo Tier-1 function (compute 1+2+3) and return the result.
    [[nodiscard]] std::expected<int64_t, std::string> run_tier1_demo();

    // Run the demo Tier-2 SoN pipeline on a synthesized graph and dump it.
    [[nodiscard]] std::string run_tier2_demo();

    [[nodiscard]] const CompilationStats& stats() const noexcept { return stats_; }

    // Access the underlying scheduler (for advanced users).
    [[nodiscard]] enki::TaskScheduler& scheduler() noexcept { return *scheduler_; }

private:
    std::unique_ptr<enki::TaskScheduler> scheduler_;
    SafepointManager                       safepoint_mgr_;
    CompilationStats                      stats_;

    // Per-mutator interpreter state. In a real runtime, each mutator thread
    // would have its own Interpreter; here we have one for simplicity.
    std::unique_ptr<Interpreter> interp_;
};

}  // namespace arcjit
