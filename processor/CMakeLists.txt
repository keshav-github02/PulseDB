# Event Processor module: interprets typed events into metrics.
add_library(pulsedb_processor
    src/metric_accumulator.cpp)
add_library(pulsedb::processor ALIAS pulsedb_processor)

target_include_directories(pulsedb_processor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_processor
    PUBLIC
        pulsedb::core
    PRIVATE
        pulsedb::warnings)

target_compile_features(pulsedb_processor PUBLIC cxx_std_20)
