cmake_minimum_required(VERSION 3.28)

project(arcJIT
    VERSION 0.1.0
    DESCRIPTION "A 3-tier JIT for the Arc programming language"
    LANGUAGES CXX
)

# --- Global C++23 setup -------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- Options -----------------------------------------------------------------
option(ARCJIT_BUILD_TESTS       "Build unit tests"                    ON)
option(ARCJIT_BUILD_EXAMPLES    "Build example programs"              ON)
option(ARCJIT_ENABLE_LTO        "Enable link-time optimization"       ON)
option(ARCJIT_WARNINGS_AS_ERRORS "Treat warnings as errors"           ON)
option(ARCJIT_USE_SANITIZERS    "Enable ASan/UBSan in Debug"          OFF)

# --- FetchContent deps -------------------------------------------------------
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# asmjit — x86-64 code generation
FetchContent_Declare(
    asmjit
    GIT_REPOSITORY https://github.com/asmjit/asmjit.git
    GIT_TAG        0bd5787b54b575ed94bf32ac452153b34385c514  # master snapshot, pinned
    GIT_SHALLOW    TRUE
)

# enkiTS — work-stealing task scheduler
FetchContent_Declare(
    enkits
    GIT_REPOSITORY https://github.com/dougbinks/enkiTS.git
    GIT_TAG        v1.12  # latest stable tag
    GIT_SHALLOW    TRUE
)

# We disable ASMJIT_EMBED so asmjit builds its own static library with its own
# compile flags (asmjit uses anonymous structs that trip -Wpedantic).
set(ASMJIT_EMBED OFF CACHE BOOL "" FORCE)
set(ASMJIT_STATIC ON  CACHE BOOL "" FORCE)
set(ENKITS_BUILD_EXAMPLE  OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ENKITS_TASK_PRIORITY_SCHEDULER ON CACHE BOOL "" FORCE)
# Force static lib for enkiTS so we don't need to ship a .so.
set(ENKITS_LIB_TYPE STATIC CACHE STRING "" FORCE)

FetchContent_MakeAvailable(asmjit enkits)

# --- Compiler flags ----------------------------------------------------------
set(ARCJIT_WARNING_FLAGS
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wno-gnu-zero-variadic-macro-arguments
)

if(ARCJIT_WARNINGS_AS_ERRORS)
    list(APPEND ARCJIT_WARNING_FLAGS -Werror)
endif()

# JIT proper: no exceptions, no RTTI
set(ARCJIT_JIT_FLAGS -fno-exceptions -fno-rtti)

# --- Helper: arcjit_jit_library ----------------------------------------------
function(arcjit_jit_library target)
    add_library(${target} ${ARGN})
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_options(${target} PRIVATE
        ${ARCJIT_WARNING_FLAGS}
        ${ARCJIT_JIT_FLAGS}
    )
    target_link_libraries(${target} PUBLIC asmjit::asmjit enkits::enkits)
endfunction()

# --- Helper: arcjit_runtime_library ------------------------------------------
function(arcjit_runtime_library target)
    add_library(${target} ${ARGN})
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_options(${target} PRIVATE ${ARCJIT_WARNING_FLAGS})
    target_link_libraries(${target} PUBLIC asmjit::asmjit enkits::enkits)
endfunction()

# --- Subdirs -----------------------------------------------------------------
add_subdirectory(src)

if(ARCJIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ARCJIT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Install -----------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS arcjit_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
