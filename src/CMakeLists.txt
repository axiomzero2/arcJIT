# arcJIT — src/CMakeLists.txt

# --- Bytecode library (mirrors Arc's headers) -------------------------------
add_library(arcjit_bytecode STATIC
    bytecode/chunk.cpp
    bytecode/value.cpp
)
target_include_directories(arcjit_bytecode PUBLIC .)
target_compile_features(arcjit_bytecode PUBLIC cxx_std_23)
target_compile_options(arcjit_bytecode PRIVATE -Wall -Wextra -Wpedantic)

# --- Core IR library (Node, Graph, Arena, IR dumper, Verifier, Replay) ------
add_library(arcjit_core STATIC
    core/node.cpp
    core/graph.cpp
    core/ir_dump.cpp
    core/verifier.cpp
    core/replay.cpp
)
target_include_directories(arcjit_core PUBLIC .)
target_compile_features(arcjit_core PUBLIC cxx_std_23)
target_compile_options(arcjit_core PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(arcjit_core PUBLIC arcjit_bytecode)

# --- Pass manager (with instrumentation) ------------------------------------
add_library(arcjit_passman STATIC
    passman/pass.cpp
    passman/instrument.cpp
)
target_include_directories(arcjit_passman PUBLIC .)
target_compile_features(arcjit_passman PUBLIC cxx_std_23)
target_compile_options(arcjit_passman PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(arcjit_passman PUBLIC arcjit_core)

# --- Tier 1 baseline SSA JIT ------------------------------------------------
add_library(arcjit_tier1 STATIC
    tier1/tier1.cpp
)
target_include_directories(arcjit_tier1 PUBLIC .)
target_compile_features(arcjit_tier1 PUBLIC cxx_std_23)
target_compile_options(arcjit_tier1 PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror)
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
target_include_directories(arcjit_tier2 PUBLIC .)
target_compile_features(arcjit_tier2 PUBLIC cxx_std_23)
target_compile_options(arcjit_tier2 PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror)
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
target_include_directories(arcjit_runtime PUBLIC .)
target_compile_features(arcjit_runtime PUBLIC cxx_std_23)
target_compile_options(arcjit_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(arcjit_runtime PUBLIC
    arcjit_core
    arcjit_bytecode
    arcjit_passman
    arcjit_tier1
    arcjit_tier2
    enkiTS
)

# --- CLI --------------------------------------------------------------------
add_executable(arcjit_cli main.cpp)
target_compile_features(arcjit_cli PUBLIC cxx_std_23)
target_compile_options(arcjit_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(arcjit_cli PRIVATE arcjit_runtime)
set_target_properties(arcjit_cli PROPERTIES OUTPUT_NAME arcjit-cli)
