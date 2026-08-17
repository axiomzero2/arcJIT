// SPDX-License-Identifier: MIT
// arcJIT — Pass instrumentation.
//
// Every pass emits structured PassEvent records that can be used to build:
//   - Pass timeline viewer (which passes dominate compile time)
//   - Change log per node (every transformation a node undergoes)
//   - Diff between pass runs
//
// Environment-variable breakpoints:
//   ARCJIT_BREAK_NODE=<id>   — trap when any pass touches node <id>
//   ARCJIT_BREAK_PASS=<name> — trap when pass <name> runs
//
// In release builds, instrumentation is compiled out via ARCJIT_NO_INSTR.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/node.h"

namespace arcjit {

enum class PassEventType : uint8_t {
    PassBegin        = 0,
    PassEnd          = 1,
    NodeChanged      = 2,
    NodeCreated      = 3,
    NodeDeleted      = 4,
    AnalysisInvalidated = 5,
};

struct PassEvent {
    PassEventType   type;
    std::string_view pass_name;
    NodeId          node;            // invalid if N/A
    uint64_t        timestamp_ns;
    int64_t         memory_delta;    // bytes allocated/freed since last event
};

// Event recorder — one per compilation. Thread-local.
class PassInstrumentation {
public:
    PassInstrumentation() = default;

    void record(PassEventType type, std::string_view pass_name,
                NodeId node = {}, int64_t mem_delta = 0) {
#ifndef ARCJIT_NO_INSTR
        if (!enabled_) return;

        PassEvent e;
        e.type          = type;
        e.pass_name     = pass_name;
        e.node          = node;
        e.timestamp_ns  = now_ns();
        e.memory_delta  = mem_delta;
        events_.push_back(e);

        check_breakpoints_(type, pass_name, node);
#endif
    }

    // Enable/disable recording. Default: enabled in debug builds only.
    void set_enabled(bool e) noexcept { enabled_ = e; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    [[nodiscard]] const std::vector<PassEvent>& events() const noexcept { return events_; }

    // Clear events (for reuse across compilations).
    void clear() noexcept { events_.clear(); }

    // Summary stats for a given pass.
    struct PassStats {
        std::string_view name;
        uint64_t begin_ns = 0;
        uint64_t end_ns   = 0;
        uint64_t duration_ns() const noexcept { return end_ns - begin_ns; }
        uint32_t nodes_created  = 0;
        uint32_t nodes_deleted  = 0;
        uint32_t nodes_changed  = 0;
    };

    [[nodiscard]] std::vector<PassStats> summarize() const;

    // Print a human-readable timeline to stderr.
    void dump_timeline(std::string_view context = "") const;

private:
    bool                     enabled_ =
#ifdef NDEBUG
        false
#else
        true
#endif
        ;
    std::vector<PassEvent>   events_;

    static uint64_t now_ns() noexcept {
        // Use std::chrono for portability.
        // (Inlined here to avoid pulling <chrono> into the header.)
        return static_cast<uint64_t>(__builtin_ia32_rdtsc());
    }

    void check_breakpoints_(PassEventType type, std::string_view pass, NodeId node) {
        // ARCJIT_BREAK_PASS
        if (const char* bp = std::getenv("ARCJIT_BREAK_PASS")) {
            if (type == PassEventType::PassBegin && pass == bp) {
                std::fprintf(stderr, "[arcjit] breakpoint: pass '%.*s' begin\n",
                             static_cast<int>(pass.size()), pass.data());
                __builtin_trap();
            }
        }
        // ARCJIT_BREAK_NODE
        if (const char* bn = std::getenv("ARCJIT_BREAK_NODE")) {
            uint32_t target = static_cast<uint32_t>(std::strtoul(bn, nullptr, 10));
            if (node.valid() && node.value == target) {
                std::fprintf(stderr,
                             "[arcjit] breakpoint: pass '%.*s' touched node n%u (event=%d)\n",
                             static_cast<int>(pass.size()), pass.data(),
                             target, static_cast<int>(type));
                __builtin_trap();
            }
        }
    }
};

// Thread-local global instrumentation instance.
PassInstrumentation& global_instrumentation();

}  // namespace arcjit
