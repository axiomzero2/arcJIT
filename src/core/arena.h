// SPDX-License-Identifier: MIT
// arcJIT — Arena bump allocator.
//
// Every IR node, basic block, side-table entry, and small string is allocated
// from a thread-local bump arena. Free is bulk — drop the whole arena at end
// of compilation. Allocating a node is one pointer bump, branchless modulo
// alignment.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <new>
#include <span>
#include <vector>

namespace arcjit {

// One arena slab. Owned by Arena, not exposed publicly.
struct ArenaSlab {
    uint8_t* base = nullptr;
    size_t   size = 0;
    size_t   used = 0;
};

// Bump allocator. Not thread-safe by design — use one Arena per compiler
// thread. The thread pool orchestrator hands out fresh arenas per task.
class Arena {
public:
    Arena() = default;
    ~Arena() { release(); }

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept
        : slabs_(std::move(other.slabs_)), current_slab_(other.current_slab_) {
        other.current_slab_ = 0;
    }
    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            release();
            slabs_        = std::move(other.slabs_);
            current_slab_ = other.current_slab_;
            other.current_slab_ = 0;
        }
        return *this;
    }

    // Allocate `bytes` bytes with `align` alignment.
    void* allocate(size_t bytes, size_t align) {
        // Bump within current slab
        if (!slabs_.empty()) {
            auto& slab = slabs_[current_slab_];
            size_t aligned = (slab.used + align - 1) & ~(align - 1);
            if (aligned + bytes <= slab.size) {
                void* ptr = slab.base + aligned;
                slab.used = aligned + bytes;
                return ptr;
            }
        }
        // Need a new slab. Size it to at least 2× the requested size, with a
        // minimum of 64 KiB (typical for JIT IR graphs).
        grow_slab(std::max(bytes, size_t{64 * 1024}));
        return allocate(bytes, align);
    }

    // Typed allocator. T must be trivially destructible (we never call dtors).
    template <typename T, typename... Args>
    T* alloc(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena only stores trivially-destructible types");
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    // Allocate a contiguous array of T. Returns a writable span.
    template <typename T>
    std::span<T> alloc_array(size_t n) {
        static_assert(std::is_trivially_destructible_v<T>);
        void* mem = allocate(sizeof(T) * n, alignof(T));
        return {static_cast<T*>(mem), n};
    }

    // Bytes used across all slabs. Mostly for stats and tests.
    [[nodiscard]] size_t bytes_used() const noexcept {
        size_t total = 0;
        for (const auto& s : slabs_) total += s.used;
        return total;
    }

    // Bulk free. Returns the arena to a fresh state.
    void release() noexcept {
        for (auto& s : slabs_) {
            if (s.base) std::free(s.base);
        }
        slabs_.clear();
        current_slab_ = 0;
    }

    // Reset for reuse without freeing slabs (cheaper than release + alloc).
    void reset() noexcept {
        for (auto& s : slabs_) s.used = 0;
        current_slab_ = 0;
    }

private:
    void grow_slab(size_t min_size) {
        size_t sz = 64 * 1024;
        while (sz < min_size) sz *= 2;
        ArenaSlab s;
        s.base = static_cast<uint8_t*>(std::aligned_alloc(64, sz));
        // If allocation fails, we abort. The JIT proper is compiled with
        // -fno-exceptions, so we can't throw bad_alloc.
        if (!s.base) {
            // Re-enter exception mode just for this single fatal path.
            // (This is acceptable because the runtime is dead anyway.)
            std::terminate();
        }
        s.size = sz;
        s.used = 0;
        slabs_.push_back(s);
        current_slab_ = slabs_.size() - 1;
    }

    std::vector<ArenaSlab> slabs_;
    size_t                 current_slab_ = 0;
};

}  // namespace arcjit
