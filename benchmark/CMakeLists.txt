# Benchmark module: dependency-free throughput suites (see bench_util.hpp).
# Opt-in via -DPULSEDB_BUILD_BENCHMARKS=ON (see scripts/bench.ps1).

# Queue throughput (single-thread round-trip + multi-producer/consumer).
add_executable(pulsedb_queue_bench queue_bench.cpp)
target_link_libraries(pulsedb_queue_bench PRIVATE pulsedb::queue pulsedb::warnings)

# Aggregation engine throughput (event -> per-minute bucket folding).
add_executable(pulsedb_aggregation_bench aggregation_bench.cpp)
target_link_libraries(pulsedb_aggregation_bench
    PRIVATE
        pulsedb::aggregation
        pulsedb::processor
        pulsedb::storage
        pulsedb::core
        pulsedb::warnings)

# Ingest validation throughput (raw JSON batch -> validated EventBatch).
add_executable(pulsedb_ingest_bench ingest_bench.cpp)
target_link_libraries(pulsedb_ingest_bench
    PRIVATE
        pulsedb::collector
        pulsedb::core
        nlohmann_json::nlohmann_json
        pulsedb::warnings)
if(WIN32)
    target_link_libraries(pulsedb_ingest_bench PRIVATE ws2_32)
endif()
