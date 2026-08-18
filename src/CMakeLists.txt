# arcJIT — src/CMakeLists.txt
#
# All libraries apply: -fno-exceptions -fno-rtti -march=native -flto (the
# JIT-flags + arch + LTO helpers from the top-level CMakeLists.txt). The
# runtime library used to be built with exceptions+RTTI enabled, which
# silently paid for unwinding tables and typeinfo on every Value copy and
# std::expected construction on the hot path — fixed.

# Common compile-options for arcjit targets (warnings + JIT flags + arch + LTO).
set(ARCJIT_LIB_FLAGS
    ${ARCJIT_WARNING_FLAGS}
    ${ARCJIT_JIT_FLAGS}
    ${ARCJIT_ARCH_FLAGS}
    ${ARCJIT_LTO_FLAGS}
)

# Helper to apply standard arcjit lib settings.
function(arcjit_lib_settings target)
    target_include_directories(${target} PUBLIC .)
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_options(${target} PRIVATE ${ARCJIT_LIB_FLAGS})
    target_link_options(${target} PRIVATE ${ARCJIT_LTO_FLAGS})
endfunction()

# Helper for executables (benchmarks, CLI, profilers).
function(arcjit_exe_settings target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_options(${target} PRIVATE
        ${ARCJIT_WARNING_FLAGS}
        ${ARCJIT_JIT_FLAGS}
        ${ARCJIT_ARCH_FLAGS}
        ${ARCJIT_LTO_FLAGS}
        -Wno-error  # tools may use features that trip pedantic
    )
    target_link_options(${target} PRIVATE ${ARCJIT_LTO_FLAGS})
endfunction()

# --- Bytecode library (mirrors Arc's headers) -------------------------------
add_library(arcjit_bytecode STATIC
    bytecode/chunk.cpp
    bytecode/value.cpp
)
arcjit_lib_settings(arcjit_bytecode)

# --- Machinery (Watchdog, Meter, Probe, Regulator, Fuse, Trip, Capacitor, Relay, GroundFault, Governor)
add_library(arcjit_machinery STATIC
    machinery/watchdog.cpp
    machinery/meter.cpp
    machinery/probe.cpp
    machinery/regulator.cpp
    machinery/fuse.cpp
    machinery/trip.cpp
    machinery/capacitor.cpp
    machinery/relay.cpp
    machinery/ground_fault.cpp
    machinery/governor.cpp
)
arcjit_lib_settings(arcjit_machinery)

# --- Core IR library (Node, Graph, Arena, IR dumper, Verifier, Replay, Dominance, Bitmask)
add_library(arcjit_core STATIC
    core/node.cpp
    core/graph.cpp
    core/ir_dump.cpp
    core/verifier.cpp
    core/replay.cpp
    core/dominance.cpp
    core/bitmask.cpp
)
arcjit_lib_settings(arcjit_core)
target_link_libraries(arcjit_core PUBLIC arcjit_bytecode)

# --- Pass manager (with instrumentation) ------------------------------------
add_library(arcjit_passman STATIC
    passman/pass.cpp
    passman/instrument.cpp
)
arcjit_lib_settings(arcjit_passman)
target_link_libraries(arcjit_passman PUBLIC arcjit_core)

# --- Tier 1 baseline SSA JIT ------------------------------------------------
add_library(arcjit_tier1 STATIC
    tier1/tier1.cpp
)
arcjit_lib_settings(arcjit_tier1)
target_link_libraries(arcjit_tier1 PUBLIC
    arcjit_core
    arcjit_bytecode
    asmjit::asmjit
    enkiTS
)

# --- Tier 2 Sea of Nodes JIT -------------------------------------------------
add_library(arcjit_tier2 STATIC
    tier2/tier2.cpp
)
arcjit_lib_settings(arcjit_tier2)
target_link_libraries(arcjit_tier2 PUBLIC
    arcjit_core
    arcjit_passman
    arcjit_tier1
    asmjit::asmjit
)

# --- Runtime (orchestrator, interpreter, thread pool, deopt validator, fuzzer)
add_library(arcjit_runtime STATIC
    runtime/runtime.cpp
    runtime/deopt_validator.cpp
    runtime/fuzzer.cpp
)
arcjit_lib_settings(arcjit_runtime)
target_link_libraries(arcjit_runtime PUBLIC
    arcjit_core
    arcjit_bytecode
    arcjit_passman
    arcjit_tier1
    arcjit_tier2
    arcjit_machinery
    enkiTS
)

# --- CLI --------------------------------------------------------------------
add_executable(arcjit_cli main.cpp)
arcjit_exe_settings(arcjit_cli)
target_link_libraries(arcjit_cli PRIVATE arcjit_runtime)
set_target_properties(arcjit_cli PROPERTIES OUTPUT_NAME arcjit-cli)

# --- Benchmark (compile-once-then-execute, used by arcjit-bench) ------------
add_executable(arcjit_bench bench.cpp)
arcjit_exe_settings(arcjit_bench)
target_link_libraries(arcjit_bench PRIVATE arcjit_runtime)
set_target_properties(arcjit_bench PROPERTIES OUTPUT_NAME arcjit-bench)

# --- Overhead Profiler ------------------------------------------------------
add_executable(arcjit_profiler overhead_profiler.cpp)
arcjit_exe_settings(arcjit_profiler)
target_link_libraries(arcjit_profiler PRIVATE arcjit_runtime)
set_target_properties(arcjit_profiler PROPERTIES OUTPUT_NAME arcjit-profiler)

# --- Overhead Breakdown Profiler -------------------------------------------
add_executable(arcjit_breakdown overhead_breakdown_profiler.cpp)
arcjit_exe_settings(arcjit_breakdown)
target_link_libraries(arcjit_breakdown PRIVATE arcjit_runtime)
set_target_properties(arcjit_breakdown PROPERTIES OUTPUT_NAME arcjit-breakdown)

# --- Gigavolt Diagnostic ----------------------------------------------------
add_executable(arcjit_diag gigavolt_diag.cpp)
arcjit_exe_settings(arcjit_diag)
target_link_libraries(arcjit_diag PRIVATE arcjit_runtime)
set_target_properties(arcjit_diag PROPERTIES OUTPUT_NAME arcjit-diag)

# --- Benchmark Suite --------------------------------------------------------
add_executable(arcjit_suite bench_suite.cpp)
arcjit_exe_settings(arcjit_suite)
target_link_libraries(arcjit_suite PRIVATE arcjit_runtime)
set_target_properties(arcjit_suite PROPERTIES OUTPUT_NAME arcjit-suite)

# --- Repro GVN segfault ----------------------------------------------------
