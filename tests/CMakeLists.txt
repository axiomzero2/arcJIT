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
)
target_compile_features(arcjit_tests PUBLIC cxx_std_23)
target_compile_options(arcjit_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
target_link_libraries(arcjit_tests PRIVATE
    arcjit_runtime
    GTest::gtest_main
)
gtest_discover_tests(arcjit_tests)
