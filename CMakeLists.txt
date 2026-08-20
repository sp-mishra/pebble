cmake_minimum_required(VERSION 4.1)
project(pebble LANGUAGES C CXX)

# Set the default C++ standard to C++26
set(PROJECT_NAME pebble)

# Set the default C standard
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# C++26 configuration
# We check if the compiler supports -std=c++26 or -std=c++2c directly.
# We keep CMAKE_CXX_STANDARD at 23 for CMake internal targets / try_compile
# so CMake doesn't error out looking for an "AppleClang CXX26" dialect entry,
# while injecting the C++26 flag into compile options.
# ---------------------------------------------------------------------------
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++26" _pebble_have_cxx26)
if(NOT _pebble_have_cxx26)
    check_cxx_compiler_flag("-std=c++2c" _pebble_have_cxx2c)
endif()

if(_pebble_have_cxx26)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++26>)
    message(STATUS "[pebble] C++26 enabled: using -std=c++26")
elseif(_pebble_have_cxx2c)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++2c>)
    message(STATUS "[pebble] C++26 enabled: using -std=c++2c")
else()
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    message(STATUS "[pebble] Compiler does not support C++26 — falling back to C++23")
endif()
set(CMAKE_VERBOSE_MAKEFILE ON)

set(BUILD_EXECUTABLE ON)
set(BUILD_EXEMAIN "exemain")
set(BUILD_SHARED_LIBS OFF)
set(BUILD_SHARED OFF)

# ---------------------------------------------------------------------------
# Top-level detection
# When pebble is consumed via add_subdirectory() by another CMake project,
# CMAKE_CURRENT_SOURCE_DIR != CMAKE_SOURCE_DIR.  We use this to flip the
# default for BUILD_TESTS so that a parent project never accidentally builds
# our test targets.  The parent can still opt-in with -DPEBBLE_BUILD_TESTS=ON.
# ---------------------------------------------------------------------------
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(PEBBLE_IS_TOP_LEVEL ON)
else()
    set(PEBBLE_IS_TOP_LEVEL OFF)
endif()

option(BUILD_TESTS "Build pebble's tests (auto-disabled when used as a subdirectory)" ${PEBBLE_IS_TOP_LEVEL})
option(BUILD_APP   "Build the application" OFF)

if (PEBBLE_IS_TOP_LEVEL)
    message(STATUS "[pebble] Top-level build — BUILD_TESTS default: ${BUILD_TESTS}")
else()
    message(STATUS "[pebble] Subdirectory build — BUILD_TESTS default: ${BUILD_TESTS} (tests suppressed)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Catch2
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "disable building tests" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "disable building examples" FORCE)
set(CATCH_BUILD_FUZZERS OFF CACHE BOOL "disable building fuzzers" FORCE)

message("CMAKE_SOURCE_DIR location: ${CMAKE_SOURCE_DIR}")
include(FetchContent)
include(ExternalProject)

# ---------------------------------------------------------------------------
# Dependency bootstrapping via fetch_deps.sh
# ---------------------------------------------------------------------------
# option: set -DDEPS_VERBOSE=OFF to silence dependency output during configure
option(DEPS_VERBOSE "Show verbose dependency fetch output" ON)

set(FETCH_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/fetch_deps.sh")

if (NOT EXISTS "${FETCH_SCRIPT}")
    message(FATAL_ERROR
        "Dependency fetch script not found: ${FETCH_SCRIPT}\n"
        "Please ensure scripts/fetch_deps.sh is present in the repository."
    )
endif()

if (DEPS_VERBOSE)
    message(STATUS "[deps] ════════════════════════════════════════")
    message(STATUS "[deps] Checking dependencies in: ${CMAKE_SOURCE_DIR}/dependencies")
    message(STATUS "[deps] ════════════════════════════════════════")
endif()

# Map of library-key -> expected folder under dependencies/
set(PEBBLE_DEP_FOLDERS
    Catch2   "Catch2-3.9.0"
    crc32c   "crc32c"
    glaze    "glaze"
    liblmdb  "liblmdb"
    highway  "highway"
    spdlog   "spdlog"
)

set(MISSING_DEPS "")
list(LENGTH PEBBLE_DEP_FOLDERS _pair_count)
math(EXPR _pair_last "${_pair_count} - 1")

foreach(_idx RANGE 0 ${_pair_last} 2)
    math(EXPR _fidx "${_idx} + 1")
    list(GET PEBBLE_DEP_FOLDERS ${_idx}  _dep_key)
    list(GET PEBBLE_DEP_FOLDERS ${_fidx} _dep_folder)
    set(_dep_path "${CMAKE_SOURCE_DIR}/dependencies/${_dep_folder}")
    if (EXISTS "${_dep_path}")
        if (DEPS_VERBOSE)
            message(STATUS "[deps]   OK       ${_dep_key} → ${_dep_path}")
        endif()
    else()
        message(STATUS "[deps]   MISSING  ${_dep_key} → ${_dep_path}")
        list(APPEND MISSING_DEPS "${_dep_key}")
    endif()
endforeach()

if (MISSING_DEPS)
    list(LENGTH MISSING_DEPS _missing_count)
    message(STATUS "[deps] ────────────────────────────────────────")
    message(STATUS "[deps] ${_missing_count} missing — invoking fetch_deps.sh")
    message(STATUS "[deps] ────────────────────────────────────────")

    # Build command — pass --quiet when DEPS_VERBOSE is OFF
    set(_fetch_cmd bash "${FETCH_SCRIPT}")
    if (NOT DEPS_VERBOSE)
        list(APPEND _fetch_cmd "--quiet")
    endif()
    list(APPEND _fetch_cmd ${MISSING_DEPS})

    execute_process(
        COMMAND         ${_fetch_cmd}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _fetch_result
        OUTPUT_VARIABLE _fetch_stdout
        ERROR_VARIABLE  _fetch_stderr
        ECHO_OUTPUT_VARIABLE    # stream script output live to the configure log
        ECHO_ERROR_VARIABLE     # stream script errors live to the configure log
    )

    if (NOT _fetch_result EQUAL 0)
        message(FATAL_ERROR
            "[deps] fetch_deps.sh exited with code ${_fetch_result}.\n"
            "stdout:\n${_fetch_stdout}\n"
            "stderr:\n${_fetch_stderr}"
        )
    endif()

    message(STATUS "[deps] ════════════════════════════════════════")
    message(STATUS "[deps] All missing dependencies fetched successfully.")
    message(STATUS "[deps] ════════════════════════════════════════")
else()
    message(STATUS "[deps] ────────────────────────────────────────")
    message(STATUS "[deps] All dependencies already present — no download needed.")
    message(STATUS "[deps] ────────────────────────────────────────")
endif()
include_directories(/usr/local/include)
include_directories(${HOME_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/dependencies)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/Catch2-3.9.0/extras)
include_directories(include)
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/highway)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/glaze/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/liblmdb)

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BUILD_EXAMPLES OFF)
set(BUILD_SHARED_LIBS OFF)

