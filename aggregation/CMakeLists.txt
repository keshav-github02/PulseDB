# Aggregation Engine module: buckets events into per-minute time series.
add_library(pulsedb_aggregation
    src/aggregation_engine.cpp)
add_library(pulsedb::aggregation ALIAS pulsedb_aggregation)

target_include_directories(pulsedb_aggregation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_aggregation
    PUBLIC
        pulsedb::core
        pulsedb::processor
        pulsedb::storage
    PRIVATE
        pulsedb::warnings)

target_compile_features(pulsedb_aggregation PUBLIC cxx_std_20)
