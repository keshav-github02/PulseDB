# Worker Pool module: threads that drain the queue and feed the processor.
add_library(pulsedb_workers
    src/worker_pool.cpp)
add_library(pulsedb::workers ALIAS pulsedb_workers)

target_include_directories(pulsedb_workers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_workers
    PUBLIC
        pulsedb::core
        pulsedb::queue
        pulsedb::processor
    PRIVATE
        pulsedb::warnings)

target_compile_features(pulsedb_workers PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(pulsedb_workers PUBLIC Threads::Threads)
