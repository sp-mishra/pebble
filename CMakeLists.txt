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
option(PEBBLE_ENABLE_KALPANA_SOKOL_BACKEND "Enable kalpana::sokol_backend API" ON)
option(PEBBLE_ENABLE_KALPANA_NOTCURSES_BACKEND "Enable kalpana::notcurses_backend API" ON)
option(PEBBLE_ENABLE_PRAVAHA_EXTERNAL_ADAPTER_TESTS
       "Build Pravaha tests requiring downstream Lithe, Vulkan, or Sutra headers" OFF)

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
    Catch2    "Catch2-3.9.0"
    crc32c    "crc32c"
    glaze     "glaze"
    liblmdb   "liblmdb"
    highway   "highway"
    spdlog    "spdlog"
    z3        "z3"
    metal-cpp "metal-cpp"
    mlx       "mlx"
    sokol     "sokol"
    notcurses "notcurses"
    sqlite    "sqlite"
    libduckdb "libduckdb"
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
include_directories(${CMAKE_SOURCE_DIR}/dependencies/lexy/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/Catch2-3.9.0/extras)
include_directories(include)
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/highway)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/glaze/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/liblmdb)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/z3/include)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/z3)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/sokol)
include_directories(${CMAKE_SOURCE_DIR}/dependencies/notcurses/include)


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

# SQLite library integration
option(BUILD_SQLITE "Build with SQLite library support" ON)
set(HAS_SQLITE FALSE)
if(BUILD_SQLITE)
    set(SQLITE_DIR "${CMAKE_SOURCE_DIR}/dependencies/sqlite")
    if (EXISTS "${SQLITE_DIR}/CMakeLists.txt")
        add_subdirectory(${SQLITE_DIR} sqlite_build)
        set(HAS_SQLITE TRUE)
        add_compile_definitions(HAS_SQLITE=1)
        message(STATUS "[pebble] SQLite: Building library from dependencies/sqlite")
    elseif (EXISTS "${SQLITE_DIR}/sqlite3.c")
        add_library(sqlite3 STATIC "${SQLITE_DIR}/sqlite3.c")
        target_include_directories(sqlite3 PUBLIC "${SQLITE_DIR}")
        find_package(Threads REQUIRED)
        target_link_libraries(sqlite3 PRIVATE Threads::Threads)
        add_library(sqlite::sqlite ALIAS sqlite3)
        add_library(sqlite3::sqlite3 ALIAS sqlite3)
        set(HAS_SQLITE TRUE)
        add_compile_definitions(HAS_SQLITE=1)
        message(STATUS "[pebble] SQLite: Building library directly from dependencies/sqlite/sqlite3.c")
    endif()
endif()

