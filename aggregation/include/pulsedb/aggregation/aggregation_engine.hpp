#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pulsedb/core/event.hpp"
#include "pulsedb/processor/event_processor.hpp"
#include "pulsedb/processor/metric_accumulator.hpp"
#include "pulsedb/processor/metrics.hpp"
#include "pulsedb/storage/minute_key.hpp"
#include "pulsedb/storage/time_series_store.hpp"

namespace pulsedb::aggregation {

/// Metrics for one minute bucket.
struct MinutePoint {
    storage::MinuteKey key;
    processor::MetricsSnapshot metrics;
};

/// Metrics for one named segment (a player or a device).
struct Segment {
    std::string name;
    processor::MetricsSnapshot metrics;
};

/// The segment every name beyond max_segments_per_dimension is folded into, so
/// its events still count toward totals instead of being dropped.
inline constexpr std::string_view kOverflowSegmentName = "__other__";

/// Tunable limits on what the engine will allocate.
struct AggregationOptions {
    /// Distinct player (and device) names tracked before the rest collapse
    /// into kOverflowSegmentName.
    ///
    /// Segment names arrive as arbitrary client-supplied strings and become
    /// permanent map keys, so without a cap a client sending a unique player
    /// per event grows memory without bound -- the label-cardinality
    /// explosion that monitoring systems are routinely taken down by. The
    /// ingest layer bounds each name's *length*; this bounds their *count*.
    std::size_t max_segments_per_dimension = 1'000;
};

/// An EventProcessor that aggregates events three ways at once:
///   * into per-minute buckets (the time series),
///   * by player,
///   * by device.
///
/// Each event is folded into the bucket for the minute of its (event-time)
/// timestamp and into its player/device segments. The same instance drives
/// the worker pool and answers queries.
///
/// Synchronisation, stated precisely because an earlier version of this comment
/// claimed the opposite: once a bucket or segment reference is in hand, updating
/// it is lock-free (the counters are atomics). *Reaching* it is not. process()
/// takes three mutexes per event -- the store's, and one per segment map -- even
/// on the common path where the bucket already exists, and every query holds the
/// same store mutex for the duration of its traversal. So ingestion and queries
/// do contend, ingestion does not scale with worker count, and query cost grows
/// with the number of buckets (which are never evicted).
///
/// Measured on 16 cores: 3.9M events/s on one thread, 1.9M on eight (0.49x); a
/// 15-bucket /metrics/live costs ~13 ms of held store mutex at 30 days of
/// uptime, ~38 ms at 90 days. The fix is per-worker sharded accumulators merged
/// on read, which removes the mutex from the write path entirely; it is not done
/// here, and until it is, these numbers are the operating envelope.
class AggregationEngine : public processor::EventProcessor {
public:
    explicit AggregationEngine(AggregationOptions options = {})
        : players_(options.max_segments_per_dimension),
          devices_(options.max_segments_per_dimension) {}

    void process(const core::Event& event) override;

    // --- Time-series queries ---
    /// Metrics for a specific minute, or std::nullopt if it has no events.
    std::optional<processor::MetricsSnapshot> minute(const storage::MinuteKey& key) const;
    /// Every minute bucket, ordered by time (ascending).
    std::vector<MinutePoint> points() const;
    /// The @p n most recent minute buckets, ordered by time (ascending).
    std::vector<MinutePoint> recent(std::size_t n) const;
    /// Buckets whose minute falls within [from, to], inclusive.
    std::vector<MinutePoint> range(const storage::MinuteKey& from,
                                   const storage::MinuteKey& to) const;
    std::size_t minute_count() const { return store_.minute_count(); }

    /// Sessions currently in flight: incremented on video_start, decremented
    /// on playback_end but never below zero. A live gauge (not persisted, so it
    /// restarts at zero while sessions opened before the restart are still
    /// running -- their playback_end events are absorbed by the floor).
    std::int64_t active_sessions() const noexcept {
        return active_sessions_.load(std::memory_order_relaxed);
    }

