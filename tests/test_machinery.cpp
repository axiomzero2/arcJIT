// SPDX-License-Identifier: MIT
// Tests for the Wave 1 machinery: Watchdog, Meter, Probe, Regulator, Fuse,
// Trip, Capacitor, Relay.
#include <gtest/gtest.h>

#include "machinery/watchdog.h"
#include "machinery/meter.h"
#include "machinery/probe.h"
#include "machinery/regulator.h"
#include "machinery/fuse.h"
#include "machinery/trip.h"
#include "machinery/capacitor.h"
#include "machinery/relay.h"

using namespace arcjit;

// === Watchdog ===

TEST(WatchdogTest, register_and_check_valid) {
    Watchdog w;
    uint32_t id = w.register_assumption(AssumptionKind::ShapeStable, 42, nullptr, "test");
    EXPECT_TRUE(w.is_valid(id));
    EXPECT_EQ(w.active_count(), 1u);
}

TEST(WatchdogTest, invalidate_marks_invalid) {
    Watchdog w;
    bool callback_called = false;
    uint32_t id = w.register_assumption(
        AssumptionKind::GlobalConstant, 99,
        [&](uint32_t) { callback_called = true; }, "global_x");
    EXPECT_TRUE(w.is_valid(id));
    w.invalidate(id);
    EXPECT_FALSE(w.is_valid(id));
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(w.invalidation_count(), 1u);
}

TEST(WatchdogTest, invalidate_by_kind) {
    Watchdog w;
    uint32_t id1 = w.register_assumption(AssumptionKind::ShapeStable, 1, nullptr, "a");
    uint32_t id2 = w.register_assumption(AssumptionKind::ShapeStable, 1, nullptr, "b");
    uint32_t id3 = w.register_assumption(AssumptionKind::ShapeStable, 2, nullptr, "c");
    w.invalidate_by_kind(AssumptionKind::ShapeStable, 1);
    EXPECT_FALSE(w.is_valid(id1));
    EXPECT_FALSE(w.is_valid(id2));
    EXPECT_TRUE(w.is_valid(id3));
}

TEST(WatchdogTest, double_invalidate_is_noop) {
    Watchdog w;
    uint32_t id = w.register_assumption(AssumptionKind::TypeStable, 0, nullptr, "x");
    w.invalidate(id);
    EXPECT_EQ(w.invalidation_count(), 1u);
    w.invalidate(id);  // already invalid
    EXPECT_EQ(w.invalidation_count(), 1u);
}

// === Meter ===

TEST(MeterTest, no_samples_is_none_confidence) {
    ProfileEntry e;
    EXPECT_EQ(e.confidence(), ConfidenceLevel::None);
    EXPECT_FALSE(e.can_speculate_monomorphic());
}

TEST(MeterTest, high_sample_count_is_high_confidence) {
    ProfileEntry e;
    for (int i = 0; i < 15000; ++i) {
        e.record_sample(1, 1);  // always same type/shape
    }
    EXPECT_EQ(e.confidence(), ConfidenceLevel::VeryHigh);
    EXPECT_TRUE(e.can_speculate_monomorphic());
}

TEST(MeterTest, deopt_lowers_confidence) {
    ProfileEntry e;
    for (int i = 0; i < 15000; ++i) e.record_sample(1, 1);
    e.record_deopt();
    // Immediately after deopt, confidence is Low (last_deopt_age < 1000).
    EXPECT_EQ(e.confidence(), ConfidenceLevel::Low);
    // After 2000 more samples, confidence recovers to High (deopt_count > 0
    // but last_deopt_age >= 1000).
    for (int i = 0; i < 2000; ++i) e.record_sample(1, 1);
    EXPECT_EQ(e.confidence(), ConfidenceLevel::High);
}

TEST(MeterTest, many_distinct_types_is_low) {
    ProfileEntry e;
    for (int i = 0; i < 200; ++i) e.record_sample(i % 5, i % 5);
    EXPECT_LE(e.confidence(), ConfidenceLevel::Low);
    EXPECT_FALSE(e.can_speculate_monomorphic());
}

