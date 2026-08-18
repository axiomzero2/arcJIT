# arcJIT — tests/CMakeLists.txt

find_package(Git QUIET)

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(arcjit_tests
    test_value.cpp
    test_bytecode.cpp
    test_node.cpp
    test_graph.cpp
    test_passman.cpp
    test_tier1.cpp
    test_tier2.cpp
    test_interp.cpp
    test_runtime.cpp
    test_verifier.cpp
    test_ir_dump.cpp
    test_differential.cpp
    test_differential_impl.cpp
    golden/test_golden_gvn.cpp
    golden/test_golden_constfold.cpp
    golden/test_golden_dce.cpp
    golden/test_golden_algebraic_simp.cpp
    golden/test_golden_compare_fold.cpp
    golden/test_golden_branch_fold.cpp
    golden/test_golden_type_narrow.cpp
    golden/test_golden_strength_reduce.cpp
    golden/test_golden_licm.cpp
    golden/test_golden_escape_analysis.cpp
    golden/test_golden_bce.cpp
    test_rule36_branch_bug.cpp
    test_replay.cpp
    test_instrumentation.cpp
    test_deopt_validator.cpp
    test_fuzzer.cpp
    test_dominance.cpp
    test_machinery.cpp
    test_machinery_wiring.cpp
    test_ground_fault_governor.cpp
    test_bitmask.cpp
)
target_include_directories(arcjit_tests PRIVATE .)
target_compile_features(arcjit_tests PUBLIC cxx_std_23)
target_compile_options(arcjit_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
target_link_libraries(arcjit_tests PRIVATE
    arcjit_runtime
    arcjit_machinery
    GTest::gtest_main
)

# Pass the golden test directory via env var so tests can find .ir files.
gtest_discover_tests(arcjit_tests
    PROPERTIES
    ENVIRONMENT "ARCJIT_GOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/golden"
)
