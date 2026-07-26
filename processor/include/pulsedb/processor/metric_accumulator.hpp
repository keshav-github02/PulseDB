#pragma once

#include <atomic>
#include <cstdint>

#include "pulsedb/core/event.hpp"
#include "pulsedb/processor/metrics.hpp"

namespace pulsedb::processor {

/// Lock-free accumulator that folds events into aggregate metric counters.
///
/// This is the single implementation of "event -> metrics" folding, reused
/// both for global totals (MetricsProcessor) and for each time bucket in
/// the aggregation engine. Counters are atomics, so many worker threads can
/// add() concurrently without a mutex. It is neither copyable nor movable
/// (atomics), so it is constructed in place wherever it lives.
class MetricAccumulator {
public:
    MetricAccumulator() = default;

    /// Fold one event into the counters. Safe to call from many threads.
    ///
    /// Negative sample payloads are skipped rather than folded in. This is the
    /// invariant that makes counter poisoning unreachable: the counters are
    /// unsigned, so a single negative sample would convert to ~1.8e19 and
    /// permanently destroy the corresponding average -- irreversibly, because
    /// counters only ever increase and buckets are never recomputed. The ingest
    /// layer already range-checks these fields and answers 422, so reaching
    /// here with a negative value means a non-HTTP caller; enforcing the
    /// invariant at the point of aggregation keeps it true for every caller.
    void add(const core::Event& event);

    /// Add a previously captured snapshot's raw counters back in. Used to
    /// restore aggregate state from a persisted snapshot.
    void add_snapshot(const MetricsSnapshot& snapshot);

    /// A consistent-enough copy of the current counters.
    MetricsSnapshot snapshot() const;

private:
    std::atomic<std::uint64_t> total_events_{0};
    std::atomic<std::uint64_t> total_views_{0};
    std::atomic<std::uint64_t> startup_samples_{0};
    std::atomic<std::uint64_t> startup_time_ms_sum_{0};
    std::atomic<std::uint64_t> buffer_count_{0};
    std::atomic<std::uint64_t> buffer_samples_{0};
    std::atomic<std::uint64_t> buffer_duration_ms_sum_{0};
    std::atomic<std::uint64_t> error_count_{0};
    std::atomic<std::uint64_t> watch_time_ms_sum_{0};
    std::atomic<std::uint64_t> bitrate_samples_{0};
    std::atomic<std::uint64_t> bitrate_kbps_sum_{0};
};

}  // namespace pulsedb::processor
