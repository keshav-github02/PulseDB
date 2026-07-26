#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "pulsedb/core/event_batch.hpp"
#include "pulsedb/processor/event_processor.hpp"
#include "pulsedb/queue/bounded_blocking_queue.hpp"

namespace pulsedb::workers {

/// The queue the pool consumes: the same instantiation the collector fills.
using EventQueue = queue::BoundedBlockingQueue<core::EventBatch>;

/// Cumulative work performed by the pool.
struct WorkerStats {
    std::uint64_t batches_processed = 0;
    std::uint64_t events_processed = 0;
    std::uint64_t parse_failures = 0;   ///< Events that could not be parsed.
    std::uint64_t process_errors = 0;   ///< Events whose processing threw.
};

/// A fixed-size pool of worker threads draining an EventQueue.
///
/// Each worker blocks on queue.pop(), parses every event in the popped
/// batch, and forwards the parsed events to a shared EventProcessor
/// (which must be thread-safe). Shutdown is cooperative: stop() closes the
/// queue, letting workers drain whatever remains before they exit.
class WorkerPool {
public:
    /// @param queue      Batch source (owned by the caller).
    /// @param processor  Destination for parsed events (owned by the caller).
    /// @param num_workers Thread count; 0 selects default_worker_count().
    WorkerPool(EventQueue& queue, processor::EventProcessor& processor,
               std::size_t num_workers = 0);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Spawn the worker threads. Idempotent (a second call is a no-op).
    void start();

    /// Close the queue and join all workers. Idempotent.
    void stop();

    [[nodiscard]] std::size_t worker_count() const noexcept { return num_workers_; }
    [[nodiscard]] WorkerStats stats() const;

    /// hardware_concurrency(), clamped to at least 1.
    [[nodiscard]] static std::size_t default_worker_count();

private:
    void worker_loop();

    EventQueue& queue_;
    processor::EventProcessor& processor_;
    std::size_t num_workers_;
    std::vector<std::thread> workers_;
    bool started_ = false;

    std::atomic<std::uint64_t> batches_{0};
    std::atomic<std::uint64_t> events_{0};
    std::atomic<std::uint64_t> parse_failures_{0};
    std::atomic<std::uint64_t> process_errors_{0};
};

}  // namespace pulsedb::workers
