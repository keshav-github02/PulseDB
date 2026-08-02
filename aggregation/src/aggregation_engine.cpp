#include "pulsedb/aggregation/aggregation_engine.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace pulsedb::aggregation {
namespace {

/// A stable, dense slot index for the calling thread.
///
/// Assigned once on first use and never reused, which is what makes a thread's
/// shard choice stable: a thread that migrated between shards could interleave
/// writes into two of them, and while that stays correct (the counters are
/// atomics) it would defeat the point by making the per-shard mutexes contended
/// again.
///
/// Deliberately process-wide rather than per-engine. It only has to be stable
/// and well spread; every engine indexes its own shard vector with it, so two
/// engines in the same process simply agree on which thread is "thread 3".
std::size_t this_thread_slot() {
    static std::atomic<std::size_t> next_slot{0};
    static const thread_local std::size_t slot =
        next_slot.fetch_add(1, std::memory_order_relaxed);
    return slot;
}

/// Pack a minute into one integer so distinct minutes can live in a hash set.
/// Every field is bounded (year <= 9999, month <= 12, day <= 31, hour <= 23,
/// minute <= 59), so the byte-wise layout cannot collide.
std::uint64_t pack(const storage::MinuteKey& key) {
    return (static_cast<std::uint64_t>(key.year) << 32) |
           (static_cast<std::uint64_t>(key.month) << 24) |
           (static_cast<std::uint64_t>(key.day) << 16) |
           (static_cast<std::uint64_t>(key.hour) << 8) |
           static_cast<std::uint64_t>(key.minute);
}

void add_into(processor::MetricsSnapshot& sum, const processor::MetricsSnapshot& part) {
    sum.total_events += part.total_events;
    sum.total_views += part.total_views;
    sum.startup_samples += part.startup_samples;
    sum.startup_time_ms_sum += part.startup_time_ms_sum;
    sum.buffer_count += part.buffer_count;
    sum.buffer_samples += part.buffer_samples;
    sum.buffer_duration_ms_sum += part.buffer_duration_ms_sum;
    sum.error_count += part.error_count;
    sum.watch_time_ms_sum += part.watch_time_ms_sum;
    sum.bitrate_samples += part.bitrate_samples;
    sum.bitrate_kbps_sum += part.bitrate_kbps_sum;
}

}  // namespace

std::size_t AggregationEngine::default_shard_count() {
    const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    return std::clamp<std::size_t>(hw, 1, 8);
}

AggregationEngine::AggregationEngine(AggregationOptions options) {
    const std::size_t count =
        options.shards == 0 ? default_shard_count() : options.shards;
    shards_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        shards_.push_back(std::make_unique<Shard>(options.max_segments_per_dimension));
    }
}

AggregationEngine::Shard& AggregationEngine::shard_for_this_thread() const {
    return *shards_[this_thread_slot() % shards_.size()];
}

void AggregationEngine::note_minute(const storage::MinuteKey& key) {
    std::lock_guard lock(minutes_mutex_);
    if (minutes_seen_.insert(pack(key)).second) {
        // Only the first shard to reach a given minute counts it, so the total
        // stays a count of *distinct* minutes rather than of buckets allocated.
        minute_count_.store(minutes_seen_.size(), std::memory_order_relaxed);
    }
}

void AggregationEngine::process(const core::Event& event) {
    Shard& shard = shard_for_this_thread();
    const auto key = storage::MinuteKey::from_timestamp_ms(event.timestamp_ms);

    bool created = false;
    shard.store.get_or_create(key, &created).add(event);
    if (created) {
        note_minute(key);
    }

    if (!event.player.empty()) {
        shard.players.get_or_create(event.player).add(event);
    }
    if (!event.device.empty()) {
        shard.devices.get_or_create(event.device).add(event);
    }

    // Track in-flight sessions for the live "active sessions" gauge.
    if (event.type == core::EventType::kVideoStart) {
        active_sessions_.fetch_add(1, std::memory_order_relaxed);
    } else if (event.type == core::EventType::kPlaybackEnd) {
        // Floor the decrement at zero *here* rather than clamping on read.
        // Clamping on read left the stored value drifting arbitrarily negative,
        // so a burst of unmatched playback_end events -- which is guaranteed
        // after every restart, since the gauge is not persisted but the sessions
        // it was counting are still open -- left the gauge reading 0 for every
        // subsequent real video_start until the drift was paid back.
        std::int64_t current = active_sessions_.load(std::memory_order_relaxed);
        while (current > 0 && !active_sessions_.compare_exchange_weak(
                                  current, current - 1, std::memory_order_relaxed,
                                  std::memory_order_relaxed)) {
            // compare_exchange_weak refreshed `current`; re-test and retry.
        }
    }
}