# DuckDB library integration
option(BUILD_DUCKDB "Build with DuckDB library support" ON)
set(HAS_DUCKDB FALSE)
if(BUILD_DUCKDB)
    set(DUCKDB_DIR "${CMAKE_SOURCE_DIR}/dependencies/libduckdb")
    if (EXISTS "${DUCKDB_DIR}/CMakeLists.txt")
        add_subdirectory(${DUCKDB_DIR} duckdb_build)
        set(HAS_DUCKDB TRUE)
        add_compile_definitions(HAS_DUCKDB=1)
        message(STATUS "[pebble] DuckDB: Building library from dependencies/libduckdb (isolated to C++17)")
    elseif (EXISTS "${DUCKDB_DIR}/duckdb.cpp")
        add_library(duckdb STATIC "${DUCKDB_DIR}/duckdb.cpp")
        set_target_properties(duckdb PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_include_directories(duckdb PUBLIC "${DUCKDB_DIR}")
        find_package(Threads REQUIRED)
        target_link_libraries(duckdb PRIVATE Threads::Threads)
        add_library(duckdb::duckdb ALIAS duckdb)
        add_library(libduckdb::libduckdb ALIAS duckdb)
        set(HAS_DUCKDB TRUE)
        add_compile_definitions(HAS_DUCKDB=1)
        message(STATUS "[pebble] DuckDB: Building library directly from dependencies/libduckdb/duckdb.cpp (isolated to C++17)")
    endif()
endif()

# ===================== Z3 SMT Solver (optimized static library) =========================
option(BUILD_Z3 "Build with Z3 SMT solver support" ON)

set(HAS_Z3 FALSE)
if(BUILD_Z3)
    set(Z3_DIR ${CMAKE_SOURCE_DIR}/dependencies/z3)
    if(EXISTS ${Z3_DIR}/CMakeLists.txt)
        set(Z3_BUILD_LIBZ3_SHARED    OFF CACHE BOOL "Build Z3 as static library"         FORCE)
        set(Z3_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "Disable Z3 Python bindings"          FORCE)
        set(Z3_BUILD_DOCUMENTATION   OFF CACHE BOOL "Disable Z3 documentation"            FORCE)
        set(Z3_INCLUDE_GIT_HASH      OFF CACHE BOOL "Disable Z3 git hash in version"      FORCE)
        set(Z3_INCLUDE_GIT_DESCRIBE  OFF CACHE BOOL "Disable Z3 git describe in version"  FORCE)

        # Build Z3 optimized (Release) even when top-level build type is unset to ensure high performance
        set(Z3_SAVED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(CMAKE_BUILD_TYPE Release)
        endif()

        add_subdirectory(${Z3_DIR} z3_build)

        set(CMAKE_BUILD_TYPE "${Z3_SAVED_BUILD_TYPE}")
        
        # Expose C++ and C API headers
        target_include_directories(libz3 INTERFACE
            $<BUILD_INTERFACE:${Z3_DIR}/src/api/c++>
            $<BUILD_INTERFACE:${Z3_DIR}/src/api>
            $<BUILD_INTERFACE:${Z3_DIR}/include>
        )
        set(HAS_Z3 TRUE)
        add_compile_definitions(HAS_Z3=1)
        message(STATUS "[pebble] Z3: Building as optimized static library from dependencies/z3 (HAS_Z3=1)")
    else()
        # Fallback: check prebuilt static archives or system libs
        set(Z3_LIB "")
        if (EXISTS "${Z3_DIR}/libz3.a")
            set(Z3_LIB "${Z3_DIR}/libz3.a")
        elseif (EXISTS "${Z3_DIR}/lib/libz3.a")
            set(Z3_LIB "${Z3_DIR}/lib/libz3.a")
        elseif (EXISTS "${Z3_DIR}/libz3.dylib")
            set(Z3_LIB "${Z3_DIR}/libz3.dylib")
        elseif (EXISTS "${Z3_DIR}/lib/libz3.dylib")
            set(Z3_LIB "${Z3_DIR}/lib/libz3.dylib")
        elseif (EXISTS "${Z3_DIR}/libz3.so")
            set(Z3_LIB "${Z3_DIR}/libz3.so")
        elseif (EXISTS "${Z3_DIR}/lib/libz3.so")
            set(Z3_LIB "${Z3_DIR}/lib/libz3.so")
        endif()

        if (NOT Z3_LIB)
            find_library(Z3_SYSTEM_LIB NAMES z3 libz3 PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
            if (Z3_SYSTEM_LIB)
                set(Z3_LIB "${Z3_SYSTEM_LIB}")
            endif()
        endif()

        if (Z3_LIB)
            add_library(z3_imported UNKNOWN IMPORTED)
            set_target_properties(z3_imported PROPERTIES IMPORTED_LOCATION "${Z3_LIB}")
            if (EXISTS "${Z3_DIR}/include")
                set_target_properties(z3_imported PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Z3_DIR}/include")
            elseif (EXISTS "${Z3_DIR}/src/api/c++")
                set_target_properties(z3_imported PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Z3_DIR}/src/api/c++")
            elseif (EXISTS "${Z3_DIR}")
                set_target_properties(z3_imported PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Z3_DIR}")
            endif()
            set(HAS_Z3 TRUE)
            add_compile_definitions(HAS_Z3=1)
            message(STATUS "[pebble] Z3: Prebuilt library detected: ${Z3_LIB} (HAS_Z3=1)")
        endif()
    endif()
endif()

# ===================== MLX (Apple GPU/CPU ML framework) =======================
option(BUILD_MLX "Build with MLX support" ON)

set(HAS_MLX FALSE)
if(BUILD_MLX AND APPLE)
    set(MLX_DIR ${CMAKE_SOURCE_DIR}/dependencies/mlx)
    if(EXISTS ${MLX_DIR}/CMakeLists.txt)
        # MLX build configuration — static library, minimal footprint
        set(MLX_BUILD_TESTS OFF CACHE BOOL "Disable MLX tests" FORCE)
        set(MLX_BUILD_EXAMPLES OFF CACHE BOOL "Disable MLX examples" FORCE)
        set(MLX_BUILD_BENCHMARKS OFF CACHE BOOL "Disable MLX benchmarks" FORCE)
        set(MLX_BUILD_SAFETENSORS ON CACHE BOOL "Enable safetensors support" FORCE)
        set(MLX_BUILD_GGUF ON CACHE BOOL "Enable GGUF support" FORCE)
        set(MLX_BUILD_METAL ON CACHE BOOL "Enable Metal GPU backend" FORCE)
        set(MLX_METAL_JIT ON CACHE BOOL "Enable Metal JIT compilation" FORCE)
        set(MLX_BUILD_CPU ON CACHE BOOL "Enable CPU backend" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build MLX as static library" FORCE)

        # Use MinSizeRel or Release for MLX
        set(MLX_ORIGINAL_BUILD_TYPE ${CMAKE_BUILD_TYPE})
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(CMAKE_BUILD_TYPE MinSizeRel)
        endif()

        add_subdirectory(${MLX_DIR} mlx_build)

        # Restore original build type
        if(DEFINED MLX_ORIGINAL_BUILD_TYPE)
            set(CMAKE_BUILD_TYPE ${MLX_ORIGINAL_BUILD_TYPE})
        endif()

        include_directories(${MLX_DIR})
        set(HAS_MLX TRUE)
        add_compile_definitions(HAS_MLX=1)
        message(STATUS "[pebble] MLX: Building as static library from dependencies/mlx")
    endif()
endif()

# ---- metal-cpp (raw Metal, macOS only) --------------------------------------
set(HAS_METAL_CPP FALSE)
if(APPLE)
    set(METAL_CPP_DIR ${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)
    if(EXISTS ${METAL_CPP_DIR}/Metal/Metal.hpp)
        include_directories(${METAL_CPP_DIR})
        find_library(METAL_FRAMEWORK      Metal      REQUIRED)
        find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
        find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
        set(HAS_METAL_CPP TRUE)
        add_compile_definitions(HAS_METAL_CPP=1)
        message(STATUS "[pebble] metal-cpp: enabled (${METAL_CPP_DIR})")
    else()
        message(STATUS "[pebble] metal-cpp: headers not found, GPU backend disabled")
    endif()
endif()

find_package(Threads REQUIRED)

# Pravaha is a header-only orchestration and heterogeneous-compute component.
# Lithe remains downstream: its runtime and Vulkan bridges are discovered by
# their own headers and compile to an unavailable surface when absent.
add_library(pebble_pravaha INTERFACE)
add_library(pebble::pravaha ALIAS pebble_pravaha)
target_include_directories(pebble_pravaha INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Pravaha's public API is C++23.  The top-level project independently enables
# C++26 on Apple toolchains that support it, so consumers retain that upgrade.
target_compile_features(pebble_pravaha INTERFACE cxx_std_23)
target_link_libraries(pebble_pravaha INTERFACE Threads::Threads hwy)
if (HAS_METAL_CPP)
    target_link_libraries(pebble_pravaha INTERFACE
        ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
endif()

add_library(pebble_anukrama INTERFACE)
add_library(pebble::anukrama ALIAS pebble_anukrama)
target_include_directories(pebble_anukrama INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_anukrama INTERFACE cxx_std_23)

add_library(pebble_akruti_lib INTERFACE)
add_library(pebble::akruti ALIAS pebble_akruti_lib)
target_include_directories(pebble_akruti_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_akruti_lib INTERFACE cxx_std_23)

add_library(pebble_prakriti_lib INTERFACE)
add_library(pebble::prakriti ALIAS pebble_prakriti_lib)
target_include_directories(pebble_prakriti_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_prakriti_lib INTERFACE cxx_std_23)
add_library(pebble_ecs INTERFACE)
add_library(pebble::ecs ALIAS pebble_ecs)
target_include_directories(pebble_ecs INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_ecs INTERFACE cxx_std_23)
target_link_libraries(pebble_ecs INTERFACE pebble::pravaha)

add_library(pebble_gati_lib INTERFACE)
add_library(pebble::gati ALIAS pebble_gati_lib)
target_include_directories(pebble_gati_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_gati_lib INTERFACE cxx_std_23)
target_link_libraries(pebble_gati_lib INTERFACE pebble::ecs pebble::akruti pebble::prakriti pebble::pravaha)

add_library(pebble_spandana_lib INTERFACE)
add_library(pebble::spandana ALIAS pebble_spandana_lib)
target_include_directories(pebble_spandana_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_spandana_lib INTERFACE cxx_std_23)
target_link_libraries(pebble_spandana_lib INTERFACE pebble::ecs pebble::akruti pebble::prakriti pebble::pravaha)

add_library(pebble_dhvani_lib INTERFACE)
add_library(pebble::dhvani ALIAS pebble_dhvani_lib)
target_include_directories(pebble_dhvani_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_dhvani_lib INTERFACE cxx_std_23)

add_library(pebble_kalpana_lib INTERFACE)
add_library(pebble::kalpana ALIAS pebble_kalpana_lib)
target_include_directories(pebble_kalpana_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dependencies/sokol>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dependencies/notcurses/include>)
target_compile_features(pebble_kalpana_lib INTERFACE cxx_std_23)
target_compile_definitions(pebble_kalpana_lib INTERFACE
    KALPANA_ENABLE_SOKOL_BACKEND=$<BOOL:${PEBBLE_ENABLE_KALPANA_SOKOL_BACKEND}>
    KALPANA_ENABLE_NOTCURSES_BACKEND=$<BOOL:${PEBBLE_ENABLE_KALPANA_NOTCURSES_BACKEND}>)
target_link_libraries(pebble_kalpana_lib INTERFACE pebble::akruti pebble::pravaha)

add_library(pebble_rekha_lib INTERFACE)
add_library(pebble::rekha ALIAS pebble_rekha_lib)
target_include_directories(pebble_rekha_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_rekha_lib INTERFACE cxx_std_23)
target_link_libraries(pebble_rekha_lib INTERFACE pebble::akruti pebble::gati pebble::kalpana)

# Drishya — header-only retained-mode widget/UI composition engine. Composes
# widgets via akruti (layout), kalpana (render), spandana (animation),
# gati (input), and containers (storage/reactive).
add_library(pebble_drishya_lib INTERFACE)
add_library(pebble::drishya ALIAS pebble_drishya_lib)
target_include_directories(pebble_drishya_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_drishya_lib INTERFACE cxx_std_23)
target_link_libraries(pebble_drishya_lib INTERFACE
    pebble::akruti pebble::kalpana pebble::gati pebble::spandana)

add_library(pebble_manas_lib INTERFACE)
add_library(pebble::manas ALIAS pebble_manas_lib)
target_include_directories(pebble_manas_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_manas_lib INTERFACE cxx_std_23)

add_library(pebble_kalpa_lib INTERFACE)
add_library(pebble::kalpa ALIAS pebble_kalpa_lib)
target_include_directories(pebble_kalpa_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_kalpa_lib INTERFACE cxx_std_23)

add_library(pebble_sanchaya_lib INTERFACE)
add_library(pebble::sanchaya ALIAS pebble_sanchaya_lib)
target_include_directories(pebble_sanchaya_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_sanchaya_lib INTERFACE cxx_std_23)

# dependencies limited to Pebble containers; integrations remain opt-in.
add_library(pebble_medha INTERFACE)
add_library(pebble::medha ALIAS pebble_medha)
target_include_directories(pebble_medha INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pebble_medha INTERFACE cxx_std_23)

add_library(pebble_medha_smriti INTERFACE)
add_library(pebble::medha_smriti ALIAS pebble_medha_smriti)
target_link_libraries(pebble_medha_smriti INTERFACE pebble::medha)

add_library(pebble_medha_tarka INTERFACE)
add_library(pebble::medha_tarka ALIAS pebble_medha_tarka)
target_link_libraries(pebble_medha_tarka INTERFACE pebble::medha)

add_library(pebble_medha_pravaha INTERFACE)
add_library(pebble::medha_pravaha ALIAS pebble_medha_pravaha)
target_link_libraries(pebble_medha_pravaha INTERFACE pebble::medha pebble::pravaha)

# Current Lithe lowering is metadata-only; it has no Pebble Lithe dependency.
add_library(pebble_medha_lithe_metadata INTERFACE)
add_library(pebble::medha_lithe_metadata ALIAS pebble_medha_lithe_metadata)
target_link_libraries(pebble_medha_lithe_metadata INTERFACE pebble::medha)

link_directories(/usr/local/lib)
link_directories(${HOME_LIB_DIR})

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
    if (TARGET libz3)
        target_link_libraries(${PROJECT_NAME} PRIVATE libz3)
    elseif (TARGET z3_imported)
        target_link_libraries(${PROJECT_NAME} PRIVATE z3_imported)
    endif()
    if (HAS_MLX AND TARGET mlx)
        target_link_libraries(${PROJECT_NAME} PRIVATE mlx)
    endif()
    if (HAS_METAL_CPP)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        TURBO_TWIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

if (EXISTS "${CMAKE_SOURCE_DIR}/src/examples/rekha_demo.cpp")
    add_executable(pebble_rekha_demo src/examples/rekha_demo.cpp)
    target_link_libraries(pebble_rekha_demo PRIVATE
        pebble::rekha
        pebble::kalpana
        pebble::gati
        pebble::akruti
    )
    target_compile_definitions(pebble_rekha_demo PRIVATE
        TURBO_TWIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

if (BUILD_TESTS)
    file(
            GLOB_RECURSE TEST_SOURCE
            "${CMAKE_SOURCE_DIR}/src/tests/*.cpp"
    )
    if (NOT PEBBLE_ENABLE_PRAVAHA_EXTERNAL_ADAPTER_TESTS)
        # These suites exercise downstream Lithe preset/Vulkan APIs or the
        # Turbo Twig Sutra adapter; neither belongs to Pebble's core graph.
        list(FILTER TEST_SOURCE EXCLUDE REGEX ".*/src/tests/pravaha/(test_npravaha_.*|test_pravaha_ext)\\.cpp$")
    endif()
    add_executable(pebble_tests ${TEST_SOURCE})
    target_link_libraries(pebble_tests PRIVATE
            Catch2
            pebble::medha
            pebble::medha_pravaha
            pebble::pravaha
            pebble::drishya
            spdlog::spdlog_header_only
            crc32c
    )
    if (TARGET lmdb)
        target_link_libraries(pebble_tests PRIVATE lmdb)
    endif()
    if (TARGET libz3)
        target_link_libraries(pebble_tests PRIVATE libz3)
    elseif (TARGET z3_imported)
        target_link_libraries(pebble_tests PRIVATE z3_imported)
    endif()
    if (HAS_MLX AND TARGET mlx)
        target_link_libraries(pebble_tests PRIVATE mlx)
    endif()
    if (HAS_METAL_CPP)
        target_link_libraries(pebble_tests PRIVATE
            ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    endif()
    target_compile_definitions(pebble_tests PRIVATE
        TURBO_TWIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif ()

# ---------------------------------------------------------------------------
# Master Application Executable (src/app/main.cpp) — Sokol native window
# ---------------------------------------------------------------------------
if (EXISTS "${CMAKE_SOURCE_DIR}/src/app/main.cpp")

    set(PEBBLE_APP_SOURCES
        src/app/main.cpp
        src/app/sokol_impl.cpp
    )

    add_executable(pebble_app ${PEBBLE_APP_SOURCES})

    target_link_libraries(pebble_app PRIVATE
        pebble::ecs
        pebble::akruti
        pebble::prakriti
        pebble::pravaha
        pebble::gati
        pebble::spandana
        pebble::kalpana
        pebble::dhvani
        spdlog::spdlog_header_only
    )

    # macOS: Metal backend + frameworks required by sokol_app.h + sokol_gfx.h
    if (APPLE)
        # Compile both translation units as Objective-C++ (required for Metal backend)
        set_source_files_properties(
            src/app/main.cpp
            src/app/sokol_impl.cpp
            PROPERTIES COMPILE_OPTIONS "-x;objective-c++"
        )
        target_compile_definitions(pebble_app PRIVATE SOKOL_METAL)
        find_library(FW_METAL        Metal)
        find_library(FW_FOUNDATION   Foundation)
        find_library(FW_QUARTZ       QuartzCore)
        find_library(FW_APPKIT       AppKit)
        find_library(FW_IOKIT        IOKit)
        target_link_libraries(pebble_app PRIVATE
            ${FW_METAL} ${FW_FOUNDATION} ${FW_QUARTZ} ${FW_APPKIT} ${FW_IOKIT})
    elseif (UNIX)
        target_compile_definitions(pebble_app PRIVATE SOKOL_GLCORE)
        target_link_libraries(pebble_app PRIVATE GL dl m)
    endif()

    if (HAS_METAL_CPP)
        target_link_libraries(pebble_app PRIVATE
            ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    endif()

    target_compile_definitions(pebble_app PRIVATE
        TURBO_TWIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Standalone Showcase Executables
# ---------------------------------------------------------------------------
foreach(_showcase pebble_akriti pebble_gati pebble_prakriti pebble_gati_prakriti pebble_tanks pebble_verse pebble_rekha pebble_drishya_ml pebble_drishya_hud)
    if (_showcase STREQUAL "pebble_rekha" AND DEFINED PEBBLE_ENABLE_KALPANA_SOKOL_BACKEND AND NOT PEBBLE_ENABLE_KALPANA_SOKOL_BACKEND)
        continue()
    endif()
    if (EXISTS "${CMAKE_SOURCE_DIR}/src/app/${_showcase}.cpp")
        set(_showcase_srcs
            src/app/${_showcase}.cpp
            src/app/sokol_impl.cpp
        )
        add_executable(${_showcase} ${_showcase_srcs})
        target_link_libraries(${_showcase} PRIVATE
            pebble::ecs
            pebble::akruti
            pebble::prakriti
            pebble::pravaha
            pebble::gati
            pebble::spandana
            pebble::kalpana
            pebble::dhvani
            spdlog::spdlog_header_only
        )
        if (APPLE)
            set_source_files_properties(
                src/app/${_showcase}.cpp
                PROPERTIES COMPILE_OPTIONS "-x;objective-c++"
            )
            target_compile_definitions(${_showcase} PRIVATE SOKOL_METAL)
            target_link_libraries(${_showcase} PRIVATE
                ${FW_METAL} ${FW_FOUNDATION} ${FW_QUARTZ} ${FW_APPKIT} ${FW_IOKIT})
        elseif (UNIX)
            target_compile_definitions(${_showcase} PRIVATE SOKOL_GLCORE)
            target_link_libraries(${_showcase} PRIVATE GL dl m)
        endif()
        if (HAS_METAL_CPP)
            target_link_libraries(${_showcase} PRIVATE
                ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
        endif()
        target_compile_definitions(${_showcase} PRIVATE
            TURBO_TWIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    endif()
endforeach()

# Extra deps for Drishya intelligence showcase apps
foreach(_drishya_app pebble_drishya_ml pebble_drishya_hud)
    if (TARGET ${_drishya_app})
        target_link_libraries(${_drishya_app} PRIVATE
            pebble::drishya
            pebble::rekha
            pebble::manas
            pebble::kalpa
            crc32c
            hwy)
    endif()
endforeach()

