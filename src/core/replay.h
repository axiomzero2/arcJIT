// SPDX-License-Identifier: MIT
// arcJIT — Replay serialization (Rule 40).
//
// Serializes a compile job's inputs (bytecode + profile + options) to a
// binary file so that a failed compilation can be replayed deterministically.
//
// Format (little-endian):
//   [magic: 4 bytes "AJRP"]
//   [version: u32]
//   [bytecode_len: u32]
//   [bytecode: N bytes]
//   [const_count: u32]
//   [consts: N * (u8 type + u64 value)]
//   [max_locals: u32]
//   [name_len: u32]
//   [name: N bytes]
//   [options: u32 flags]
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "bytecode/chunk.h"

namespace arcjit {

struct CompileOptions {
    uint32_t opt_level      = 2;     // 0=none, 1=basic, 2=aggressive
    uint32_t max_inline     = 32;    // max nodes to inline
    bool     gvn_enabled    = true;
    bool     constfold_enabled = true;
    bool     dce_enabled    = true;
    bool     escape_analysis = false;  // not yet implemented
    uint64_t rng_seed       = 0xDEADBEEFCAFEBABEULL;
};

struct ReplayJob {
    std::vector<uint8_t>  bytecode;
    std::vector<uint8_t>  const_types;   // ObjType per constant
    std::vector<int64_t>  const_values;  // numeric value (for Number consts)
    int                   max_locals = 0;
    std::string           name;
    CompileOptions        options;
};

// Serialize a ReplayJob to a binary file.
[[nodiscard]] std::expected<void, std::string>
write_replay(const ReplayJob& job, std::string_view path);

// Deserialize a ReplayJob from a binary file.
[[nodiscard]] std::expected<ReplayJob, std::string>
read_replay(std::string_view path);

// Build a ReplayJob from a Chunk + options.
[[nodiscard]] ReplayJob
make_replay_job(const Chunk& chunk, std::string_view name, CompileOptions opts = {});

// Reconstruct a Chunk from a ReplayJob.
// Note: constant pool reconstruction is best-effort — only Number constants
// are preserved. String/Function/Class constants become nullptr.
[[nodiscard]] Chunk
chunk_from_replay(const ReplayJob& job);

// Save a replay for a failed compilation. The path is derived from the
// chunk name + a timestamp. Returns the path written.
[[nodiscard]] std::string
save_replay_on_failure(const Chunk& chunk, std::string_view name,
                        std::string_view reason,
                        std::string_view replay_dir = "replays");

}  // namespace arcjit