std::vector<Segment> AggregationEngine::SegmentMap::snapshot() const {
    std::vector<Segment> segments;
    {
        std::lock_guard lock(mutex_);
        segments.reserve(map_.size() + 1);
        for (const auto& [name, accumulator] : map_) {
            segments.push_back(Segment{name, accumulator.snapshot()});
        }
    }
    // Surface the overflow bucket only once something has landed in it, so a
    // normal deployment never sees a spurious "__other__" row.
    //
    // Keyed on the accumulator's own contents, not on the overflows() counter: a
    // restored snapshot puts events here without any label having been folded in
    // during this process's lifetime, and gating on the counter discarded them
    // from the response entirely.
    if (const auto overflow = overflow_.snapshot(); overflow.total_events > 0) {
        segments.push_back(Segment{std::string(kOverflowSegmentName), overflow});
    }
    std::sort(segments.begin(), segments.end(),
              [](const Segment& a, const Segment& b) { return a.name < b.name; });
    return segments;
}

std::optional<processor::MetricsSnapshot> AggregationEngine::minute(
    const storage::MinuteKey& key) const {
    std::optional<processor::MetricsSnapshot> result;
    for (const auto& shard : shards_) {
        shard->store.with_bucket(key, [&result](const processor::MetricAccumulator& bucket) {
            if (!result) {
                result.emplace();
            }
            add_into(*result, bucket.snapshot());
        });
    }
    // Still nullopt when no shard holds the minute, so "no events" stays
    // distinguishable from "a bucket exists and is all zeroes".
    return result;
}

std::vector<MinutePoint> AggregationEngine::points() const {
    // Every shard may hold its own bucket for the same minute, so merge by key
    // before sorting -- otherwise the series would carry one point per shard per
    // minute, each with a fraction of that minute's events.
    std::unordered_map<std::uint64_t, MinutePoint> merged;
    for (const auto& shard : shards_) {
        shard->store.for_each([&merged](const storage::MinuteKey& key,
                                        const processor::MetricAccumulator& bucket) {
            auto [it, inserted] = merged.try_emplace(pack(key), MinutePoint{key, {}});
            add_into(it->second.metrics, bucket.snapshot());
        });
    }

    std::vector<MinutePoint> points;
    points.reserve(merged.size());
    for (auto& [_, point] : merged) {
        points.push_back(std::move(point));
    }
    std::sort(points.begin(), points.end(),
              [](const MinutePoint& a, const MinutePoint& b) { return a.key < b.key; });
    return points;
}

std::vector<MinutePoint> AggregationEngine::recent(std::size_t n) const {
    auto all = points();
    if (all.size() > n) {
        all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(n));
    }
    return all;
}

std::vector<MinutePoint> AggregationEngine::range(const storage::MinuteKey& from,
                                                  const storage::MinuteKey& to) const {
    std::vector<MinutePoint> result;
    for (auto& point : points()) {
        if (!(point.key < from) && !(to < point.key)) {
            result.push_back(point);
        }
    }
    return result;
}

// Restores all land in shard 0. They run once at startup on a single thread, so
// spreading them buys nothing, and concentrating them keeps a restored engine's
// layout independent of how many threads happened to touch it.
void AggregationEngine::restore_minute(const storage::MinuteKey& key,
                                       const processor::MetricsSnapshot& metrics) {
    bool created = false;
    shards_.front()->store.get_or_create(key, &created).add_snapshot(metrics);
    if (created) {
        note_minute(key);
    }
}

void AggregationEngine::restore_player(const std::string& name,
                                       const processor::MetricsSnapshot& metrics) {
    shards_.front()->players.get_or_create(name).add_snapshot(metrics);
}

void AggregationEngine::restore_device(const std::string& name,
                                       const processor::MetricsSnapshot& metrics) {
    shards_.front()->devices.get_or_create(name).add_snapshot(metrics);
}

processor::MetricsSnapshot AggregationEngine::total() const {
    processor::MetricsSnapshot sum;
    for (const auto& shard : shards_) {
        shard->store.for_each([&sum](const storage::MinuteKey&,
                                     const processor::MetricAccumulator& bucket) {
            add_into(sum, bucket.snapshot());
        });
    }
    return sum;
}

std::vector<Segment> AggregationEngine::collect_segments(bool players) const {
    // A name can appear in every shard, so sum by name. This is also what keeps
    // the single-overflow-row guarantee: each shard contributes at most one
    // kOverflowSegmentName entry and they merge into one.
    std::unordered_map<std::string, processor::MetricsSnapshot> merged;
    for (const auto& shard : shards_) {
        const SegmentMap& map = players ? shard->players : shard->devices;
        for (const auto& segment : map.snapshot()) {
            add_into(merged[segment.name], segment.metrics);
        }
    }

    std::vector<Segment> segments;
    segments.reserve(merged.size());
    for (auto& [name, metrics] : merged) {
        segments.push_back(Segment{name, metrics});
    }
    std::sort(segments.begin(), segments.end(),
              [](const Segment& a, const Segment& b) { return a.name < b.name; });
    return segments;
}

}  // namespace pulsedb::aggregation
