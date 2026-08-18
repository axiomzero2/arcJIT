// SPDX-License-Identifier: MIT
#include "machinery/relay.h"

#include <format>

namespace arcjit {

static std::string_view stub_kind_name(StubKind k) {
    switch (k) {
        case StubKind::ICMiss:          return "ICMiss";
        case StubKind::DeoptEntry:      return "DeoptEntry";
        case StubKind::AllocationSlow:  return "AllocationSlow";
        case StubKind::ShapeTransition: return "ShapeTransition";
        case StubKind::OSREntry:        return "OSREntry";
        case StubKind::FFITransition:   return "FFITransition";
        case StubKind::ExceptionThrow:  return "ExceptionThrow";
        case StubKind::TypeCheckFail:   return "TypeCheckFail";
        case StubKind::BoundsCheckFail: return "BoundsCheckFail";
        case StubKind::StackOverflow:   return "StackOverflow";
        case StubKind::GCEntry:         return "GCEntry";
    }
    return "Unknown";
}

void Relay::register_stub(StubKind kind, void* address, uint32_t size,
                           std::string_view name) {
    StubEntry e;
    e.kind    = kind;
    e.address = address;
    e.size    = size;
    e.name    = std::string(name);
    stubs_[static_cast<uint32_t>(kind)] = std::move(e);
    name_index_[std::string(name)] = address;
}

void* Relay::get_stub(StubKind kind) const {
    auto it = stubs_.find(static_cast<uint32_t>(kind));
    return it != stubs_.end() ? it->second.address : nullptr;
}

void* Relay::get_stub_by_name(std::string_view name) const {
    auto it = name_index_.find(std::string(name));
    return it != name_index_.end() ? it->second : nullptr;
}

bool Relay::has_stub(StubKind kind) const {
    return stubs_.count(static_cast<uint32_t>(kind)) > 0;
}

std::string Relay::dump() const {
    std::string out;
    out += std::format("=== Relay ({} stubs) ===\n", stubs_.size());
    for (const auto& [_, e] : stubs_) {
        out += std::format("  {} ({}) addr={} size={}B\n",
                           e.name, stub_kind_name(e.kind),
                           e.address, e.size);
    }
    return out;
}

Relay& global_relay() {
    static thread_local Relay r;
    return r;
}

}  // namespace arcjit
