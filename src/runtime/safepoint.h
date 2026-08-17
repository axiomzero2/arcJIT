// SPDX-License-Identifier: MIT
// arcJIT — Safepoint / handshake state machine.
//
// Per docs/ARCHITECTURE.md §5.3, we pause the world without OS mutexes by
// having every mutator thread check an atomic flag at loop back-edges and
// call sites. The GC thread requests a safepoint; mutators notice at their
// next safepoint and announce themselves "stopped".
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace arcjit {

enum class SafepointState : uint8_t {
    Running            = 0,
    SafepointRequested = 1,
    Safepointed        = 2,
};

// Per-mutator safepoint state. Stored in thread-local storage.
struct alignas(64) MutatorState {
    std::atomic<SafepointState> state{SafepointState::Running};

    // Counter for stats / debugging.
    std::atomic<uint64_t> safepoint_count{0};

    // Platform futex (we use a small spin-then-futex pattern via std::condition_variable
    // for portability; in production this would be a Linux futex on the state word).
    std::mutex             wait_mu;
    std::condition_variable wait_cv;

    // Called by mutators at every safepoint.
    void check_safepoint() {
        // Fast path: load with relaxed — no fence needed for the common case.
        if (state.load(std::memory_order_relaxed) != SafepointState::SafepointRequested) {
            return;
        }

        // Slow path: announce ourselves as safepointed and wait for release.
        state.store(SafepointState::Safepointed, std::memory_order_relaxed);
        safepoint_count.fetch_add(1, std::memory_order_relaxed);

        // We don't actually block in this initial scaffold — real blocking
        // requires the GC to also wake us up, which we wire up when the GC
        // module is added. For now, busy-wait on state changing back to Running.
        while (state.load(std::memory_order_acquire) != SafepointState::Running) {
            std::this_thread::yield();
        }
    }

    // Called by the GC / orchestrator thread to request a safepoint.
    void request() {
        state.store(SafepointState::SafepointRequested, std::memory_order_release);
    }

    // Called by the GC after work is done.
    void release() {
        state.store(SafepointState::Running, std::memory_order_release);
    }
};

// Global safepoint manager — shared across all mutators.
class SafepointManager {
public:
    void register_mutator(MutatorState* s) {
        std::lock_guard<std::mutex> g(mu_);
        mutators_.push_back(s);
    }

    void unregister_mutator(MutatorState* s) {
        std::lock_guard<std::mutex> g(mu_);
        std::erase(mutators_, s);
    }

    // Request all mutators to safepoint. Returns when all are safepointed.
    void request_global_safepoint() {
        std::lock_guard<std::mutex> g(mu_);
        for (auto* m : mutators_) m->request();
        for (auto* m : mutators_) {
            while (m->state.load(std::memory_order_acquire) != SafepointState::Safepointed) {
                std::this_thread::yield();
            }
        }
    }

    void release_all() {
        std::lock_guard<std::mutex> g(mu_);
        for (auto* m : mutators_) m->release();
    }

private:
    std::mutex                   mu_;
    std::vector<MutatorState*>   mutators_;
};

}  // namespace arcjit
