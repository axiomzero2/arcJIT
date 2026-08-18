// SPDX-License-Identifier: MIT
// Tests for the machinery integration with the Runtime.
#include <gtest/gtest.h>

#include "runtime/runtime.h"
#include "machinery/watchdog.h"
#include "machinery/meter.h"
#include "machinery/probe.h"
#include "machinery/trip.h"
#include "machinery/capacitor.h"
#include "machinery/relay.h"

using namespace arcjit;

static Object* make_num_mw(int64_t v) {
    static std::vector<std::unique_ptr<Number>> pool;
    auto n = std::make_unique<Number>();
    n->base.type = ObjType::NumberInt; n->base.ref_count = 1;
    n->base.is_static = true; n->as.i = v;
    Object* p = reinterpret_cast<Object*>(n.get());
    pool.push_back(std::move(n));
    return p;
}

static Chunk make_simple_chunk_mw() {
    Chunk c;
    c.set_max_locals(0);
    c.add_const(make_num_mw(10));
    c.add_const(make_num_mw(20));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);
    return c;
}

// Runtime initializes all machinery.
TEST(MachineryWiringTest, runtime_has_all_machinery) {
    Runtime rt;
    EXPECT_NO_THROW(rt.watchdog());
    EXPECT_NO_THROW(rt.meter());
    EXPECT_NO_THROW(rt.trip());
    EXPECT_NO_THROW(rt.capacitor());
    EXPECT_NO_THROW(rt.relay());
    EXPECT_NO_THROW(rt.regulator());
    EXPECT_NO_THROW(rt.fuse());
}

// dump_machinery produces non-empty output.
TEST(MachineryWiringTest, dump_machinery_nonempty) {
    Runtime rt;
    auto s = rt.dump_machinery();
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("Watchdog"), std::string::npos);
    EXPECT_NE(s.find("Meter"), std::string::npos);
    EXPECT_NE(s.find("Probe"), std::string::npos);
    EXPECT_NE(s.find("Trip"), std::string::npos);
    EXPECT_NE(s.find("Capacitor"), std::string::npos);
    EXPECT_NE(s.find("Relay"), std::string::npos);
    EXPECT_NE(s.find("Regulator"), std::string::npos);
    EXPECT_NE(s.find("Fuse"), std::string::npos);
}

// Compiling at Tier 1 registers code with Trip.
TEST(MachineryWiringTest, tier1_compile_registers_with_trip) {
    Runtime rt;
    Chunk c = make_simple_chunk_mw();
    auto r = rt.run_at_tier(c, Tier::Tier1Baseline);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->as_int(), 30);

    // Trip should have at least one code entry.
    EXPECT_GT(rt.trip().active_count(), 0u);
}

// Compiling at Tier 2 registers code with Trip.
TEST(MachineryWiringTest, tier2_compile_registers_with_trip) {
    Runtime rt;
    Chunk c = make_simple_chunk_mw();
    auto r = rt.run_at_tier(c, Tier::Tier2Optimizing);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->as_int(), 30);

    // Trip should have at least one code entry.
    EXPECT_GT(rt.trip().active_count(), 0u);
}

// Compiling registers a Watchdog assumption.
TEST(MachineryWiringTest, compile_registers_watchdog_assumption) {
    Runtime rt;
    Chunk c = make_simple_chunk_mw();
    rt.run_at_tier(c, Tier::Tier1Baseline);

    // Watchdog should have at least one assumption.
    EXPECT_GT(rt.watchdog().total_count(), 0u);
    EXPECT_GT(rt.watchdog().active_count(), 0u);
}

// invalidate_chunk resets the tier to interpreter.
TEST(MachineryWiringTest, invalidate_chunk_resets_tier) {
    Runtime rt;
    Chunk c = make_simple_chunk_mw();
    rt.run_at_tier(c, Tier::Tier1Baseline);

    // Invalidate.
    rt.invalidate_chunk(c);

    // Running again should fall back to interpreter.
    auto r = rt.run(c);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->as_int(), 30);
}

// Capacitor tracks code allocations.
TEST(MachineryWiringTest, capacitor_tracks_allocations) {
    Runtime rt;
    Chunk c = make_simple_chunk_mw();
    rt.run_at_tier(c, Tier::Tier1Baseline);
    rt.run_at_tier(c, Tier::Tier2Optimizing);

    // Capacitor should have at least 2 allocations.
    EXPECT_GE(rt.capacitor().allocation_count(), 2u);
}

// Probe detects CPU features.
TEST(MachineryWiringTest, probe_detects_features) {
    auto& f = cpu_features();
    EXPECT_TRUE(f.sse2);  // SSE2 is always available on x86-64.
    EXPECT_FALSE(f.vendor.empty());
}

// After invalidation, recompilation works.
TEST(MachineryWiringTest, recompile_after_invalidation) {
    Runtime rt;
    Chunk c = make_simple_chunk_mw();

    // Compile at Tier 1.
    auto r1 = rt.run_at_tier(c, Tier::Tier1Baseline);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->as_int(), 30);

    // Invalidate.
    rt.invalidate_chunk(c);

    // Recompile at Tier 1.
    auto r2 = rt.run_at_tier(c, Tier::Tier1Baseline);
    ASSERT_TRUE(r2.has_value()) << r2.error();
    EXPECT_EQ(r2->as_int(), 30);

    // Trip should have the new code.
    EXPECT_GT(rt.trip().active_count(), 0u);
}