# Spdlog
set(SPDLOG_BUILD_EXAMPLES OFF)
set(SPDLOG_USE_STD_FORMAT ON)
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_ENABLE_PCH OFF)
set(SPDLOG_BUILD_EXAMPLE OFF)
set(SPDLOG_BUILD_TESTS OFF)
set(SPDLOG_BUILD_BENCH OFF)

# highway
set(HWY_TEST_STANDALONE OFF CACHE BOOL "Disable use of googletest" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "Install library" FORCE)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "Enable HWY tests" FORCE)
set(HWY_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/dependencies/spdlog)
add_subdirectory(${CMAKE_SOURCE_DIR}/dependencies/highway)

# crc32c
set(CRC32C_BUILD_TESTS OFF CACHE BOOL "Build CRC32C's unit tests" FORCE)
set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "Build CRC32C's benchmarks" FORCE)
set(CRC32C_USE_GLOG OFF CACHE BOOL "Build CRC32C's tests with Google Logging" FORCE)
set(CRC32C_INSTALL OFF CACHE BOOL "Install CRC32C's header and library" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/dependencies/crc32c)

# liblmdb
if (EXISTS "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/mdb.c")
    set(LMDB_SOURCE
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/mdb.c"
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/midl.c"
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/lmdb.h"
            "${CMAKE_SOURCE_DIR}/dependencies/liblmdb/midl.h"
    )
    add_library(lmdb STATIC ${LMDB_SOURCE})
    target_include_directories(lmdb PUBLIC "${CMAKE_SOURCE_DIR}/dependencies/liblmdb")
    target_link_libraries(lmdb PUBLIC Threads::Threads)
endif()

find_package(Threads REQUIRED)

link_directories(/usr/local/lib)
link_directories(${HOME_LIB_DIR})

# Separate real compilable source files (C/C++) from headers so we can
# avoid creating an executable target with only headers (which causes
# "No SOURCES given to target" errors).
#set(APP_SOURCES_CPP)
#foreach(_f IN LISTS APP_SOURCE)
#    if(_f MATCHES "\\.(c|cc|cpp|cxx)$")
#        list(APPEND APP_SOURCES_CPP "${_f}")
#    endif()
#endforeach()

SET(CATCH2_SOURCE
        "${CMAKE_SOURCE_DIR}/dependencies/Catch2-3.9.0/extras/catch_amalgamated.cpp"
        "${CMAKE_SOURCE_DIR}/dependencies/Catch2-3.9.0/extras/catch_amalgamated.hpp"
)
add_library(Catch2 STATIC ${CATCH2_SOURCE})

if (BUILD_EXECUTABLE)
    file(GLOB APP_SOURCE
            "${CMAKE_SOURCE_DIR}/src/examples/*.cpp"
            "${CMAKE_SOURCE_DIR}/src/examples/*.hpp"
    )

    add_executable(${PROJECT_NAME} ${APP_SOURCE})
    target_link_libraries(${PROJECT_NAME} PRIVATE
            spdlog::spdlog_header_only
            hwy
            crc32c
    )
    if (TARGET lmdb)
        target_link_libraries(${PROJECT_NAME} PRIVATE lmdb)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        TURBO_TWIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()


if (BUILD_TESTS)
    file(
            GLOB TEST_SOURCE
            "${CMAKE_SOURCE_DIR}/src/tests/*.cpp"
    )
    add_executable(pebble_tests ${TEST_SOURCE})
    target_link_libraries(pebble_tests PRIVATE
            Catch2
            spdlog::spdlog_header_only
            hwy
            crc32c
    )
    if (TARGET lmdb)
        target_link_libraries(pebble_tests PRIVATE lmdb)
    endif()
    target_compile_definitions(pebble_tests PRIVATE
        TURBO_TWIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif ()