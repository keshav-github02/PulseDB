#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    /// Distinct player (and device) names tracked *per shard* before the rest
    /// collapse into kOverflowSegmentName.
    ///
    /// Segment names arrive as arbitrary client-supplied strings and become
    /// permanent map keys, so without a cap a client sending a unique player
    /// per event grows memory without bound -- the label-cardinality
    /// explosion that monitoring systems are routinely taken down by. The
    /// ingest layer bounds each name's *length*; this bounds their *count*.
    ///
    /// Since sharding replicates the maps, the global bound is `shards` times
    /// this value. Memory stays bounded, which is the property that matters,
    /// but the ceiling is higher than the number alone suggests.
    std::size_t max_segments_per_dimension = 1'000;

    /// Independent accumulator shards. 0 selects default_shard_count().
    ///
    /// Ingestion writes only to the shard belonging to the calling thread, so
    /// with one writer per shard the per-shard mutexes are uncontended and
    /// throughput scales with worker count instead of collapsing under it.
    /// Reads merge across every shard.
    std::size_t shards = 0;
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
/// Synchronisation: state is **sharded per writer thread**. Each thread folds
/// events into its own shard -- its own time-series store and its own
/// player/device maps, each with its own mutex -- so with one writer per shard
/// those mutexes are uncontended and the atomics inside a bucket are the only
/// thing threads share. Reads merge across every shard.
///
/// This replaced a single shared store plus two shared segment maps, where
/// process() took three global mutexes per event even on the common path where
/// the bucket already existed. Measured on 16 cores, that design *lost*
/// throughput as workers were added -- 2.72M events/s on one thread, 1.53M on
/// eight (0.56x) -- so a pool sized to hardware_concurrency() ingested more
/// slowly than a single thread. Isolating the cause showed the mutex cost
/// roughly 15x the atomic contention, so removing it from the write path, not
/// padding the counters, was the fix.
///
/// The costs are real and worth stating: per-minute buckets and segment maps are
/// replicated per shard, so memory scales with shard count, and every query pays
/// a merge across shards. Reads are infrequent (a dashboard poll) and writes are
/// the hot path, which is the trade this makes deliberately.
///
/// Query cost still grows with the number of buckets, which are never evicted --
/// sharding does not address retention.
class AggregationEngine : public processor::EventProcessor {
public:
    explicit AggregationEngine(AggregationOptions options = {});

    /// Shards to use when AggregationOptions::shards is 0: hardware_concurrency()
    /// capped at 8. The cap is deliberate -- shards multiply resident memory, and
    /// past the worker count extra shards buy nothing but replication.
    [[nodiscard]] static std::size_t default_shard_count();

    /// Number of independent shards this engine was built with.
    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }

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
    /// Distinct minutes across every shard (not the number of buckets, which is
    /// higher because shards each hold their own bucket for a shared minute).
    std::size_t minute_count() const noexcept {
        return minute_count_.load(std::memory_order_relaxed);
    }

    /// Sessions currently in flight: incremented on video_start, decremented
    /// on playback_end but never below zero. A live gauge (not persisted, so it
    /// restarts at zero while sessions opened before the restart are still
    /// running -- their playback_end events are absorbed by the floor).
    std::int64_t active_sessions() const noexcept {
        return active_sessions_.load(std::memory_order_relaxed);
    }

    // --- Segment queries ---
    /// Per-player metrics, ordered by player name, merged across shards.
    std::vector<Segment> by_player() const { return collect_segments(/*players=*/true); }
    /// Per-device metrics, ordered by device name, merged across shards.
    std::vector<Segment> by_device() const { return collect_segments(/*players=*/false); }

    /// Times a segment name was folded into kOverflowSegmentName because the
    /// cardinality cap was already reached. Non-zero means the player/device
    /// breakdown has lost resolution (totals are still exact).
    std::uint64_t segment_overflows() const noexcept {
        std::uint64_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->players.overflows() + shard->devices.overflows();
        }
        return total;
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

    /// One writer thread's private slice of the aggregate state.
    ///
    /// Over-aligned to a cache line so two shards cannot share one: without it
    /// the mutexes and hot counters of adjacent shards would land in the same
    /// line and reintroduce, as coherence traffic, the contention sharding
    /// exists to remove.
    struct alignas(64) Shard {
        explicit Shard(std::size_t max_segments)
            : players(max_segments), devices(max_segments) {}

        storage::TimeSeriesStore<processor::MetricAccumulator> store;
        SegmentMap players;
        SegmentMap devices;
    };

    /// The shard owned by the calling thread. Stable for that thread's lifetime.
    Shard& shard_for_this_thread() const;

    /// Record that @p key now exists somewhere, keeping minute_count() O(1).
    /// Called only when a shard actually creates a bucket -- once per minute per
    /// shard -- so this mutex never appears on the per-event path.
    void note_minute(const storage::MinuteKey& key);

    /// Merge the player (or device) breakdown across every shard, summing
    /// segments that share a name.
    std::vector<Segment> collect_segments(bool players) const;

    std::vector<std::unique_ptr<Shard>> shards_;

    /// Distinct minutes across all shards. The set is consulted only on bucket
    /// creation; the count is what readers actually load.
    mutable std::mutex minutes_mutex_;
    std::unordered_set<std::uint64_t> minutes_seen_;
    std::atomic<std::size_t> minute_count_{0};

    std::atomic<std::int64_t> active_sessions_{0};
};

}  // namespace pulsedb::aggregation
