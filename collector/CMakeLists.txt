# Collector module: the HTTP ingestion library plus a thin server binary.

# --- Library (unit-testable, no main) --------------------------------------
add_library(pulsedb_collector
    src/ingest_handler.cpp
    src/collector.cpp)
add_library(pulsedb::collector ALIAS pulsedb_collector)

target_include_directories(pulsedb_collector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_collector
    PUBLIC
        pulsedb::core
        pulsedb::queue
    PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
        pulsedb::warnings)

target_compile_features(pulsedb_collector PUBLIC cxx_std_20)

# httplib uses Winsock on Windows.
if(WIN32)
    target_link_libraries(pulsedb_collector PRIVATE ws2_32)
endif()

# --- Server executable: wires collector + queue + workers + processor ------
add_executable(pulsedb_collector_app app/main.cpp)
set_target_properties(pulsedb_collector_app PROPERTIES OUTPUT_NAME pulsedb_collector)

target_link_libraries(pulsedb_collector_app
    PRIVATE
        pulsedb::collector
        pulsedb::core
        pulsedb::queue
        pulsedb::workers
        pulsedb::aggregation
        pulsedb::query
        pulsedb::persistence
        pulsedb::processor
        nlohmann_json::nlohmann_json
        pulsedb::warnings)
