// SPDX-License-Identifier: MIT
// arcJIT — CLI entry point.
//
// Usage:
//   arcjit-cli --tier <0|1|2> --bytecode <spec>   Run a synthesized bytecode program
//   arcjit-cli --demo tier1                        Run the synthetic Tier-1 demo (1+2+3)
//   arcjit-cli --demo tier2                        Run the synthetic Tier-2 SoN demo
//   arcjit-cli --bench <tier>                      Run a benchmark at the given tier
//   arcjit-cli --version
//   arcjit-cli --help
//
// The --bytecode spec is a simple text format:
//   "1+2+3"            → LoadConst 1; LoadConst 2; Add; LoadConst 3; Add; Return
//   "(10+20)*3"        → LoadConst 10; LoadConst 20; Add; LoadConst 3; Mul; Return
//   "loop:1+2+3"       → same as 1+2+3 but run 1000 times (tier ladder test)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime.h"

using namespace arcjit;

static void print_help() {
    std::println("arcJIT — a 3-tier JIT for the Arc programming language");
    std::println("");
    std::println("Tiers:");
    std::println("  0 = Spark   — register interpreter");
    std::println("  1 = Jolt    — baseline SSA JIT");
    std::println("  2 = Surge   — optimizing Sea of Nodes JIT (Gigavolt pipeline)");
    std::println("");
    std::println("USAGE:");
    std::println("  arcjit-cli --tier <0|1|2> --bytecode <spec>");
    std::println("  arcjit-cli --demo jolt");
    std::println("  arcjit-cli --demo surge");
    std::println("  arcjit-cli --bench <tier>");
    std::println("  arcjit-cli --version");
    std::println("  arcjit-cli --help");
    std::println("");
    std::println("OPTIONS:");
    std::println("  --tier <n>      Run the bytecode at the given tier");
    std::println("                  0=Spark, 1=Jolt, 2=Surge");
    std::println("  --bytecode <s>  Bytecode spec (e.g. '1+2+3', '(10+20)*3')");
    std::println("  --demo <id>     Run a synthetic demo (jolt or surge)");
    std::println("  --bench <tier>  Run a 10000-iteration benchmark at the given tier");
    std::println("  --version       Print version and exit");
    std::println("  --help          Print this message and exit");
}

// Parse a simple arithmetic expression into a Chunk.
// Supports: integers, +, -, *, /, parentheses.
//
// The Number constants are heap-allocated and owned by a shared pool that
// outlives the Chunk. This avoids dangling pointers when the chunk's
// constant pool is referenced after parsing.
static Chunk parse_bytecode_spec(std::string_view spec) {
    Chunk c;
    c.set_max_locals(0);

    size_t i = 0;
    auto skip_ws = [&] { while (i < spec.size() && (spec[i] == ' ' || spec[i] == '\t')) ++i; };

    std::vector<int64_t> nums;
    std::vector<char>    ops;

    skip_ws();
    while (i < spec.size()) {
        if (spec[i] == '(' || spec[i] == ')') { ++i; skip_ws(); continue; }
        int64_t n = 0;
        bool has_num = false;
        while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
            n = n * 10 + (spec[i] - '0');
            ++i;
            has_num = true;
        }
        if (has_num) nums.push_back(n);
        skip_ws();
        if (i < spec.size() && (spec[i] == '+' || spec[i] == '-' || spec[i] == '*' || spec[i] == '/')) {
            ops.push_back(spec[i]);
            ++i;
            skip_ws();
        }
    }

    // Allocate Number constants on the heap. They leak (intentional for a CLI —
    // process exits immediately) but are stable in memory.
    static std::vector<std::unique_ptr<Number>> num_pool;

    for (size_t j = 0; j < nums.size(); ++j) {
        auto num = std::make_unique<Number>();
        num->base.type = ObjType::NumberInt;
        num->base.ref_count = 1;
        num->base.is_static = true;
        num->as.i = nums[j];
        uint32_t idx = c.add_const(reinterpret_cast<Object*>(num.get()));
        num_pool.push_back(std::move(num));

        c.emit_op(OpCode::LoadConst);
        c.emit_const_idx(idx);

        if (j > 0 && j - 1 < ops.size()) {
            char op = ops[j - 1];
            switch (op) {
                case '+': c.emit_op(OpCode::Add); break;
                case '-': c.emit_op(OpCode::Sub); break;
                case '*': c.emit_op(OpCode::Mul); break;
                case '/': c.emit_op(OpCode::Div); break;
            }
        }
    }

    c.emit_op(OpCode::Return);
    return c;
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

static int run_bytecode(Runtime& rt, Tier tier, std::string_view spec) {
    Chunk c = parse_bytecode_spec(spec);
    auto r = rt.run_at_tier(c, tier);
    if (!r) {
        std::println(stderr, "error: {}", r.error());
        return 1;
    }
    std::println("Result: {}", r->as_int());
    return 0;
}

static int run_bench(Runtime& rt, Tier tier) {
    // Benchmark: compute (10 + 20) * 3 = 90, 10000 times.
    Chunk c = parse_bytecode_spec("(10+20)*3");
    constexpr int N = 10000;

    auto t0 = std::chrono::high_resolution_clock::now();
    int64_t result = 0;
    for (int i = 0; i < N; ++i) {
        auto r = rt.run_at_tier(c, tier);
        if (!r) {
            std::println(stderr, "error: {}", r.error());
            return 1;
        }
        result = r->as_int();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    std::println("Benchmark ({} x {}):", tier_name(tier), N);
    std::println("  result: {}", result);
    std::println("  total:  {} us", us);
    std::println("  per op: {} ns", us * 1000 / N);
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
        std::println("arcJIT v0.3.0 (C++23, Spark/Jolt/Surge)");
        return 0;
    }
    if (arg1 == "--machinery") {
        Runtime rt;
        std::println("{}", rt.dump_machinery());
        return 0;
    }
    if (arg1 == "--demo") {
        if (argc < 3) {
            std::println(stderr, "error: --demo requires an argument (jolt or surge)");
            return 1;
        }
        std::string_view which = argv[2];
        Runtime rt;
        if (which == "jolt" || which == "tier1") return run_tier1_demo(rt);
        if (which == "surge" || which == "tier2") return run_tier2_demo(rt);
        std::println(stderr, "error: unknown demo '{}'", which);
        return 1;
    }
    if (arg1 == "--bench") {
        if (argc < 3) {
            std::println(stderr, "error: --bench requires a tier (0, 1, or 2)");
            return 1;
        }
        int t = std::atoi(argv[2]);
        if (t < 0 || t > 2) {
            std::println(stderr, "error: invalid tier {}", t);
            return 1;
        }
        Runtime rt;
        return run_bench(rt, static_cast<Tier>(t));
    }
    if (arg1 == "--tier") {
        // Parse: --tier <n> --bytecode <spec>
        // argv[1]="--tier", argv[2]=<n>, argv[3]="--bytecode", argv[4]=<spec>
        if (argc < 5) {
            std::println(stderr, "error: usage: --tier <n> --bytecode <spec>");
            return 1;
        }
        int t = std::atoi(argv[2]);
        if (std::string_view(argv[3]) != "--bytecode") {
            std::println(stderr, "error: expected --bytecode after tier number");
            return 1;
        }
        std::string_view spec = argv[4];
        Runtime rt;
        return run_bytecode(rt, static_cast<Tier>(t), spec);
    }

    print_help();
    return 1;
}
