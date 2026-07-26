#include "pulsedb/aggregation/aggregation_engine.hpp"

#include <algorithm>

namespace pulsedb::aggregation {
namespace {

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

void AggregationEngine::process(const core::Event& event) {
    const auto key = storage::MinuteKey::from_timestamp_ms(event.timestamp_ms);
    store_.get_or_create(key).add(event);

    if (!event.player.empty()) {
        players_.get_or_create(event.player).add(event);
    }
    if (!event.device.empty()) {
        devices_.get_or_create(event.device).add(event);
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
    if (overflows() > 0) {
        segments.push_back(Segment{std::string(kOverflowSegmentName), overflow_.snapshot()});
    }
    std::sort(segments.begin(), segments.end(),
              [](const Segment& a, const Segment& b) { return a.name < b.name; });
    return segments;
}

std::optional<processor::MetricsSnapshot> AggregationEngine::minute(
    const storage::MinuteKey& key) const {
    std::optional<processor::MetricsSnapshot> result;
    store_.with_bucket(key, [&result](const processor::MetricAccumulator& bucket) {
        result = bucket.snapshot();
    });
    return result;
}

std::vector<MinutePoint> AggregationEngine::points() const {
    std::vector<MinutePoint> points;
    store_.for_each([&points](const storage::MinuteKey& key,
                              const processor::MetricAccumulator& bucket) {
        points.push_back(MinutePoint{key, bucket.snapshot()});
    });
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

void AggregationEngine::restore_minute(const storage::MinuteKey& key,
                                       const processor::MetricsSnapshot& metrics) {
    store_.get_or_create(key).add_snapshot(metrics);
}

void AggregationEngine::restore_player(const std::string& name,
                                       const processor::MetricsSnapshot& metrics) {
    players_.get_or_create(name).add_snapshot(metrics);
}

void AggregationEngine::restore_device(const std::string& name,
                                       const processor::MetricsSnapshot& metrics) {
    devices_.get_or_create(name).add_snapshot(metrics);
}

processor::MetricsSnapshot AggregationEngine::total() const {
    processor::MetricsSnapshot sum;
    store_.for_each([&sum](const storage::MinuteKey&,
                           const processor::MetricAccumulator& bucket) {
        add_into(sum, bucket.snapshot());
    });
    return sum;
}

}  // namespace pulsedb::aggregation
