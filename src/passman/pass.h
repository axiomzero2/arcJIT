// SPDX-License-Identifier: MIT
// arcJIT — Pass Manager.
//
// Per docs/ARCHITECTURE.md §4, every pass must be idempotent and monotonic
// decreasing in IR size (or guarded by a budget). The pass manager runs
// passes to a fixpoint.
//
// In debug builds (Rule 42), the pipeline runs the graph verifier after
// every pass. In release builds, verification is skipped for speed.
#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/graph.h"
#include "passman/instrument.h"

namespace arcjit {

// Forward
class Pass;

// Result of running a pass.
struct PassResult {
    bool     changed   = false;
    uint32_t nodes_removed = 0;
    uint32_t nodes_added   = 0;
    int      budget_used    = 0;

    PassResult& operator|=(const PassResult& o) {
        changed       = changed || o.changed;
        nodes_removed += o.nodes_removed;
        nodes_added   += o.nodes_added;
        budget_used   += o.budget_used;
        return *this;
    }
};

// Base pass. Each pass is a small, idempotent reducer.
class Pass {
public:
    explicit Pass(std::string_view name) : name_(name) {}
    virtual ~Pass() = default;

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // Run on a graph. Must be idempotent.
    virtual PassResult run(Graph& g) = 0;

private:
    std::string_view name_;
};

// Pipeline of passes. Runs each pass once, optionally to a fixpoint.
class PassPipeline {
public:
    void add(std::unique_ptr<Pass> p) { passes_.push_back(std::move(p)); }

    [[nodiscard]] size_t size() const noexcept { return passes_.size(); }

    // Run all passes once. In debug builds, verifies the graph after each pass.
    PassResult run_once(Graph& g) {
        PassResult total;
        auto& instr = global_instrumentation();

        for (auto& p : passes_) {
            instr.record(PassEventType::PassBegin, p->name());

            PassResult r = p->run(g);
            total |= r;

            instr.record(PassEventType::PassEnd, p->name());

#ifndef NDEBUG
            // Rule 42: verify after every pass in debug builds.
            verify_or_die(g, std::format("after pass {}", p->name()));
#endif

            if (!r.changed) {
                // No-op pass: skip in future runs.
            }
        }
        return total;
    }

    // Run all passes to fixpoint, with a maximum iteration budget.
    PassResult run_to_fixpoint(Graph& g, uint32_t max_iter = 16) {
        PassResult total;
        for (uint32_t i = 0; i < max_iter; ++i) {
            PassResult r = run_once(g);
            total |= r;
            if (!r.changed) break;
        }
        return total;
    }

private:
    std::vector<std::unique_ptr<Pass>> passes_;
};

// --- Built-in passes --------------------------------------------------------

// Dead code elimination — drop any node with zero uses that has no side effects.
class DeadCodeElimPass : public Pass {
public:
    DeadCodeElimPass() : Pass("DCE") {}
    PassResult run(Graph& g) override;
};

// Constant folding — fold ConstInt + ConstInt → ConstInt, etc.
class ConstantFoldingPass : public Pass {
public:
    ConstantFoldingPass() : Pass("ConstFold") {}
    PassResult run(Graph& g) override;
};

// Global value numbering — replace duplicate computations with a single node.
class GVNPass : public Pass {
public:
    GVNPass() : Pass("GVN") {}
    PassResult run(Graph& g) override;
};

}  // namespace arcjit
