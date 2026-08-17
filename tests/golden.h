// SPDX-License-Identifier: MIT
// arcJIT — Golden test harness.
//
// Per Rule 37, every pass must have ≥10 golden IR tests. Golden tests are
// .in.ir / .out.ir file pairs under tests/golden/<pass_name>/. The runner
// builds a graph from the .in.ir file, runs the specified pass, and compares
// the output against the .out.ir file.
//
// Usage in tests:
//   GoldenTest::check("gVN/fold_duplicate_add", []() {
//       // build graph, run GVN, compare
//   });
//
// Or via the helper macro:
//   ARCJIT_GOLDEN_TEST("gVN", "fold_duplicate_add") {
//       Graph g;
//       // ... build input ...
//       return g;
//   }
#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>

#include "core/graph.h"
#include "core/ir_dump.h"

namespace arcjit {

// Read a file's contents into a string.
[[nodiscard]] inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Write a string to a file (creates parent dirs).
inline void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

// Compare two IR dump strings. Returns empty string if equal, else a diff.
[[nodiscard]] inline std::string diff_ir(std::string_view actual,
                                          std::string_view expected) {
    if (actual == expected) return "";

    std::string out;
    out += "IR mismatch:\n";
    out += "--- expected ---\n";
    out += expected;
    out += "--- actual ---\n";
    out += actual;
    out += "--- end ---\n";
    return out;
}

// The golden test root. Override at runtime with ARCJIT_GOLDEN_DIR env var.
[[nodiscard]] inline std::filesystem::path golden_dir() {
    if (const char* env = std::getenv("ARCJIT_GOLDEN_DIR")) {
        return env;
    }
    // Default: tests/golden/ relative to CWD.
    return "tests/golden";
}

// Check a single golden test case.
//
// `pass_name` is the subdirectory under tests/golden/.
// `case_name` is the file stem (e.g. "fold_add_zero" → fold_add_zero.in.ir / .out.ir).
// `build_input` constructs the input graph.
// `run_pass` runs the pass under test and returns the modified graph.
//
// Returns true if the test passed (or if --update-golden was set and the
// golden file was updated).
//
// Set ARCJIT_UPDATE_GOLDEN=1 in the environment to update golden files.
[[nodiscard]] inline bool check_golden(
    std::string_view pass_name,
    std::string_view case_name,
    std::function<Graph()> build_input,
    std::function<void(Graph&)> run_pass) {

    auto base = golden_dir() / pass_name / case_name;
    auto in_path  = base;
    in_path += ".in.ir";
    auto out_path = base;
    out_path += ".out.ir";

    Graph g = build_input();
    run_pass(g);
    std::string actual = dump_graph_text(g);

    // Check if we should update goldens.
    if (const char* env = std::getenv("ARCJIT_UPDATE_GOLDEN")) {
        if (std::string(env) == "1") {
            write_file(out_path, actual);
            return true;
        }
    }

    // If the golden doesn't exist yet, create it (first run).
    if (!std::filesystem::exists(out_path)) {
        write_file(out_path, actual);
        return true;
    }

    std::string expected = read_file(out_path);
    std::string diff = diff_ir(actual, expected);
    if (!diff.empty()) {
        std::fprintf(stderr, "Golden test '%.*s/%.*s' FAILED:\n%s",
                     static_cast<int>(pass_name.size()), pass_name.data(),
                     static_cast<int>(case_name.size()), case_name.data(),
                     diff.c_str());
        return false;
    }
    return true;
}

}  // namespace arcjit
