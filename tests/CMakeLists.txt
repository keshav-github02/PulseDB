include(FetchContent)

FetchContent_Declare(
    googletest
    SYSTEM
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# --- Queue tests -----------------------------------------------------------
add_executable(pulsedb_queue_tests
    queue/bounded_blocking_queue_test.cpp)
target_link_libraries(pulsedb_queue_tests
    PRIVATE
        pulsedb::queue
        pulsedb::warnings
        GTest::gtest_main)
gtest_discover_tests(pulsedb_queue_tests)

# --- Core tests (event parsing) --------------------------------------------
add_executable(pulsedb_core_tests
    core/event_parse_test.cpp)
target_link_libraries(pulsedb_core_tests
    PRIVATE
        pulsedb::core
        pulsedb::warnings
        GTest::gtest_main)
gtest_discover_tests(pulsedb_core_tests)

# --- Processor tests -------------------------------------------------------
add_executable(pulsedb_processor_tests
    processor/metrics_processor_test.cpp)
target_link_libraries(pulsedb_processor_tests
    PRIVATE
        pulsedb::processor
        pulsedb::core
        pulsedb::warnings
        GTest::gtest_main)
gtest_discover_tests(pulsedb_processor_tests)

# --- Worker pool tests -----------------------------------------------------
add_executable(pulsedb_workers_tests
    workers/worker_pool_test.cpp)
target_link_libraries(pulsedb_workers_tests
    PRIVATE
        pulsedb::workers
        pulsedb::processor
        pulsedb::core
        pulsedb::queue
        pulsedb::warnings
        GTest::gtest_main)
gtest_discover_tests(pulsedb_workers_tests)

# --- Storage tests ---------------------------------------------------------
add_executable(pulsedb_storage_tests
    storage/minute_key_test.cpp
    storage/time_series_store_test.cpp)
target_link_libraries(pulsedb_storage_tests
    PRIVATE
        pulsedb::storage
        pulsedb::warnings
        GTest::gtest_main)
gtest_discover_tests(pulsedb_storage_tests)

# --- Aggregation tests -----------------------------------------------------
add_executable(pulsedb_aggregation_tests
    aggregation/aggregation_engine_test.cpp)
target_link_libraries(pulsedb_aggregation_tests
    PRIVATE
        pulsedb::aggregation
        pulsedb::processor
        pulsedb::storage
        pulsedb::core
        pulsedb::warnings
        GTest::gtest_main)
gtest_discover_tests(pulsedb_aggregation_tests)

# --- Query / REST API tests (unit + live integration) ----------------------
add_executable(pulsedb_query_tests
    query/metrics_api_test.cpp
    query/query_server_test.cpp
    query/runtime_status_test.cpp
    query/system_stats_test.cpp)
target_link_libraries(pulsedb_query_tests
    PRIVATE
        pulsedb::query
        pulsedb::aggregation
        pulsedb::processor
        pulsedb::storage
        pulsedb::core
        pulsedb::warnings
        httplib::httplib
        nlohmann_json::nlohmann_json
        GTest::gtest_main)
if(WIN32)
    target_link_libraries(pulsedb_query_tests PRIVATE ws2_32)
endif()
gtest_discover_tests(pulsedb_query_tests)

# --- Persistence tests -----------------------------------------------------
add_executable(pulsedb_persistence_tests
    persistence/metrics_snapshot_store_test.cpp)
target_link_libraries(pulsedb_persistence_tests
    PRIVATE
        pulsedb::persistence
        pulsedb::aggregation
        pulsedb::processor
        pulsedb::storage
        pulsedb::core
        pulsedb::warnings
        GTest::gtest_main)
gtest_discover_tests(pulsedb_persistence_tests)

# --- Collector tests (pure unit + live integration) ------------------------
add_executable(pulsedb_collector_tests
    collector/ingest_handler_test.cpp
    collector/collector_integration_test.cpp)
target_link_libraries(pulsedb_collector_tests
    PRIVATE
        pulsedb::collector
        pulsedb::core
        pulsedb::queue
        pulsedb::storage  # timestamp-window tests assert against MinuteKey's domain
        pulsedb::warnings
        httplib::httplib
        nlohmann_json::nlohmann_json
        GTest::gtest_main)
if(WIN32)
    target_link_libraries(pulsedb_collector_tests PRIVATE ws2_32)
endif()
gtest_discover_tests(pulsedb_collector_tests)

# --- SDK simulator tests (unit + live sink integration) --------------------
add_executable(pulsedb_sdk_tests
    sdk/session_generator_test.cpp
    sdk/backoff_test.cpp
    sdk/retrying_sender_test.cpp
    sdk/url_test.cpp
    sdk/simulator_test.cpp
    sdk/spool_store_test.cpp
    sdk/spooling_sender_test.cpp
    sdk/http_event_sink_integration_test.cpp)
target_link_libraries(pulsedb_sdk_tests
    PRIVATE
        pulsedb::sdk
        pulsedb::collector
        pulsedb::core
        pulsedb::queue
        pulsedb::warnings
        httplib::httplib
        nlohmann_json::nlohmann_json
        GTest::gtest_main)
if(WIN32)
    target_link_libraries(pulsedb_sdk_tests PRIVATE ws2_32)
endif()
gtest_discover_tests(pulsedb_sdk_tests)
