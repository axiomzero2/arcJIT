// SPDX-License-Identifier: MIT
// arcJIT — Runtime orchestrator.
//
// Ties the three tiers together. Decides when to compile a chunk up the
// tier ladder, manages the compiler thread pool (enkiTS), and exposes the
// `run(chunk)` API used by the CLI.
//
// Tier ladder:
//   1. Chunk enters at Tier 0 (interpreter).
//   2. After kHotThreshold invocations, Runtime kicks off Tier-1 compilation
//      in the background (enkiTS compiler pool).
//   3. On subsequent invocations, if Tier-1 code is ready, Runtime calls it
//      directly instead of interpreting.
//   4. After kTier2Threshold invocations at Tier 1, Runtime kicks off Tier-2
//      compilation. Tier 2 runs the SoN pipeline (GVN, ConstFold, DCE) and
//      replaces the Tier-1 entry.
//
// OSR (On-Stack Replacement):
//   When a Tier-2 compilation finishes, we can switch to it mid-execution
//   at the next safepoint. The frame state (locals, operand stack) is
//   extracted and passed to the Tier-2 compiled code.
#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "bytecode/chunk.h"
#include "bytecode/value.h"
#include "interp/interpreter.h"
#include "runtime/safepoint.h"
#include "tier1/tier1.h"
#include "tier2/tier2.h"

#include <TaskScheduler.h>

#include "machinery/watchdog.h"
#include "machinery/meter.h"
#include "machinery/probe.h"
#include "machinery/regulator.h"
#include "machinery/fuse.h"
#include "machinery/trip.h"
#include "machinery/capacitor.h"
#include "machinery/relay.h"

namespace arcjit {

enum class Tier : uint8_t {
    Interpreter = 0,
    Tier1Baseline,
    Tier2Optimizing,
};

// Tier names (used in logs, CLI output, stats).
//   Tier 0: Spark  — register interpreter
//   Tier 1: Jolt   — baseline SSA JIT
//   Tier 2: Surge  — optimizing Sea of Nodes JIT (Gigavolt pipeline)
[[nodiscard]] inline std::string_view tier_name(Tier t) noexcept {
    switch (t) {
        case Tier::Interpreter:    return "Spark";
        case Tier::Tier1Baseline:  return "Jolt";
        case Tier::Tier2Optimizing: return "Surge";
    }
    return "unknown";
}

// The Surge optimizing pipeline is called Gigavolt.
inline constexpr std::string_view kGigavoltPipelineName = "Gigavolt";

struct CompilationStats {
    std::atomic<uint64_t> interp_invocations   {0};
    std::atomic<uint64_t> tier1_invocations    {0};
    std::atomic<uint64_t> tier2_invocations    {0};
    std::atomic<uint64_t> tier1_compiles       {0};
    std::atomic<uint64_t> tier2_compiles       {0};
    std::atomic<uint64_t> deopts               {0};
    std::atomic<uint64_t> total_compilation_time_us {0};
    std::atomic<uint64_t> osr_transitions      {0};
};

// Function pointer type for compiled Tier-1/Tier-2 code.
// Takes a void* (locals base) and returns int64_t.
using CompiledEntry = int64_t (*)(void*);

// Per-chunk compilation state. Lives in a concurrent hash map keyed by
// Chunk* (chunk lifetimes are managed by the host).
struct ChunkEntry {
    std::atomic<uint32_t> invocations    {0};
    std::atomic<Tier>     current_tier   {Tier::Interpreter};

    // Compiled entry points. Set by the compiler pool, read by mutators.
    std::mutex            compile_mu;
    CompiledEntry         tier1_entry {nullptr};
    CompiledEntry         tier2_entry {nullptr};
    std::unique_ptr<Tier1Function> tier1_fn;   // owned
    std::unique_ptr<Tier1Function> tier2_fn;   // owned (lowered from SoN)

    // True if a compilation is in progress (prevents duplicate jobs).
    std::atomic<bool>     tier1_compiling {false};
    std::atomic<bool>     tier2_compiling {false};

