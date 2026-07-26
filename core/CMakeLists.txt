# Core module: shared domain vocabulary types. Header-only so that both
# the collector and the (future) worker pool can depend on it without
# depending on each other.
add_library(pulsedb_core INTERFACE)
add_library(pulsedb::core ALIAS pulsedb_core)

target_include_directories(pulsedb_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_core INTERFACE nlohmann_json::nlohmann_json)
target_compile_features(pulsedb_core INTERFACE cxx_std_20)
