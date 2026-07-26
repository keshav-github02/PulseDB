#include "pulsedb/query/metrics_api.hpp"

#include <vector>

#include "pulsedb/aggregation/aggregation_engine.hpp"
#include "pulsedb/processor/metrics.hpp"
#include "pulsedb/storage/minute_key.hpp"

namespace pulsedb::query {
namespace {

using nlohmann::json;

/// The canonical JSON shape for one set of metrics: raw sums plus derived
/// rates a dashboard plots.
///
/// @param with_ratio include buffer_ratio_per_view (= rebuffers / views).
///        That ratio is only meaningful over a complete population (platform
///        totals, per-player, per-device); it is NOT meaningful within a
///        single minute bucket, because a session's view (video_start) and
///        its later rebuffers fall in different minutes. So per-minute points
///        omit it and expose only bucket-local counts.
///
/// buffer_count and buffer_samples are both exposed because they count
/// different things: stalls that *began* in this population versus stalls that
/// *finished* in it. buffer_avg_ms divides by the latter. Dividing by the
/// former (as this did) under-reports whenever a stall crosses a bucket
/// boundary, and reported 0 ms for a bucket that held real stall time.
json to_json(const processor::MetricsSnapshot& m, bool with_ratio = true) {
    json j;
    j["total_events"] = m.total_events;
    j["total_views"] = m.total_views;
    j["startup_samples"] = m.startup_samples;
    j["startup_avg_ms"] = m.avg_startup_ms();
    j["buffer_count"] = m.buffer_count;
    j["buffer_samples"] = m.buffer_samples;
    j["buffer_avg_ms"] = m.avg_buffer_ms();
    if (with_ratio) {
        j["buffer_ratio_per_view"] = m.buffer_ratio();
    }
    j["error_count"] = m.error_count;
    j["watch_time_ms"] = m.watch_time_ms_sum;
    j["watch_time_min"] = static_cast<double>(m.watch_time_ms_sum) / 60'000.0;
    j["bitrate_avg_kbps"] = m.avg_bitrate_kbps();
    return j;
}

json points_to_json(const std::vector<aggregation::MinutePoint>& points) {
    json minutes = json::array();
    for (const auto& point : points) {
        minutes.push_back(
            {{"minute", point.key.to_iso()}, {"metrics", to_json(point.metrics, /*with_ratio=*/false)}});
    }
    return minutes;
}

json segments_to_json(const std::vector<aggregation::Segment>& segments) {
    json array = json::array();
    for (const auto& segment : segments) {
        array.push_back({{"name", segment.name}, {"metrics", to_json(segment.metrics)}});
    }
    return array;
}

}  // namespace

json MetricsApi::overall() const {
    return json{{"totals", to_json(engine_.total())},
                {"minutes_tracked", engine_.minute_count()}};
}

json MetricsApi::live(std::size_t minutes) const {
    return json{{"minutes", points_to_json(engine_.recent(minutes))}};
}

json MetricsApi::range(std::int64_t from_ms, std::int64_t to_ms) const {
    const auto from = storage::MinuteKey::from_timestamp_ms(from_ms);
    const auto to = storage::MinuteKey::from_timestamp_ms(to_ms);
    return json{{"from", from.to_iso()},
                {"to", to.to_iso()},
                {"minutes", points_to_json(engine_.range(from, to))}};
}

json MetricsApi::by_player() const {
    return json{{"players", segments_to_json(engine_.by_player())}};
}

json MetricsApi::by_device() const {
    return json{{"devices", segments_to_json(engine_.by_device())}};
}

}  // namespace pulsedb::query
