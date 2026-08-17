// SPDX-License-Identifier: MIT
#include "passman/instrument.h"

#include <algorithm>
#include <cstdio>
#include <print>
#include <unordered_map>

namespace arcjit {

PassInstrumentation& global_instrumentation() {
    static thread_local PassInstrumentation inst;
    return inst;
}

std::vector<PassInstrumentation::PassStats> PassInstrumentation::summarize() const {
    std::unordered_map<std::string_view, PassStats> by_name;

    for (const auto& e : events_) {
        auto& s = by_name[e.pass_name];
        s.name = e.pass_name;
        switch (e.type) {
            case PassEventType::PassBegin:
                s.begin_ns = e.timestamp_ns;
                break;
            case PassEventType::PassEnd:
                s.end_ns = e.timestamp_ns;
                break;
            case PassEventType::NodeCreated:
                s.nodes_created++;
                break;
            case PassEventType::NodeDeleted:
                s.nodes_deleted++;
                break;
            case PassEventType::NodeChanged:
                s.nodes_changed++;
                break;
            default:
                break;
        }
    }

    std::vector<PassStats> out;
    out.reserve(by_name.size());
    for (auto& [_, s] : by_name) out.push_back(s);
    std::sort(out.begin(), out.end(),
              [](const PassStats& a, const PassStats& b) { return a.begin_ns < b.begin_ns; });
    return out;
}

void PassInstrumentation::dump_timeline(std::string_view context) const {
    if (context.empty()) {
        std::println(stderr, "=== Pass timeline ===");
    } else {
        std::println(stderr, "=== Pass timeline ({}) ===", context);
    }

    auto stats = summarize();
    if (stats.empty()) {
        std::println(stderr, "  (no passes recorded)");
        return;
    }

    uint64_t total = 0;
    for (const auto& s : stats) {
        uint64_t dur = s.duration_ns();
        total += dur;
        std::println(stderr, "  {:<20} {:>10} cycles  created={:<4} deleted={:<4} changed={}",
                     s.name, dur, s.nodes_created, s.nodes_deleted, s.nodes_changed);
    }
    std::println(stderr, "  {:<20} {:>10} cycles", "TOTAL", total);
}

}  // namespace arcjit
