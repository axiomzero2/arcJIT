// SPDX-License-Identifier: MIT
// Tests for GroundFault (deopt analytics) and Governor (speculation policy).
#include <gtest/gtest.h>

#include "machinery/ground_fault.h"
#include "machinery/governor.h"
#include "runtime/runtime.h"  // for Tier enum

using namespace arcjit;

// === GroundFault ===

TEST(GroundFaultTest, record_and_count) {
    GroundFault gf;
    DeoptEvent e;
    e.reason = DeoptReason::ShapeMismatch;
    e.chunk_offset = 42;
    e.code_id = 1;
    e.function_name = "test_fn";
    gf.record(e);
    EXPECT_EQ(gf.total_deopts(), 1u);
    EXPECT_EQ(gf.deopts_for_chunk(42), 1u);
}

TEST(GroundFaultTest, multiple_deopts_same_chunk) {
    GroundFault gf;
    for (int i = 0; i < 5; ++i) {
        DeoptEvent e;
        e.reason = DeoptReason::TypeMismatch;
        e.chunk_offset = 10;
        gf.record(e);
    }
    EXPECT_EQ(gf.total_deopts(), 5u);
    EXPECT_EQ(gf.deopts_for_chunk(10), 5u);
}

TEST(GroundFaultTest, storm_detection) {
    GroundFault gf;
    for (int i = 0; i < 10; ++i) {
        DeoptEvent e;
        e.reason = DeoptReason::InlineCacheMiss;
        e.chunk_offset = 5;
        gf.record(e);
    }
    EXPECT_TRUE(gf.is_storm(5, 10));
    EXPECT_FALSE(gf.is_storm(5, 15));  // threshold not met
}

TEST(GroundFaultTest, clear_resets) {
    GroundFault gf;
    DeoptEvent e;
    e.chunk_offset = 1;
    gf.record(e);
    EXPECT_EQ(gf.total_deopts(), 1u);
    gf.clear();
    EXPECT_EQ(gf.total_deopts(), 0u);
    EXPECT_EQ(gf.deopts_for_chunk(1), 0u);
}

TEST(GroundFaultTest, dump_nonempty) {
    GroundFault gf;
    DeoptEvent e;
    e.reason = DeoptReason::BoundsCheck;
    e.chunk_offset = 7;
    e.function_name = "fn";
    gf.record(e);
    auto s = gf.dump();
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("BoundsCheck"), std::string::npos);
    EXPECT_NE(s.find("fn"), std::string::npos);
}

// === Governor ===

TEST(GovernorTest, no_profile_do_not_speculate) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::None;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::DoNotSpeculate);
}

TEST(GovernorTest, storm_recompile) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::VeryHigh;
    ctx.is_monomorphic = true;
    ctx.is_storm = true;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::Recompile);
}

TEST(GovernorTest, megamorphic_types) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::High;
    ctx.distinct_types = 6;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::Megamorphic);
}

TEST(GovernorTest, polymorphic_types_medium_confidence) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::Medium;
    ctx.distinct_types = 3;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::Polymorphic);
}

TEST(GovernorTest, monomorphic_high_confidence_insert_guard) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::High;
    ctx.is_monomorphic = true;
    ctx.distinct_types = 1;
    ctx.code_size_budget_remaining = 1000;
    ctx.guard_cost = 10;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::InsertGuard);
}

TEST(GovernorTest, monomorphic_very_high_clone) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::VeryHigh;
    ctx.is_monomorphic = true;
    ctx.distinct_types = 1;
    ctx.expected_speedup_pct = 50;
    ctx.clone_budget_remaining = 4;
    ctx.code_size_budget_remaining = 1000;
    ctx.guard_cost = 10;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::Clone);
}

TEST(GovernorTest, monomorphic_medium_uncommon_trap) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::Medium;
    ctx.is_monomorphic = true;
    ctx.distinct_types = 1;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::UncommonTrap);
}

TEST(GovernorTest, high_deopt_count_low_confidence_no_speculate) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::Low;
    ctx.deopt_count = 10;
    auto d = Governor::decide(ctx);
    EXPECT_EQ(d, SpeculationDecision::DoNotSpeculate);
}

TEST(GovernorTest, explain_nonempty) {
    SpeculationContext ctx;
    ctx.confidence = ConfidenceLevel::VeryHigh;
    ctx.is_monomorphic = true;
    ctx.expected_speedup_pct = 50;
    ctx.clone_budget_remaining = 1;
    ctx.code_size_budget_remaining = 1000;
    auto d = Governor::decide(ctx);
    auto explanation = Governor::explain(ctx, d);
    EXPECT_FALSE(explanation.empty());
    EXPECT_NE(explanation.find("Clone"), std::string::npos);
}

TEST(GovernorTest, decision_name) {
    EXPECT_EQ(Governor::decision_name(SpeculationDecision::InsertGuard), "InsertGuard");
    EXPECT_EQ(Governor::decision_name(SpeculationDecision::DoNotSpeculate), "DoNotSpeculate");
    EXPECT_EQ(Governor::decision_name(SpeculationDecision::Clone), "Clone");
}

// === Meter wiring into interpreter ===

TEST(MeterWiringTest, interpreter_feeds_meter) {
    Runtime rt;
    // Run a chunk at Spark (interpreter) — Meter should record samples.
    Chunk c;
    c.set_max_locals(0);
    static Number n;
    n.base.type = ObjType::NumberInt; n.base.ref_count = 1;
    n.base.is_static = true; n.as.i = 42;
    c.add_const(reinterpret_cast<Object*>(&n));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);

    // Run a few times to build up profile.
    for (int i = 0; i < 100; ++i) {
        rt.run_at_tier(c, Tier::Interpreter);
    }

    // Meter should have at least one site with samples.
    auto s = rt.meter().summarize();
    EXPECT_GT(s.total_samples, 0u);
}
