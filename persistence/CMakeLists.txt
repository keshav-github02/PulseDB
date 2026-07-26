# Persistence module: snapshot/restore of aggregate metrics to disk.
add_library(pulsedb_persistence
    src/metrics_snapshot_store.cpp)
add_library(pulsedb::persistence ALIAS pulsedb_persistence)

target_include_directories(pulsedb_persistence PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_persistence
    PUBLIC
        pulsedb::aggregation
        pulsedb::processor
        pulsedb::storage
        nlohmann_json::nlohmann_json
    PRIVATE
        pulsedb::warnings)

target_compile_features(pulsedb_persistence PUBLIC cxx_std_20)
