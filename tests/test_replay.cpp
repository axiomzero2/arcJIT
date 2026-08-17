// SPDX-License-Identifier: MIT
// Tests for the replay serialization system (Rule 40).
#include <gtest/gtest.h>

#include <filesystem>

#include "bytecode/chunk.h"
#include "bytecode/heap.h"
#include "core/replay.h"

using namespace arcjit;

TEST(ReplayTest, serialize_deserialize_round_trip) {
    // Build a chunk.
    Chunk c;
    c.set_max_locals(2);
    static Number n1;
    n1.base.type = ObjType::NumberInt; n1.base.ref_count = 1;
    n1.base.is_static = true; n1.as.i = 42;
    static Number n2;
    n2.base.type = ObjType::NumberInt; n2.base.ref_count = 1;
    n2.base.is_static = true; n2.as.i = 99;
    c.add_const(reinterpret_cast<Object*>(&n1));
    c.add_const(reinterpret_cast<Object*>(&n2));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(1);
    c.emit_op(OpCode::Add);
    c.emit_op(OpCode::Return);

    // Serialize.
    ReplayJob job = make_replay_job(c, "test_fn", CompileOptions{});
    EXPECT_EQ(job.name, "test_fn");
    EXPECT_EQ(job.max_locals, 2);
    EXPECT_EQ(job.const_types.size(), 2u);
    EXPECT_EQ(job.bytecode.size(), c.code_size());

    // Write to a temp file.
    std::string path = "test_replay_round_trip.replay";
    auto w = write_replay(job, path);
    ASSERT_TRUE(w.has_value()) << w.error();

    // Read back.
    auto r = read_replay(path);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->name, "test_fn");
    EXPECT_EQ(r->max_locals, 2);
    EXPECT_EQ(r->const_types.size(), 2u);
    EXPECT_EQ(r->bytecode, job.bytecode);

    // Clean up.
    std::filesystem::remove(path);
}

TEST(ReplayTest, chunk_from_replay_preserves_bytecode) {
    Chunk c;
    c.set_max_locals(1);
    static Number n;
    n.base.type = ObjType::NumberInt; n.base.ref_count = 1;
    n.base.is_static = true; n.as.i = 7;
    c.add_const(reinterpret_cast<Object*>(&n));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);

    ReplayJob job = make_replay_job(c, "test", {});
    Chunk c2 = chunk_from_replay(job);

    EXPECT_EQ(c2.code_size(), c.code_size());
    EXPECT_EQ(c2.max_locals(), c.max_locals());
    EXPECT_EQ(c2.constants().size(), 1u);
    // The constant should be a NumberInt with value 7.
    Object* c2_const = c2.constants()[0];
    ASSERT_NE(c2_const, nullptr);
    EXPECT_EQ(c2_const->type, ObjType::NumberInt);
    EXPECT_EQ(cast_to<Number>(c2_const)->as.i, 7);
}

TEST(ReplayTest, options_round_trip) {
    CompileOptions opts;
    opts.opt_level = 1;
    opts.max_inline = 64;
    opts.gvn_enabled = false;
    opts.constfold_enabled = true;
    opts.dce_enabled = false;
    opts.escape_analysis = true;
    opts.rng_seed = 0x123456789ABCDEF0ULL;

    Chunk c;
    ReplayJob job = make_replay_job(c, "opts_test", opts);

    std::string path = "test_replay_opts.replay";
    write_replay(job, path);
    auto r = read_replay(path);
    ASSERT_TRUE(r.has_value());

    EXPECT_EQ(r->options.opt_level, 1u);
    EXPECT_EQ(r->options.max_inline, 64u);
    EXPECT_FALSE(r->options.gvn_enabled);
    EXPECT_TRUE(r->options.constfold_enabled);
    EXPECT_FALSE(r->options.dce_enabled);
    EXPECT_TRUE(r->options.escape_analysis);
    EXPECT_EQ(r->options.rng_seed, 0x123456789ABCDEF0ULL);

    std::filesystem::remove(path);
}

TEST(ReplayTest, bad_magic_returns_error) {
    std::string path = "test_bad_magic.replay";
    {
        std::ofstream f(path, std::ios::binary);
        const char bad[] = "XXXX";
        f.write(bad, 4);
    }
    auto r = read_replay(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("bad magic"), std::string::npos);
    std::filesystem::remove(path);
}

TEST(ReplayTest, nonexistent_file_returns_error) {
    auto r = read_replay("/nonexistent/path/file.replay");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("cannot open"), std::string::npos);
}

TEST(ReplayTest, save_replay_on_failure_creates_file) {
    Chunk c;
    c.set_max_locals(0);
    static Number n;
    n.base.type = ObjType::NumberInt; n.base.ref_count = 1;
    n.base.is_static = true; n.as.i = 1;
    c.add_const(reinterpret_cast<Object*>(&n));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);

    std::string replay_dir = "test_replays";
    std::string path = save_replay_on_failure(c, "fail_test", "test reason", replay_dir);
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(std::filesystem::exists(path));

    // Verify the replay can be read back.
    auto r = read_replay(path);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->name, "fail_test");

    // Clean up.
    std::filesystem::remove_all(replay_dir);
}

TEST(ReplayTest, empty_chunk_round_trips) {
    Chunk c;
    ReplayJob job = make_replay_job(c, "empty", {});
    std::string path = "test_empty.replay";
    write_replay(job, path);
    auto r = read_replay(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->bytecode.size(), 0u);
    EXPECT_EQ(r->const_types.size(), 0u);
    std::filesystem::remove(path);
}

TEST(ReplayTest, float_constant_round_trips) {
    Chunk c;
    static Number f;
    f.base.type = ObjType::NumberFloat; f.base.ref_count = 1;
    f.base.is_static = true; f.as.f = 3.14;
    c.add_const(reinterpret_cast<Object*>(&f));
    c.emit_op(OpCode::LoadConst); c.emit_const_idx(0);
    c.emit_op(OpCode::Return);

    ReplayJob job = make_replay_job(c, "float_test", {});
    std::string path = "test_float.replay";
    write_replay(job, path);
    auto r = read_replay(path);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->const_types.size(), 1u);
    EXPECT_EQ(static_cast<ObjType>(r->const_types[0]), ObjType::NumberFloat);

    // Reconstruct and verify.
    Chunk c2 = chunk_from_replay(*r);
    ASSERT_EQ(c2.constants().size(), 1u);
    Object* c2_const = c2.constants()[0];
    EXPECT_EQ(c2_const->type, ObjType::NumberFloat);
    double f2 = cast_to<Number>(c2_const)->as.f;
    EXPECT_DOUBLE_EQ(f2, 3.14);

    std::filesystem::remove(path);
}
