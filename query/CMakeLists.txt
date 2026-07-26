# Query module: the read-only REST metrics API (library + HTTP server).

add_library(pulsedb_query
    src/metrics_api.cpp
    src/query_server.cpp
    src/system_stats.cpp)
add_library(pulsedb::query ALIAS pulsedb_query)

target_include_directories(pulsedb_query PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_query
    PUBLIC
        pulsedb::aggregation
        pulsedb::processor
        pulsedb::storage
        nlohmann_json::nlohmann_json
    PRIVATE
        httplib::httplib
        pulsedb::warnings)

target_compile_features(pulsedb_query PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(pulsedb_query PRIVATE ws2_32 psapi)
endif()