TEST(MeterTest, summarize_works) {
    Meter m;
    auto& e = m.entry_for(42);
    for (int i = 0; i < 1000; ++i) e.record_sample(1, 1);
    auto s = m.summarize();
    EXPECT_EQ(s.total_sites, 1u);
    EXPECT_EQ(s.total_samples, 1000u);
    EXPECT_EQ(s.monomorphic, 1u);
}

// === Probe ===

TEST(ProbeTest, detects_features) {
    auto& f = cpu_features();
    // On x86-64, SSE2 is always available.
    EXPECT_TRUE(f.sse2);
    // Vendor should be non-empty.
    EXPECT_FALSE(f.vendor.empty());
}

TEST(ProbeTest, has_string_lookup) {
    auto& f = cpu_features();
    EXPECT_EQ(f.has("sse2"), f.sse2);
    EXPECT_EQ(f.has("avx2"), f.avx2);
    EXPECT_FALSE(f.has("nonexistent"));
}

TEST(ProbeTest, dump_nonempty) {
    auto& f = cpu_features();
    EXPECT_FALSE(f.dump().empty());
}

// === Regulator ===

TEST(RegulatorTest, cost_worth_it) {
    CompileCost c;
    c.expected_speedup_pct = 50;
    c.compile_time_us = 100;
    EXPECT_TRUE(c.is_worth_it());
}

TEST(RegulatorTest, cost_not_worth_low_speedup) {
    CompileCost c;
    c.expected_speedup_pct = 5;
    c.compile_time_us = 20000;
    EXPECT_FALSE(c.is_worth_it());
}

TEST(RegulatorTest, cost_not_worth_too_many_guards) {
    CompileCost c;
    c.expected_speedup_pct = 50;
    c.guard_count = 30;
    c.deopt_risk_score = 80;
    EXPECT_FALSE(c.is_worth_it());
}

TEST(RegulatorTest, merge_costs) {
    CompileCost a, b;
    a.graph_nodes = 10;
    a.code_size_bytes = 100;
    b.graph_nodes = 20;
    b.code_size_bytes = 200;
    a.merge(b);
    EXPECT_EQ(a.graph_nodes, 30u);
    EXPECT_EQ(a.code_size_bytes, 300u);
}

TEST(RegulatorTest, inline_budget) {
    InlineBudget b;
    EXPECT_TRUE(b.can_inline(20, 1));
    EXPECT_FALSE(b.can_inline(50, 1));  // too many nodes
    EXPECT_FALSE(b.can_inline(10, 5));  // too deep
    b.consume(20);
    EXPECT_FALSE(b.can_inline(240, 1));  // would exceed total
}

// === Fuse ===

TEST(FuseTest, check_budgets_ok) {
    Fuse f;
    auto blown = f.check(1000, 100, 500, 1024, 5);
    EXPECT_TRUE(blown.empty());
}

TEST(FuseTest, check_compile_time_blown) {
    Fuse f;
    auto blown = f.check(100000, 100, 500, 1024, 5);
    EXPECT_EQ(blown, "compile_time");
}

TEST(FuseTest, check_graph_nodes_blown) {
    Fuse f;
    auto blown = f.check(1000, 200000, 500, 1024, 5);
    EXPECT_EQ(blown, "graph_nodes");
}

TEST(FuseTest, clone_budget) {
    Fuse f;
    EXPECT_TRUE(f.register_clone(1));
    EXPECT_TRUE(f.register_clone(1));
    EXPECT_TRUE(f.register_clone(1));
    EXPECT_TRUE(f.register_clone(1));
    EXPECT_FALSE(f.register_clone(1));  // max 4 per function
    EXPECT_EQ(f.total_clones(), 4u);
}

// === Trip ===

TEST(TripTest, register_and_check_valid) {
    Trip t;
    uint32_t id = t.register_code(reinterpret_cast<void*>(0x1000), 256, "test_fn");
    EXPECT_TRUE(t.is_valid(id));
    EXPECT_EQ(t.active_count(), 1u);
}

