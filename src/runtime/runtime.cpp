// SPDX-License-Identifier: MIT
#include "runtime/runtime.h"

#include <chrono>
#include <print>

namespace arcjit {

Runtime::Runtime() {
    scheduler_ = std::make_unique<enki::TaskScheduler>();
    scheduler_->Initialize();

    interp_ = std::make_unique<Interpreter>();
    interp_->attach_safepoint(&safepoint_mgr_);
}

Runtime::~Runtime() = default;

[[nodiscard]] std::expected<Value, std::string> Runtime::run(const Chunk& chunk) {
    stats_.interp_invocations++;

    // Tier 0: always run the interpreter.
    // (In a real runtime we'd consult the invocation counter and trigger
    // Tier 1 compilation when a function crosses kHotThreshold.)
    return interp_->run(chunk);
}

[[nodiscard]] std::expected<Value, std::string> Runtime::run_at_tier(const Chunk& chunk, Tier t) {
    if (t == Tier::Interpreter) {
        return interp_->run(chunk);
    }
    // Tier 1/2 from a Chunk requires lowering; that's wired in run_tier1_demo
    // for the demo case. Running arbitrary bytecode at Tier 1/2 is the next
    // milestone.
    return std::unexpected("Tier 1/2 execution of arbitrary bytecode not wired in this scaffold — "
                            "use run_tier1_demo() / run_tier2_demo() instead.");
}

[[nodiscard]] std::expected<int64_t, std::string> Runtime::run_tier1_demo() {
    auto t0 = std::chrono::high_resolution_clock::now();

    Tier1Function fn = make_demo_add3();

    Tier1Compiler compiler;
    auto maybe_entry = compiler.compile(fn);
    if (!maybe_entry) {
        return std::unexpected(maybe_entry.error());
    }

    void (*entry)() = *maybe_entry;

    // The compiled function returns rax as int64_t. We cast and call.
    int64_t result;
    using EntryFn = int64_t (*)();
    auto fn_ptr = reinterpret_cast<EntryFn>(entry);
    result = fn_ptr();

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.tier1_compiles++;
    stats_.total_compilation_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    return result;
}

[[nodiscard]] std::string Runtime::run_tier2_demo() {
    auto t0 = std::chrono::high_resolution_clock::now();

    Tier2Job job;
    job.function_name = "demo";
    build_demo_graph(job.graph);

    PassResult r = run_tier2_pipeline(job);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.tier2_compiles++;
    stats_.total_compilation_time_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    std::string out;
    out += std::format("Tier-2 demo pipeline finished:\n");
    out += std::format("  changed        : {}\n", r.changed);
    out += std::format("  nodes removed  : {}\n", r.nodes_removed);
    out += std::format("  nodes added    : {}\n", r.nodes_added);
    out += std::format("  time           : {} us\n",
                       std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    out += "\n";
    out += dump_graph_dot(job.graph);
    return out;
}

}  // namespace arcjit