    // --- Segment queries ---
    /// Per-player metrics, ordered by player name.
    std::vector<Segment> by_player() const { return players_.snapshot(); }
    /// Per-device metrics, ordered by device name.
    std::vector<Segment> by_device() const { return devices_.snapshot(); }

    /// Times a segment name was folded into kOverflowSegmentName because the
    /// cardinality cap was already reached. Non-zero means the player/device
    /// breakdown has lost resolution (totals are still exact).
    std::uint64_t segment_overflows() const noexcept {
        return players_.overflows() + devices_.overflows();
    }

    // --- Platform totals ---
    /// Sum of every minute bucket.
    processor::MetricsSnapshot total() const;

    // --- Restore (from a persisted snapshot) ---
    // Additively fold saved counters back into the corresponding bucket or
    // segment. Intended to reload state into a fresh engine at startup.
    void restore_minute(const storage::MinuteKey& key,
                        const processor::MetricsSnapshot& metrics);
    void restore_player(const std::string& name, const processor::MetricsSnapshot& metrics);
    void restore_device(const std::string& name, const processor::MetricsSnapshot& metrics);

private:
    /// A thread-safe map of name -> accumulator, used for the player and
    /// device breakdowns. Mirrors the time-series store's discipline: a mutex
    /// guards structure while accumulators update via atomics, and accumulator
    /// addresses stay stable across insertion.
    ///
    /// Uses std::mutex rather than std::shared_mutex for the same reason
    /// TimeSeriesStore does -- see the note there; std::shared_mutex does not
    /// reliably provide mutual exclusion on MinGW/winpthreads.
    class SegmentMap {
    public:
        explicit SegmentMap(std::size_t max_segments) : max_segments_(max_segments) {}

        /// Accumulator for @p name, or the shared overflow accumulator once
        /// max_segments distinct names are already tracked.
        ///
        /// Overflow folds rather than drops: the events still land in totals
        /// under kOverflowSegmentName, so aggregate numbers stay correct even
        /// when the breakdown loses resolution.
        processor::MetricAccumulator& get_or_create(const std::string& name) {
            std::lock_guard lock(mutex_);
            // kOverflowSegmentName is reserved, and has exactly one home: the
            // dedicated accumulator below. Two callers reach here with it.
            //
            // A restore does, because a snapshot records the overflow segment as
            // an ordinary named row. Letting that become a normal map entry while
            // subsequent labels still folded into overflow_ made snapshot() emit
            // *two* segments sharing this name -- so /metrics/player returned a
            // duplicate key, the dashboard drew two identically-labelled bars,
            // and any consumer keyed by name double-counted or silently picked
            // one. Reachable after any restart of a deployment that had exceeded
            // the cardinality cap.
            //
            // A client can too, and routing it here also stops it occupying one
            // of the capped slots or forging a row that looks like this
            // aggregator's own bookkeeping.
            if (name == kOverflowSegmentName) {
                return overflow_;
            }
            if (const auto it = map_.find(name); it != map_.end()) {
                return it->second;
            }
            if (map_.size() >= max_segments_) {
                overflows_.fetch_add(1, std::memory_order_relaxed);
                return overflow_;
            }
            return map_.try_emplace(name).first->second;
        }

        /// Names folded into the overflow segment (counts occurrences, not
        /// distinct names -- tracking those would defeat the cap).
        std::uint64_t overflows() const noexcept {
            return overflows_.load(std::memory_order_relaxed);
        }

        std::vector<Segment> snapshot() const;

    private:
        mutable std::mutex mutex_;
        std::size_t max_segments_;
        std::unordered_map<std::string, processor::MetricAccumulator> map_;
        /// Held outside map_ so it never consumes one of the capped slots.
        processor::MetricAccumulator overflow_;
        std::atomic<std::uint64_t> overflows_{0};
    };

    storage::TimeSeriesStore<processor::MetricAccumulator> store_;
    SegmentMap players_;
    SegmentMap devices_;
    std::atomic<std::int64_t> active_sessions_{0};
};

}  // namespace pulsedb::aggregation
