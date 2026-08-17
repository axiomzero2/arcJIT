// SPDX-License-Identifier: MIT
// arcJIT — CLI entry point.
//
// Usage:
//   arcjit-cli --tier 0 <chunk.arc>      Run via interpreter only
//   arcjit-cli --tier 1 <chunk.arc>      Run via Tier-1 baseline JIT
//   arcjit-cli --tier 2 <chunk.arc>      Run via Tier-2 SoN JIT
//   arcjit-cli --demo tier1              Run the synthetic Tier-1 demo (1+2+3)
//   arcjit-cli --demo tier2              Run the synthetic Tier-2 SoN demo
//   arcjit-cli --dump-ir <chunk.arc>     Dump the IR graph for a chunk
//   arcjit-cli --version
//   arcjit-cli --help

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <print>
#include <string>
#include <string_view>

#include "runtime/runtime.h"

using namespace arcjit;

static void print_help() {
    std::println("arcJIT — a 3-tier JIT for the Arc programming language");
    std::println("");
    std::println("USAGE:");
    std::println("  arcjit-cli --tier <0|1|2> <chunk>");
    std::println("  arcjit-cli --demo tier1");
    std::println("  arcjit-cli --demo tier2");
    std::println("  arcjit-cli --version");
    std::println("  arcjit-cli --help");
    std::println("");
    std::println("OPTIONS:");
    std::println("  --tier <n>   Run the chunk at the given tier");
    std::println("               0 = interpreter, 1 = baseline SSA JIT, 2 = Sea of Nodes");
    std::println("  --demo <id>  Run a synthetic demo (tier1 or tier2)");
    std::println("  --dump-ir    Dump the IR graph (for --tier 2 or --demo tier2)");
    std::println("  --version    Print version and exit");
    std::println("  --help       Print this message and exit");
}

static int run_tier1_demo(Runtime& rt) {
    auto result = rt.run_tier1_demo();
    if (!result) {
        std::println(stderr, "error: {}", result.error());
        return 1;
    }
    std::println("Tier-1 demo result: 1 + 2 + 3 = {}", *result);
    return 0;
}

static int run_tier2_demo(Runtime& rt) {
    std::string out = rt.run_tier2_demo();
    std::println("{}", out);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string_view arg1 = argv[1];

    if (arg1 == "--help" || arg1 == "-h") {
        print_help();
        return 0;
    }
    if (arg1 == "--version") {
        std::println("arcJIT v0.1.0 (C++23)");
        return 0;
    }
    if (arg1 == "--demo") {
        if (argc < 3) {
            std::println(stderr, "error: --demo requires an argument (tier1 or tier2)");
            return 1;
        }
        std::string_view which = argv[2];

        Runtime rt;

        if (which == "tier1") return run_tier1_demo(rt);
        if (which == "tier2") return run_tier2_demo(rt);

        std::println(stderr, "error: unknown demo '{}'", which);
        return 1;
    }

    // For now, anything else falls through to the help text.
    print_help();
    return 1;
}
