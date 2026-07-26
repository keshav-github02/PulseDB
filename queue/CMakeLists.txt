# Event Queue module: header-only, no external dependencies.
add_library(pulsedb_queue INTERFACE)
add_library(pulsedb::queue ALIAS pulsedb_queue)

target_include_directories(pulsedb_queue INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(pulsedb_queue INTERFACE cxx_std_20)