TEST(TripTest, invalidate_code) {
    Trip t;
    uint32_t id = t.register_code(reinterpret_cast<void*>(0x2000), 512, "fn2");
    EXPECT_TRUE(t.is_valid(id));
    t.invalidate(id);
    EXPECT_FALSE(t.is_valid(id));
    EXPECT_EQ(t.invalidation_count(), 1u);
}

TEST(TripTest, invalidate_by_assumption) {
    Trip t;
    uint32_t code_id = t.register_code(reinterpret_cast<void*>(0x3000), 128, "fn3");
    uint32_t assumption_id = 42;
    t.add_dependency(code_id, assumption_id);
    EXPECT_TRUE(t.is_valid(code_id));
    t.invalidate_by_assumption(assumption_id);
    EXPECT_FALSE(t.is_valid(code_id));
}

TEST(TripTest, double_invalidate_noop) {
    Trip t;
    uint32_t id = t.register_code(reinterpret_cast<void*>(0x4000), 64, "fn4");
    t.invalidate(id);
    EXPECT_EQ(t.invalidation_count(), 1u);
    t.invalidate(id);  // already invalid
    EXPECT_EQ(t.invalidation_count(), 1u);
}

// === Capacitor ===

TEST(CapacitorTest, register_and_track) {
    Capacitor c;
    c.register_allocation(reinterpret_cast<void*>(0x5000), 1024);
    EXPECT_EQ(c.total_allocated(), 1024u);
    EXPECT_EQ(c.allocation_count(), 1u);
}

TEST(CapacitorTest, record_hit) {
    Capacitor c;
    void* base = reinterpret_cast<void*>(0x6000);
    c.register_allocation(base, 2048);
    c.record_hit(base);
    c.record_hit(base);
    // No crash, allocation still present.
    EXPECT_EQ(c.allocation_count(), 1u);
}

TEST(CapacitorTest, evict_cold) {
    Capacitor c;
    // Register 3 allocations.
    c.register_allocation(reinterpret_cast<void*>(0x10000), 4096);
    c.register_allocation(reinterpret_cast<void*>(0x20000), 4096);
    c.register_allocation(reinterpret_cast<void*>(0x30000), 4096);
    // Hit the first two.
    c.record_hit(reinterpret_cast<void*>(0x10000));
    c.record_hit(reinterpret_cast<void*>(0x20000));
    // Age all.
    c.age_all();
    // Evict cold (the third allocation has no hits).
    uint64_t freed = c.evict_cold(4096);
    EXPECT_GE(freed, 4096u);
    EXPECT_EQ(c.eviction_count(), 1u);
}

// === Relay ===

TEST(RelayTest, register_and_lookup) {
    Relay r;
    void* addr = reinterpret_cast<void*>(0x8000);
    r.register_stub(StubKind::ICMiss, addr, 64, "ic_miss_stub");
    EXPECT_EQ(r.get_stub(StubKind::ICMiss), addr);
    EXPECT_TRUE(r.has_stub(StubKind::ICMiss));
    EXPECT_FALSE(r.has_stub(StubKind::DeoptEntry));
}

TEST(RelayTest, lookup_by_name) {
    Relay r;
    void* addr = reinterpret_cast<void*>(0x9000);
    r.register_stub(StubKind::DeoptEntry, addr, 128, "deopt_entry");
    EXPECT_EQ(r.get_stub_by_name("deopt_entry"), addr);
    EXPECT_EQ(r.get_stub_by_name("nonexistent"), nullptr);
}

TEST(RelayTest, stub_count) {
    Relay r;
    EXPECT_EQ(r.stub_count(), 0u);
    r.register_stub(StubKind::ICMiss, reinterpret_cast<void*>(0x1), 10, "a");
    r.register_stub(StubKind::DeoptEntry, reinterpret_cast<void*>(0x2), 20, "b");
    EXPECT_EQ(r.stub_count(), 2u);
}
