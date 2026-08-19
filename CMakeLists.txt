cmake_minimum_required(VERSION 4.1)
project(pebble LANGUAGES C CXX)

# Set the default C++ standard to C++23
set(PROJECT_NAME pebble)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23) # Requires C11 standard
set(CMAKE_CXX_STANDARD_REQUIRED ON)  # optional, ensure standard is supported
set(CMAKE_CXX_EXTENSIONS OFF)  # optional, keep compiler extensions off
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_VERBOSE_MAKEFILE ON)

set(BUILD_EXECUTABLE ON)
set(BUILD_EXEMAIN "exemain")
set(BUILD_SHARED_LIBS OFF)
set(BUILD_SHARED OFF)

option(BUILD_TESTS "Build and run tests" ON)
option(BUILD_APP "Build the application" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Catch2
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "disable building tests" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "disable building examples" FORCE)
set(CATCH_BUILD_FUZZERS OFF CACHE BOOL "disable building fuzzers" FORCE)

message("CMAKE_SOURCE_DIR location: ${CMAKE_SOURCE_DIR}")
include(FetchContent)
include(ExternalProject)
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