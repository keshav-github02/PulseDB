#include "pulsedb/query/metrics_api.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "pulsedb/aggregation/aggregation_engine.hpp"
#include "pulsedb/core/event.hpp"
#include "pulsedb/storage/minute_key.hpp"

namespace {

using pulsedb::aggregation::AggregationEngine;
using pulsedb::core::Event;
using pulsedb::core::EventType;
using pulsedb::query::MetricsApi;
using pulsedb::storage::MinuteKey;

constexpr std::int64_t kMinuteA = 1784556180000;  // minute-aligned
constexpr std::int64_t kMinuteB = kMinuteA + 60'000;

Event view(std::int64_t ts, const std::string& player, const std::string& device) {
    Event e;
    e.type = EventType::kVideoStart;
    e.timestamp_ms = ts;
    e.player = player;
    e.device = device;
    return e;
}

TEST(MetricsApiTest, OverallReportsTotalsAndMinuteCount) {
    AggregationEngine engine;
    engine.process(view(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view(kMinuteB, "ExoPlayer", "Android"));
    MetricsApi api{engine};

    const auto j = api.overall();
    EXPECT_EQ(j["minutes_tracked"], 2);
    EXPECT_EQ(j["totals"]["total_events"], 2);
    EXPECT_EQ(j["totals"]["total_views"], 2);
    // Derived fields are present.
    EXPECT_TRUE(j["totals"].contains("startup_avg_ms"));
    EXPECT_TRUE(j["totals"].contains("bitrate_avg_kbps"));
}

TEST(MetricsApiTest, LiveReturnsRecentMinutesInOrder) {
    AggregationEngine engine;
    for (int i = 0; i < 4; ++i) {
        engine.process(view(kMinuteA + static_cast<std::int64_t>(i) * 60'000, "P", "D"));
    }
    MetricsApi api{engine};

    const auto j = api.live(2);
    ASSERT_EQ(j["minutes"].size(), 2u);
    // Ascending by time; each entry has a label and metrics.
    const std::string first = j["minutes"][0]["minute"];
    const std::string second = j["minutes"][1]["minute"];
    EXPECT_LT(first, second);
    EXPECT_EQ(j["minutes"][0]["metrics"]["total_views"], 1);
}

TEST(MetricsApiTest, BufferRatioOnlyWhereMeaningful) {
    AggregationEngine engine;
    engine.process(view(kMinuteA, "P", "D"));
    MetricsApi api{engine};

    // Totals expose the ratio (meaningful over the whole population)...
    EXPECT_TRUE(api.overall()["totals"].contains("buffer_ratio_per_view"));
    EXPECT_TRUE(api.by_player()["players"][0]["metrics"].contains("buffer_ratio_per_view"));

    // ...but per-minute points do not (numerator/denominator are temporally
    // decoupled within a single minute).
    const auto live = api.live(5);
    ASSERT_FALSE(live["minutes"].empty());
    EXPECT_FALSE(live["minutes"][0]["metrics"].contains("buffer_ratio_per_view"));
    // Bucket-local counts are still present.
    EXPECT_TRUE(live["minutes"][0]["metrics"].contains("buffer_count"));
}

// The response must expose both stall counters, because they answer different
// questions and only one of them is the denominator of buffer_avg_ms.
TEST(MetricsApiTest, ExposesBothStallCountersAndTheCorrectAverage) {
    AggregationEngine engine;
    // One stall begins in minute A and finishes in minute B, lasting 1200 ms.
    Event start;
    start.type = EventType::kBufferStart;
    start.timestamp_ms = kMinuteA;
    engine.process(start);
    Event end;
    end.type = EventType::kBufferEnd;
    end.timestamp_ms = kMinuteB;
    end.buffer_duration_ms = 1200;
    engine.process(end);

    MetricsApi api{engine};
    const auto totals = api.overall()["totals"];
    EXPECT_EQ(totals["buffer_count"], 1u);
    EXPECT_EQ(totals["buffer_samples"], 1u);
    EXPECT_DOUBLE_EQ(totals["buffer_avg_ms"].get<double>(), 1200.0);

    const auto minutes = api.live(5)["minutes"];
    ASSERT_EQ(minutes.size(), 2u);
    // Minute A: the stall began here, so it is counted but has no sample.
    EXPECT_EQ(minutes[0]["metrics"]["buffer_count"], 1u);
    EXPECT_EQ(minutes[0]["metrics"]["buffer_samples"], 0u);
    EXPECT_DOUBLE_EQ(minutes[0]["metrics"]["buffer_avg_ms"].get<double>(), 0.0);
    // Minute B: the stall finished here, so the duration is visible.
    EXPECT_EQ(minutes[1]["metrics"]["buffer_count"], 0u);
    EXPECT_EQ(minutes[1]["metrics"]["buffer_samples"], 1u);
    EXPECT_DOUBLE_EQ(minutes[1]["metrics"]["buffer_avg_ms"].get<double>(), 1200.0);
}

TEST(MetricsApiTest, RangeEchoesBoundsAndFilters) {
    AggregationEngine engine;
    for (int i = 0; i < 5; ++i) {
        engine.process(view(kMinuteA + static_cast<std::int64_t>(i) * 60'000, "P", "D"));
    }
    MetricsApi api{engine};

    const auto j = api.range(kMinuteA + 60'000, kMinuteA + 3 * 60'000);
    EXPECT_EQ(j["from"], MinuteKey::from_timestamp_ms(kMinuteA + 60'000).to_iso());
    EXPECT_EQ(j["minutes"].size(), 3u);
}

TEST(MetricsApiTest, PlayerBreakdownJson) {
    AggregationEngine engine;
    engine.process(view(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view(kMinuteA, "AVPlayer", "iOS"));
    MetricsApi api{engine};

    const auto j = api.by_player();
    ASSERT_EQ(j["players"].size(), 2u);
    EXPECT_EQ(j["players"][0]["name"], "AVPlayer");
    EXPECT_EQ(j["players"][1]["name"], "ExoPlayer");
    EXPECT_EQ(j["players"][1]["metrics"]["total_views"], 2);
}

TEST(MetricsApiTest, DeviceBreakdownJson) {
    AggregationEngine engine;
    engine.process(view(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view(kMinuteA, "AVPlayer", "iOS"));
    MetricsApi api{engine};

    const auto j = api.by_device();
    ASSERT_EQ(j["devices"].size(), 2u);
    EXPECT_EQ(j["devices"][0]["name"], "Android");
}

// Every mean must ship with the sample count it was divided by, so a consumer
// combining populations can weight them. bitrate_avg_kbps was exposed without
// bitrate_samples, which left a caller no way to fold minute buckets into a
// windowed total except by averaging the averages -- wrong whenever buckets hold
// unequal sample counts, and wrong silently.
TEST(MetricsApiTest, EveryAverageShipsWithItsSampleCount) {
    AggregationEngine engine;
    Event bitrate;
    bitrate.type = EventType::kBitrateChange;
    bitrate.timestamp_ms = kMinuteA;
    bitrate.bitrate_kbps = 3000;
    engine.process(bitrate);
    MetricsApi api{engine};

    for (const auto& population :
         {api.overall()["totals"], api.live(10)["minutes"][0]["metrics"]}) {
        for (const auto& [mean, samples] :
             {std::pair{"startup_avg_ms", "startup_samples"},
              std::pair{"buffer_avg_ms", "buffer_samples"},
              std::pair{"bitrate_avg_kbps", "bitrate_samples"}}) {
            EXPECT_TRUE(population.contains(mean)) << mean;
            EXPECT_TRUE(population.contains(samples))
                << samples << " is missing, so " << mean << " cannot be re-weighted";
        }
    }
}

TEST(MetricsApiTest, WeightedAverageIsReconstructibleFromBuckets) {
    // Two minutes with very different sample counts: averaging the averages
    // gives 2000, weighting by samples gives 1100. Only the latter is right.
    AggregationEngine engine;
    const auto add_bitrate = [&engine](std::int64_t ts, int kbps, int times) {
        for (int i = 0; i < times; ++i) {
            Event e;
            e.type = EventType::kBitrateChange;
            e.timestamp_ms = ts;
            e.bitrate_kbps = kbps;
            engine.process(e);
        }
    };
    add_bitrate(kMinuteA, 1000, 9);
    add_bitrate(kMinuteB, 10000, 1);

    MetricsApi api{engine};
    const auto minutes = api.live(10)["minutes"];
    ASSERT_EQ(minutes.size(), 2u);

    double weighted = 0.0;
    std::uint64_t samples = 0;
    for (const auto& point : minutes) {
        const auto& m = point["metrics"];
        weighted += m["bitrate_avg_kbps"].get<double>() * m["bitrate_samples"].get<double>();
        samples += m["bitrate_samples"].get<std::uint64_t>();
    }
    ASSERT_EQ(samples, 10u);
    EXPECT_DOUBLE_EQ(weighted / static_cast<double>(samples),
                     api.overall()["totals"]["bitrate_avg_kbps"].get<double>())
        << "re-weighting the buckets must reproduce the platform-wide mean";
}

}  // namespace
