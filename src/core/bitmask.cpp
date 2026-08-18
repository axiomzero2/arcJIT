// SPDX-License-Identifier: MIT
#include "core/bitmask.h"

#include <format>

namespace arcjit {

// --- Symbolic flag printing ---

std::string format_node_flags(uint32_t raw) {
    if (raw == 0) return "(none)";
    std::string out;
    auto add = [&](const char* name, uint32_t bit) {
        if (raw & bit) {
            if (!out.empty()) out += " | ";
            out += name;
        }
    };
    add("Pure",          1u << 0);
    add("CSEable",       1u << 1);
    add("GVNable",       1u << 2);
    add("Commutative",   1u << 3);
    add("NoThrow",       1u << 4);
    add("NoDeopt",       1u << 5);
    add("HasFrameState", 1u << 6);
    add("IsAllocated",   1u << 7);
    add("IsPinned",      1u << 8);
    add("IsGuard",       1u << 9);
    add("IsControl",     1u << 10);
    add("IsEffect",      1u << 11);
    add("IsDead",        1u << 12);
    return out;
}

std::string format_analysis_invalid(uint32_t raw) {
    if (raw == 0) return "(none)";
    std::string out;
    auto add = [&](const char* name, uint32_t bit) {
        if (raw & bit) {
            if (!out.empty()) out += " | ";
            out += name;
        }
    };
    add("DominatorTree",      1u << 0);
    add("LoopTree",           1u << 1);
    add("TypeInference",      1u << 2);
    add("RangeAnalysis",      1u << 3);
    add("AliasAnalysis",      1u << 4);
    add("MemoryDependence",   1u << 5);
    add("BranchProbability",  1u << 6);
    add("Liveness",           1u << 7);
    add("FrameStateLiveness", 1u << 8);
    add("ScalarEvolution",    1u << 9);
    add("EscapeState",        1u << 10);
    add("ValueNumbering",     1u << 11);
    return out;
}

std::string format_compile_options(uint64_t raw) {
    if (raw == 0) return "(none)";
    std::string out;
    auto add = [&](const char* name, uint64_t bit) {
        if (raw & bit) {
            if (!out.empty()) out += " | ";
            out += name;
        }
    };
    add("EnableInlining",          1ULL << 0);
    add("EnablePEA",               1ULL << 1);
    add("EnableLoadElimination",   1ULL << 2);
    add("EnableLoopUnrolling",     1ULL << 3);
    add("EnableVectorization",     1ULL << 4);
    add("EnableFastMath",          1ULL << 5);
    add("EnableGuardSinking",      1ULL << 6);
    add("EnableFunctionCloning",   1ULL << 7);
    add("EnableFFISpecialization", 1ULL << 8);
    add("StressDeopt",             1ULL << 9);
    add("Deterministic",           1ULL << 10);
    add("VerifyGraph",             1ULL << 11);
    add("TracePasses",             1ULL << 12);
    return out;
}

}  // namespace arcjit
