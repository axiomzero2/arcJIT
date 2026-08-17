// SPDX-License-Identifier: MIT
#include "core/replay.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <print>

namespace arcjit {

namespace {

constexpr uint32_t kMagic   = 0x50524A41;  // "AJRP" little-endian
constexpr uint32_t kVersion = 1;

// Little-endian binary writers.
void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

void write_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void write_bytes(std::vector<uint8_t>& buf, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
}

void write_string(std::vector<uint8_t>& buf, std::string_view s) {
    write_u32(buf, static_cast<uint32_t>(s.size()));
    write_bytes(buf, s.data(), s.size());
}

// Little-endian binary readers.
struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;

    [[nodiscard]] bool at_end() const noexcept { return pos >= data.size(); }

    [[nodiscard]] std::expected<uint32_t, std::string> read_u32() {
        if (pos + 4 > data.size()) return std::unexpected("unexpected EOF reading u32");
        uint32_t v = data[pos] | (data[pos + 1] << 8) |
                     (data[pos + 2] << 16) | (data[pos + 3] << 24);
        pos += 4;
        return v;
    }

    [[nodiscard]] std::expected<uint64_t, std::string> read_u64() {
        if (pos + 8 > data.size()) return std::unexpected("unexpected EOF reading u64");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
        }
        pos += 8;
        return v;
    }

    [[nodiscard]] std::expected<uint8_t, std::string> read_u8() {
        if (pos >= data.size()) return std::unexpected("unexpected EOF reading u8");
        return data[pos++];
    }

    [[nodiscard]] std::expected<std::vector<uint8_t>, std::string>
    read_bytes(size_t n) {
        if (pos + n > data.size()) return std::unexpected("unexpected EOF reading bytes");
        std::vector<uint8_t> out(data.begin() + pos, data.begin() + pos + n);
        pos += n;
        return out;
    }

    [[nodiscard]] std::expected<std::string, std::string> read_string() {
        auto len = read_u32();
        if (!len) return std::unexpected(len.error());
        auto bytes = read_bytes(*len);
        if (!bytes) return std::unexpected(bytes.error());
        return std::string(bytes->begin(), bytes->end());
    }
};

}  // namespace

[[nodiscard]] std::expected<void, std::string>
write_replay(const ReplayJob& job, std::string_view path) {
    std::vector<uint8_t> buf;
    buf.reserve(1024 + job.bytecode.size());

    write_u32(buf, kMagic);
    write_u32(buf, kVersion);

    // Bytecode.
    write_u32(buf, static_cast<uint32_t>(job.bytecode.size()));
    write_bytes(buf, job.bytecode.data(), job.bytecode.size());

    // Constants.
    write_u32(buf, static_cast<uint32_t>(job.const_types.size()));
    for (size_t i = 0; i < job.const_types.size(); ++i) {
        write_u8(buf, job.const_types[i]);
        write_u64(buf, static_cast<uint64_t>(job.const_values[i]));
    }

    // max_locals.
    write_u32(buf, static_cast<uint32_t>(job.max_locals));

    // Name.
    write_string(buf, job.name);

    // Options.
    write_u32(buf, job.options.opt_level);
    write_u32(buf, job.options.max_inline);
    write_u8(buf, job.options.gvn_enabled ? 1 : 0);
    write_u8(buf, job.options.constfold_enabled ? 1 : 0);
    write_u8(buf, job.options.dce_enabled ? 1 : 0);
    write_u8(buf, job.options.escape_analysis ? 1 : 0);
    write_u64(buf, job.options.rng_seed);

    std::ofstream f(std::string(path), std::ios::binary);
    if (!f) return std::unexpected(std::format("cannot open {} for writing", path));
    f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    if (!f) return std::unexpected("write failed");
    return {};
}

