# Time-Series Storage module: header-only nested Y/M/D/H/Minute bucket store.
add_library(pulsedb_storage INTERFACE)
add_library(pulsedb::storage ALIAS pulsedb_storage)

target_include_directories(pulsedb_storage INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(pulsedb_storage INTERFACE cxx_std_20)
