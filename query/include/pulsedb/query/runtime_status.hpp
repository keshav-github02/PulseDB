#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace pulsedb::query {

/// A point-in-time snapshot of server operational metrics -- the data behind
/// the dashboard's "live operations" panels (events/sec, queue depth, active
/// sessions, error rate, CPU, memory).
struct RuntimeStatusSnapshot {
    double uptime_sec = 0.0;
    std::size_t workers = 0;
    std::size_t queue_depth = 0;
    double events_per_sec = 0.0;
    std::int64_t active_sessions = 0;
    std::uint64_t total_events = 0;
    std::uint64_t error_count = 0;
    double error_rate = 0.0;  // errors / total_events, in [0, 1]
    double cpu_percent = 0.0;
    double memory_mb = 0.0;
};

/// Thread-safe holder: one writer (the server's sampler thread) publishes a
/// snapshot; the query server reads it to serve GET /status.
class RuntimeStatus {
public:
    void set(const RuntimeStatusSnapshot& snapshot) {
        std::lock_guard lock(mutex_);
        snapshot_ = snapshot;
    }

    RuntimeStatusSnapshot get() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    RuntimeStatusSnapshot snapshot_{};
};

}  // namespace pulsedb::query