    // Machinery links.
    uint32_t              tier1_code_id  = 0;  // Trip code ID for Jolt code
    uint32_t              tier2_code_id  = 0;  // Trip code ID for Surge code
    uint32_t              assumption_id  = 0;  // Watchdog assumption for this chunk
};

class Runtime {
public:
    static constexpr uint32_t kHotThreshold   = 100;   // Tier-0 → Tier-1
    static constexpr uint32_t kTier2Threshold = 1000;  // Tier-1 → Tier-2

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

    // Compile a Chunk at Tier 1 and return the entry point.
    [[nodiscard]] std::expected<int64_t (*)(void*), std::string>
    compile_tier1(const Chunk& chunk);

    // Compile a Chunk at Tier 2 (full SoN pipeline) and return the entry point.
    [[nodiscard]] std::expected<int64_t (*)(void*), std::string>
    compile_tier2(const Chunk& chunk);

    // Explicit OSR: take a chunk that's currently running at Tier 0 and
    // switch it to Tier 1 at the next safepoint. The caller passes in the
    // current frame state (locals base pointer).
    [[nodiscard]] std::expected<int64_t, std::string>
    osr_to_tier1(const Chunk& chunk, void* locals_base);

    [[nodiscard]] std::expected<int64_t, std::string>
    osr_to_tier2(const Chunk& chunk, void* locals_base);

    [[nodiscard]] const CompilationStats& stats() const noexcept { return stats_; }

    // Access the underlying scheduler.
    [[nodiscard]] enki::TaskScheduler& scheduler() noexcept { return *scheduler_; }

    // --- Machinery accessors ---
    [[nodiscard]] Watchdog&   watchdog()   noexcept { return *watchdog_; }
    [[nodiscard]] Meter&      meter()      noexcept { return *meter_; }
    [[nodiscard]] Trip&       trip()       noexcept { return *trip_; }
    [[nodiscard]] Capacitor&  capacitor()  noexcept { return *capacitor_; }
    [[nodiscard]] Relay&      relay()      noexcept { return *relay_; }
    [[nodiscard]] Regulator&  regulator()  noexcept { return *regulator_; }
    [[nodiscard]] Fuse&       fuse()       noexcept { return *fuse_; }

    // Dump all machinery state (for CLI --machinery).
    [[nodiscard]] std::string dump_machinery() const;

    // Invalidate all compiled code for a chunk (via Trip/Watchdog).
    void invalidate_chunk(const Chunk& chunk);

    // Control whether the interpreter collects type/shape profile feedback
    // on every opcode. Default: ON (needed for the tier ladder to make
    // speculation decisions). Benchmarks that exercise the interpreter in
    // a tight loop should call set_profiling_enabled(false) to skip the
    // per-opcode Meter/Profile write — this is the dominant cost in Spark
    // when no tier-up is intended.
    void set_profiling_enabled(bool enabled);

private:
    std::unique_ptr<enki::TaskScheduler> scheduler_;
    SafepointManager                       safepoint_mgr_;
    CompilationStats                      stats_;

    // Machinery instances (owned by the runtime).
    std::unique_ptr<Watchdog>   watchdog_;
    std::unique_ptr<Meter>      meter_;
    std::unique_ptr<Trip>       trip_;
    std::unique_ptr<Capacitor>  capacitor_;
    std::unique_ptr<Relay>      relay_;
    std::unique_ptr<Regulator>  regulator_;
    std::unique_ptr<Fuse>       fuse_;

    // Per-mutator interpreter state. In a real runtime, each mutator thread
    // would have its own Interpreter; here we have one for simplicity.
    std::unique_ptr<Interpreter> interp_;

    // Per-chunk compilation state. Protected by `chunks_mu_` for insertion;
    // individual ChunkEntry fields are protected by their own atomics/mutex.
    std::mutex                                                  chunks_mu_;
    std::unordered_map<const Chunk*, std::unique_ptr<ChunkEntry>> chunks_;

    // Look up or create a ChunkEntry for the given chunk.
    ChunkEntry& entry_for_(const Chunk& chunk);

    // Kick off background compilation if thresholds are met.
    void maybe_compile_(ChunkEntry& entry, const Chunk& chunk);
};

}  // namespace arcjit
