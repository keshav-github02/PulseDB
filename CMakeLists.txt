cmake_minimum_required(VERSION 3.25)

project(PulseDB
    VERSION 0.1.0
    DESCRIPTION "High-performance telemetry analytics platform"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PULSEDB_BUILD_TESTS "Build PulseDB unit tests" ON)
option(PULSEDB_BUILD_BENCHMARKS "Build PulseDB benchmarks" OFF)

# Shared warning flags, consumed by every first-party PulseDB target.
add_library(pulsedb_warnings INTERFACE)
add_library(pulsedb::warnings ALIAS pulsedb_warnings)
if(MSVC)
    target_compile_options(pulsedb_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(pulsedb_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Statically link the MinGW runtime so binaries do not depend on whichever
# libstdc++/libwinpthread DLLs happen to be on PATH.
if(MINGW)
    add_link_options(-static)
endif()

# --- Third-party production dependencies -----------------------------------
# Declared SYSTEM so their headers do not trip our -Wall -Wextra -Wpedantic.
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    SYSTEM
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")

FetchContent_Declare(
    httplib
    SYSTEM
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
)
# Keep the HTTP layer dependency-free: no TLS / compression probing.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(nlohmann_json httplib)

# --- First-party modules ---------------------------------------------------
add_subdirectory(core)
add_subdirectory(queue)
add_subdirectory(storage)
add_subdirectory(processor)
add_subdirectory(aggregation)
add_subdirectory(query)
add_subdirectory(persistence)
add_subdirectory(workers)
add_subdirectory(collector)
add_subdirectory(sdk)

if(PULSEDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PULSEDB_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