[[nodiscard]] std::expected<ReplayJob, std::string>
read_replay(std::string_view path) {
    std::ifstream f(std::string(path), std::ios::binary);
    if (!f) return std::unexpected(std::format("cannot open {} for reading", path));

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    Reader r{data};

    auto magic = r.read_u32();
    if (!magic) return std::unexpected(magic.error());
    if (*magic != kMagic) return std::unexpected("bad magic — not a replay file");

    auto version = r.read_u32();
    if (!version) return std::unexpected(version.error());
    if (*version != kVersion) return std::unexpected(std::format("unsupported version {}", *version));

    ReplayJob job;

    auto bc_len = r.read_u32();
    if (!bc_len) return std::unexpected(bc_len.error());
    auto bc = r.read_bytes(*bc_len);
    if (!bc) return std::unexpected(bc.error());
    job.bytecode = std::move(*bc);

    auto cc = r.read_u32();
    if (!cc) return std::unexpected(cc.error());
    for (uint32_t i = 0; i < *cc; ++i) {
        auto type = r.read_u8();
        if (!type) return std::unexpected(type.error());
        auto val = r.read_u64();
        if (!val) return std::unexpected(val.error());
        job.const_types.push_back(*type);
        job.const_values.push_back(static_cast<int64_t>(*val));
    }

    auto ml = r.read_u32();
    if (!ml) return std::unexpected(ml.error());
    job.max_locals = static_cast<int>(*ml);

    auto name = r.read_string();
    if (!name) return std::unexpected(name.error());
    job.name = std::move(*name);

    auto ol = r.read_u32();
    if (!ol) return std::unexpected(ol.error());
    job.options.opt_level = *ol;

    auto mi = r.read_u32();
    if (!mi) return std::unexpected(mi.error());
    job.options.max_inline = *mi;

    auto gvn = r.read_u8();
    if (!gvn) return std::unexpected(gvn.error());
    job.options.gvn_enabled = *gvn != 0;

    auto cf = r.read_u8();
    if (!cf) return std::unexpected(cf.error());
    job.options.constfold_enabled = *cf != 0;

    auto dce = r.read_u8();
    if (!dce) return std::unexpected(dce.error());
    job.options.dce_enabled = *dce != 0;

    auto ea = r.read_u8();
    if (!ea) return std::unexpected(ea.error());
    job.options.escape_analysis = *ea != 0;

    auto seed = r.read_u64();
    if (!seed) return std::unexpected(seed.error());
    job.options.rng_seed = *seed;

    return job;
}

[[nodiscard]] ReplayJob
make_replay_job(const Chunk& chunk, std::string_view name, CompileOptions opts) {
    ReplayJob job;
    job.bytecode.assign(chunk.code().begin(), chunk.code().end());
    job.max_locals = chunk.max_locals();
    job.name = std::string(name);
    job.options = opts;

    // Serialize constants — only Number types are preserved.
    for (Object* c : chunk.constants()) {
        if (!c) {
            job.const_types.push_back(static_cast<uint8_t>(ObjType::Null));
            job.const_values.push_back(0);
        } else if (c->type == ObjType::NumberInt) {
            job.const_types.push_back(static_cast<uint8_t>(ObjType::NumberInt));
            job.const_values.push_back(cast_to<Number>(c)->as.i);
        } else if (c->type == ObjType::NumberFloat) {
            job.const_types.push_back(static_cast<uint8_t>(ObjType::NumberFloat));
            int64_t bits;
            std::memcpy(&bits, &cast_to<Number>(c)->as.f, sizeof(double));
            job.const_values.push_back(bits);
        } else {
            // Other types (String, Function, etc.) — store type only, value 0.
            job.const_types.push_back(static_cast<uint8_t>(c->type));
            job.const_values.push_back(0);
        }
    }

    return job;
}

[[nodiscard]] Chunk
chunk_from_replay(const ReplayJob& job) {
    Chunk c;
    c.set_max_locals(job.max_locals);
    c.set_filename(job.name);

    // Reconstruct the bytecode.
    for (uint8_t b : job.bytecode) {
        c.emit_byte(b);
    }

    // Reconstruct the constant pool — only Number types.
    // Note: these leak (intentional for replay — process lifetime).
    static std::vector<std::unique_ptr<Number>> pool;
    for (size_t i = 0; i < job.const_types.size(); ++i) {
        auto type = static_cast<ObjType>(job.const_types[i]);
        if (type == ObjType::NumberInt) {
            auto n = std::make_unique<Number>();
            n->base.type = ObjType::NumberInt;
            n->base.ref_count = 1;
            n->base.is_static = true;
            n->as.i = job.const_values[i];
            c.add_const(reinterpret_cast<Object*>(n.get()));
            pool.push_back(std::move(n));
        } else if (type == ObjType::NumberFloat) {
            auto n = std::make_unique<Number>();
            n->base.type = ObjType::NumberFloat;
            n->base.ref_count = 1;
            n->base.is_static = true;
            double f;
            int64_t bits = job.const_values[i];
            std::memcpy(&f, &bits, sizeof(double));
            n->as.f = f;
            c.add_const(reinterpret_cast<Object*>(n.get()));
            pool.push_back(std::move(n));
        } else if (type == ObjType::Null) {
            c.add_const(nullptr);
        } else {
            // Other types — can't reconstruct, store nullptr.
            c.add_const(nullptr);
        }
    }

    return c;
}

[[nodiscard]] std::string
save_replay_on_failure(const Chunk& chunk, std::string_view name,
                        std::string_view reason,
                        std::string_view replay_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(replay_dir);

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    std::string filename = std::format("{}_{}.replay", name, ts);
    fs::path path = fs::path(replay_dir) / filename;

    ReplayJob job = make_replay_job(chunk, name);
    auto r = write_replay(job, path.string());
    if (!r) {
        std::println(stderr, "Failed to save replay: {}", r.error());
        return "";
    }

    // Also write a sidecar with the failure reason.
    fs::path reason_path = path;
    reason_path.replace_extension(".reason");
    std::ofstream rf(reason_path);
    rf << reason;

    return path.string();
}

}  // namespace arcjit
