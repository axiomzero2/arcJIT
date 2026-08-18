// SPDX-License-Identifier: MIT
// arcJIT — Probe: Target Feature Detection
//
// Detects CPU features at runtime using CPUID. This allows the codegen
// to choose lowering paths dynamically (SSE, AVX, BMI, etc.).
#pragma once

#include <cstdint>
#include <string>

namespace arcjit {

struct CpuFeatures {
    // SSE
    bool sse      = false;
    bool sse2     = false;
    bool sse3     = false;
    bool ssse3    = false;
    bool sse41    = false;
    bool sse42    = false;

    // AVX
    bool avx      = false;
    bool avx2     = false;
    bool avx512f  = false;
    bool avx512vl = false;
    bool avx512bw = false;
    bool avx512dq = false;

    // FMA
    bool fma      = false;

    // BMI
    bool bmi1     = false;
    bool bmi2     = false;

    // Bit manipulation
    bool popcnt   = false;
    bool lzcnt    = false;
    bool tzcnt    = false;

    // Other
    bool movbe    = false;
    bool rdtsc    = false;

    // Cache info (approximate).
    uint32_t l1_cache_line_size = 64;
    uint32_t l1_cache_size_kb   = 32;
    uint32_t l2_cache_size_kb   = 256;
    uint32_t l3_cache_size_kb   = 8192;

    // CPU vendor.
    std::string vendor;
    std::string brand;

    // Detect all features.
    void detect();

    // Check if a feature string is supported (e.g. "avx2", "bmi2").
    [[nodiscard]] bool has(std::string_view feature) const;

    // Dump all features as a human-readable string.
    [[nodiscard]] std::string dump() const;
};

// Get the global CpuFeatures (detected once, cached).
const CpuFeatures& cpu_features();

}  // namespace arcjit
