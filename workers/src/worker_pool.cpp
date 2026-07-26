#include "pulsedb/workers/worker_pool.hpp"

#include <algorithm>

#include "pulsedb/core/event.hpp"

namespace pulsedb::workers {

std::size_t WorkerPool::default_worker_count() {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

WorkerPool::WorkerPool(EventQueue& queue, processor::EventProcessor& processor,
                       std::size_t num_workers)
    : queue_(queue),
      processor_(processor),
      num_workers_(num_workers == 0 ? default_worker_count() : num_workers) {}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::start() {
    if (started_) {
        return;
    }
    started_ = true;
    workers_.reserve(num_workers_);
    for (std::size_t i = 0; i < num_workers_; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

void WorkerPool::stop() {
    if (!started_) {
        return;
    }
    queue_.close();  // wakes workers; they drain remaining batches then exit
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    started_ = false;
}

void WorkerPool::worker_loop() {
    while (auto batch = queue_.pop()) {
        std::uint64_t processed = 0;
        std::uint64_t failed = 0;
        std::uint64_t errored = 0;

        if (batch->events.is_array()) {
            for (const auto& raw_event : batch->events) {
                // Per-event boundary. process() is a virtual call into
                // arbitrary business logic and JSON access can throw on
                // pathological input; without this an exception escapes the
                // thread function and terminates the whole process, so one bad
                // event would take down the server. Isolate it to that event
                // and keep draining -- shedding one event beats losing the
                // process and everything still queued in it.
                try {
                    if (auto event = core::parse_event(raw_event)) {
                        processor_.process(*event);
                        ++processed;
                    } else {
                        ++failed;
                    }
                } catch (...) {
                    ++errored;
                }
            }
        }

        // One atomic update per batch keeps contention low while still
        // reflecting progress to a concurrent stats() reader.
        batches_.fetch_add(1, std::memory_order_relaxed);
        events_.fetch_add(processed, std::memory_order_relaxed);
        parse_failures_.fetch_add(failed, std::memory_order_relaxed);
        if (errored > 0) {
            process_errors_.fetch_add(errored, std::memory_order_relaxed);
        }
    }
}

WorkerStats WorkerPool::stats() const {
    WorkerStats s;
    s.batches_processed = batches_.load(std::memory_order_relaxed);
    s.events_processed = events_.load(std::memory_order_relaxed);
    s.parse_failures = parse_failures_.load(std::memory_order_relaxed);
    s.process_errors = process_errors_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace pulsedb::workers
