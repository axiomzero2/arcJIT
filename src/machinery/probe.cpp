// SPDX-License-Identifier: MIT
#include "machinery/probe.h"

#include <cstring>
#include <format>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace arcjit {

#if defined(__x86_64__) || defined(__i386__)

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    __cpuid_count(leaf, subleaf, a, b, c, d);
}

void CpuFeatures::detect() {
    uint32_t a, b, c, d;

    // Vendor string (leaf 0).
    if (__get_cpuid(0, &a, &b, &c, &d)) {
        char vendor_str[13] = {};
        memcpy(vendor_str,     &b, 4);
        memcpy(vendor_str + 4, &d, 4);
        memcpy(vendor_str + 8, &c, 4);
        vendor = vendor_str;
    }

    // Brand string (leaves 0x80000002-0x80000004).
    if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004) {
        char brand_str[49] = {};
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; ++leaf) {
            cpuid(leaf, 0, a, b, c, d);
            uint32_t offset = (leaf - 0x80000002) * 16;
            memcpy(brand_str + offset,      &a, 4);
            memcpy(brand_str + offset + 4,  &b, 4);
            memcpy(brand_str + offset + 8,  &c, 4);
            memcpy(brand_str + offset + 12, &d, 4);
        }
        brand = brand_str;
    }

    // Feature flags (leaf 1).
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        sse   = d & (1 << 25);
        sse2  = d & (1 << 26);
        sse3  = c & (1 << 0);
        ssse3 = c & (1 << 9);
        sse41 = c & (1 << 19);
        sse42 = c & (1 << 20);
        popcnt = c & (1 << 23);
        avx   = c & (1 << 28);
        fma   = c & (1 << 12);
        movbe = c & (1 << 22);
        rdtsc = d & (1 << 4);
    }

    // Extended features (leaf 7, subleaf 0).
    if (__get_cpuid(7, &a, &b, &c, &d)) {
        bmi1   = b & (1 << 3);
        bmi2   = b & (1 << 8);
        avx2   = b & (1 << 5);
        avx512f  = b & (1 << 16);
        avx512vl = b & (1 << 31);
        avx512bw = b & (1 << 30);
        avx512dq = b & (1 << 17);
    }

    // LZCNT (leaf 0x80000001).
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) {
        lzcnt = c & (1 << 5);
    }

    // TZCNT is part of BMI1.
    tzcnt = bmi1;

    // Cache info (leaf 0x80000005-0x80000006).
    if (__get_cpuid(0x80000005, &a, &b, &c, &d)) {
        l1_cache_size_kb = (c >> 24) & 0xFF;
        l1_cache_line_size = (c >> 0) & 0xFF;
    }
    if (__get_cpuid(0x80000006, &a, &b, &c, &d)) {
        l2_cache_size_kb = (c >> 16) & 0xFFFF;
        l3_cache_size_kb = (d >> 18) & 0x3FFF;
        if (l3_cache_size_kb > 0) l3_cache_size_kb *= 512;
    }
}

#else

void CpuFeatures::detect() {
    // Non-x86: no CPUID. Defaults remain false.
    vendor = "unknown";
    brand  = "unknown";
}

#endif

bool CpuFeatures::has(std::string_view feature) const {
    if (feature == "sse")    return sse;
    if (feature == "sse2")   return sse2;
    if (feature == "sse3")   return sse3;
    if (feature == "ssse3")  return ssse3;
    if (feature == "sse41")  return sse41;
    if (feature == "sse42")  return sse42;
    if (feature == "avx")    return avx;
    if (feature == "avx2")   return avx2;
    if (feature == "avx512f")  return avx512f;
    if (feature == "avx512vl") return avx512vl;
    if (feature == "fma")    return fma;
    if (feature == "bmi1")   return bmi1;
    if (feature == "bmi2")   return bmi2;
    if (feature == "popcnt") return popcnt;
    if (feature == "lzcnt")  return lzcnt;
    if (feature == "tzcnt")  return tzcnt;
    if (feature == "movbe")  return movbe;
    return false;
}

std::string CpuFeatures::dump() const {
    std::string out;
    out += std::format("=== Probe: CPU Features ===\n");
    out += std::format("  vendor: {}\n", vendor);
    out += std::format("  brand:  {}\n", brand);
    out += std::format("  SSE: {} SSE2: {} SSE3: {} SSSE3: {} SSE4.1: {} SSE4.2: {}\n",
                       sse, sse2, sse3, ssse3, sse41, sse42);
    out += std::format("  AVX: {} AVX2: {} AVX512F: {} AVX512VL: {}\n",
                       avx, avx2, avx512f, avx512vl);
    out += std::format("  FMA: {} BMI1: {} BMI2: {}\n", fma, bmi1, bmi2);
    out += std::format("  POPCNT: {} LZCNT: {} TZCNT: {} MOVBE: {}\n",
                       popcnt, lzcnt, tzcnt, movbe);
    out += std::format("  L1: {}KB/{}B  L2: {}KB  L3: {}KB\n",
                       l1_cache_size_kb, l1_cache_line_size,
                       l2_cache_size_kb, l3_cache_size_kb);
    return out;
}

const CpuFeatures& cpu_features() {
    static CpuFeatures features;
    static bool detected = false;
    if (!detected) {
        features.detect();
        detected = true;
    }
    return features;
}

}  // namespace arcjit
