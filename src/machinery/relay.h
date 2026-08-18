// SPDX-License-Identifier: MIT
// arcJIT — Relay: Runtime Stub Library
//
// Reusable runtime stubs for IC miss handlers, deopt entry points,
// allocation slow paths, shape transition stubs, etc. Instead of emitting
// complicated runtime logic inline everywhere, stubs are registered once
// and called from generated code.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace arcjit {

enum class StubKind : uint8_t {
    ICMiss           = 0,  // inline cache miss handler
    DeoptEntry       = 1,  // deoptimization entry point
    AllocationSlow   = 2,  // allocation slow path (arena full)
    ShapeTransition  = 3,  // object shape transition
    OSREntry         = 4,  // on-stack replacement entry
    FFITransition    = 5,  // FFI call transition
    ExceptionThrow   = 6,  // exception throw stub
    TypeCheckFail    = 7,  // type check failure handler
    BoundsCheckFail  = 8,  // bounds check failure handler
    StackOverflow    = 9,  // stack overflow handler
    GCEntry          = 10, // GC safepoint entry
};

struct StubEntry {
    StubKind    kind;
    void*       address = nullptr;
    uint32_t    size    = 0;
    std::string name;
};

class Relay {
public:
    // Register a runtime stub.
    void register_stub(StubKind kind, void* address, uint32_t size,
                        std::string_view name);

    // Look up a stub by kind.
    [[nodiscard]] void* get_stub(StubKind kind) const;

    // Look up a stub by name.
    [[nodiscard]] void* get_stub_by_name(std::string_view name) const;

    // Check if a stub is registered.
    [[nodiscard]] bool has_stub(StubKind kind) const;

    // Get the number of registered stubs.
    [[nodiscard]] size_t stub_count() const noexcept { return stubs_.size(); }

    // Dump all stubs (for debugging).
    [[nodiscard]] std::string dump() const;

private:
    std::unordered_map<uint32_t, StubEntry> stubs_;        // keyed by StubKind
    std::unordered_map<std::string, void*>  name_index_;
};

Relay& global_relay();

}  // namespace arcjit
