#include "pulsedb/persistence/metrics_snapshot_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "pulsedb/aggregation/aggregation_engine.hpp"
#include "pulsedb/core/event.hpp"
#include "pulsedb/storage/minute_key.hpp"

namespace {

using pulsedb::aggregation::AggregationEngine;
using pulsedb::core::Event;
using pulsedb::core::EventType;
using pulsedb::storage::MinuteKey;
namespace fs = std::filesystem;
namespace persistence = pulsedb::persistence;

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

// Populate an engine with a known, multi-bucket, multi-segment workload.
void populate(AggregationEngine& engine) {
    engine.process(view(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view(kMinuteB, "AVPlayer", "iOS"));

    Event startup;
    startup.type = EventType::kStartupComplete;
    startup.timestamp_ms = kMinuteA;
    startup.player = "ExoPlayer";
    startup.device = "Android";
    startup.startup_time_ms = 2000;
    engine.process(startup);
}

fs::path unique_temp_dir() {
    static std::atomic<int> counter{0};
    const auto dir = fs::temp_directory_path() /
                     ("pulsedb_persist_test_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(dir);
    return dir;
}

TEST(MetricsSnapshotStoreTest, JsonRoundTripPreservesState) {
    AggregationEngine source;
    populate(source);

    const auto json = persistence::to_json(source);

    AggregationEngine restored;
    ASSERT_TRUE(persistence::restore(restored, json));

    // Totals match.
    EXPECT_EQ(restored.total().total_events, source.total().total_events);
    EXPECT_EQ(restored.total().total_views, source.total().total_views);
    EXPECT_EQ(restored.total().startup_time_ms_sum, source.total().startup_time_ms_sum);

    // Per-minute bucket matches.
    EXPECT_EQ(restored.minute_count(), source.minute_count());
    const auto key_a = MinuteKey::from_timestamp_ms(kMinuteA);
    ASSERT_TRUE(restored.minute(key_a).has_value());
    EXPECT_EQ(restored.minute(key_a)->total_views, 2u);
    EXPECT_EQ(restored.minute(key_a)->startup_samples, 1u);

    // Segments match.
    ASSERT_EQ(restored.by_player().size(), source.by_player().size());
    ASSERT_EQ(restored.by_device().size(), source.by_device().size());
}

TEST(MetricsSnapshotStoreTest, SaveThenLoadRestoresState) {
    const auto dir = unique_temp_dir();
    const auto path = dir / "metrics.json";

    AggregationEngine source;
    populate(source);
    ASSERT_TRUE(persistence::save(source, path));
    EXPECT_TRUE(fs::exists(path));

    AggregationEngine restored;
    ASSERT_TRUE(persistence::load(restored, path));
    EXPECT_EQ(restored.total().total_events, source.total().total_events);
    EXPECT_EQ(restored.minute_count(), source.minute_count());

    fs::remove_all(dir);
}

TEST(MetricsSnapshotStoreTest, LoadMissingFileReturnsFalse) {
    AggregationEngine engine;
    EXPECT_FALSE(persistence::load(engine, unique_temp_dir() / "nope.json"));
    EXPECT_EQ(engine.total().total_events, 0u);
}

TEST(MetricsSnapshotStoreTest, LoadCorruptFileReturnsFalseAndLeavesEngine) {
    const auto dir = unique_temp_dir();
    fs::create_directories(dir);
    const auto path = dir / "corrupt.json";
    {
        std::ofstream out(path);
        out << "{ this is not valid json ";
    }

    AggregationEngine engine;
    engine.process(view(kMinuteA, "ExoPlayer", "Android"));
    EXPECT_FALSE(persistence::load(engine, path));
    EXPECT_EQ(engine.total().total_events, 1u);  // unchanged

    fs::remove_all(dir);
}

// --- Durability and validation (C5) -----------------------------------------

TEST(MetricsSnapshotStoreTest, RejectsUnsupportedVersion) {
    AggregationEngine source;
    populate(source);
    auto json = persistence::to_json(source);
    json["version"] = persistence::kSnapshotVersion + 1;

    AggregationEngine engine;
    std::string error;
    EXPECT_FALSE(persistence::restore(engine, json, &error));
    EXPECT_NE(error.find("unsupported snapshot version"), std::string::npos) << error;
    EXPECT_EQ(engine.total().total_events, 0u) << "nothing may be applied";
}

// --- Version migration (v1 -> v2) -------------------------------------------

// Rewrite a current snapshot as the v1 it would have been: version 1, and no
// buffer_samples anywhere (the counter v2 introduced).
nlohmann::json downgrade_to_v1(nlohmann::json snapshot) {
    snapshot["version"] = 1;
    const auto strip = [](nlohmann::json& entries) {
        for (auto& entry : entries) {
            entry["m"].erase("buffer_samples");
        }
    };
    strip(snapshot["minutes"]);
    strip(snapshot["players"]);
    strip(snapshot["devices"]);
    return snapshot;
}

// A v1 file must still load. Discarding history on a purely additive format
// change would make the upgrade itself the most destructive event in the
// system's life -- worse than the bug v2 fixes.
TEST(MetricsSnapshotStoreTest, RestoresV1SnapshotWithBufferSamplesDefaultedToZero) {
    AggregationEngine source;
    populate(source);
    Event stall_end;
    stall_end.type = EventType::kBufferEnd;
    stall_end.timestamp_ms = kMinuteA;
    stall_end.player = "ExoPlayer";
    stall_end.device = "Android";
    stall_end.buffer_duration_ms = 900;
    source.process(stall_end);

    const auto v1 = downgrade_to_v1(persistence::to_json(source));
    ASSERT_FALSE(v1["minutes"][0]["m"].contains("buffer_samples"));

    AggregationEngine engine;
    std::string error;
    ASSERT_TRUE(persistence::restore(engine, v1, &error)) << error;

    const auto total = engine.total();
    // Every v1 counter round-trips exactly.
    EXPECT_EQ(total.total_events, source.total().total_events);
    EXPECT_EQ(total.total_views, source.total().total_views);
    EXPECT_EQ(total.buffer_duration_ms_sum, 900u);
    // The v2 counter is unknowable from a v1 file, so it restores as 0 -- which
    // makes the mean report "no data" rather than a number derived from the
    // mismatched denominator v1 was using.
    EXPECT_EQ(total.buffer_samples, 0u);
    EXPECT_DOUBLE_EQ(total.avg_buffer_ms(), 0.0);
}

// Back-compat must not have loosened strictness: a v1 file missing a counter v1
// itself defined is still corrupt.
TEST(MetricsSnapshotStoreTest, V1SnapshotMissingAnOriginalCounterIsStillRejected) {
    AggregationEngine source;
    populate(source);
    auto v1 = downgrade_to_v1(persistence::to_json(source));
    v1["minutes"][0]["m"].erase("total_views");

    AggregationEngine engine;
    std::string error;
    EXPECT_FALSE(persistence::restore(engine, v1, &error));
    EXPECT_NE(error.find("total_views"), std::string::npos) << error;
    EXPECT_EQ(engine.total().total_events, 0u);
}

// And a file claiming to be v2 must actually carry v2's counters.
TEST(MetricsSnapshotStoreTest, V2SnapshotMissingBufferSamplesIsRejected) {
    AggregationEngine source;
    populate(source);
    auto json = persistence::to_json(source);
    ASSERT_EQ(json["version"], 2);
    json["minutes"][0]["m"].erase("buffer_samples");

    AggregationEngine engine;
    std::string error;
    EXPECT_FALSE(persistence::restore(engine, json, &error));
    EXPECT_NE(error.find("buffer_samples"), std::string::npos) << error;
}

TEST(MetricsSnapshotStoreTest, BufferSamplesSurviveARoundTrip) {
    AggregationEngine source;
    for (int i = 0; i < 3; ++i) {
        Event end;
        end.type = EventType::kBufferEnd;
        end.timestamp_ms = kMinuteA;
        end.buffer_duration_ms = 500;
        source.process(end);
    }
    AggregationEngine engine;
    std::string error;
    ASSERT_TRUE(persistence::restore(engine, persistence::to_json(source), &error)) << error;

    const auto total = engine.total();
    EXPECT_EQ(total.buffer_samples, 3u);
    EXPECT_EQ(total.buffer_duration_ms_sum, 1500u);
    EXPECT_DOUBLE_EQ(total.avg_buffer_ms(), 500.0);
}

TEST(MetricsSnapshotStoreTest, RejectsSnapshotWithoutVersion) {
    AggregationEngine source;
    populate(source);
    auto json = persistence::to_json(source);
    json.erase("version");

    AggregationEngine engine;
    EXPECT_FALSE(persistence::restore(engine, json, nullptr));
    EXPECT_EQ(engine.total().total_events, 0u);
}

// The regression that mattered most: restore used to apply entries as it walked
// them, so a bad entry midway left the engine holding a fraction of a snapshot
// and silently wrong totals, forever.
TEST(MetricsSnapshotStoreTest, RestoreIsAllOrNothingOnAMalformedEntry) {
    AggregationEngine source;
    populate(source);
    auto json = persistence::to_json(source);
    ASSERT_GE(json["minutes"].size(), 2u);
    json["minutes"][1]["m"]["total_views"] = "not a number";  // corrupt the second

    AggregationEngine engine;
    std::string error;
    EXPECT_FALSE(persistence::restore(engine, json, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(engine.total().total_events, 0u)
        << "the valid first entry must not have been applied either";
    EXPECT_EQ(engine.minute_count(), 0u);
}

TEST(MetricsSnapshotStoreTest, RejectsOutOfRangeCalendarFields) {
    AggregationEngine source;
    populate(source);
    for (const char* field : {"mo", "d", "h", "mi"}) {
        auto json = persistence::to_json(source);
        json["minutes"][0][field] = 999;  // out of range for every one of them

        AggregationEngine engine;
        std::string error;
        EXPECT_FALSE(persistence::restore(engine, json, &error)) << field;
        EXPECT_EQ(engine.total().total_events, 0u) << field;
    }
}

TEST(MetricsSnapshotStoreTest, RejectsSegmentEntryMissingItsName) {
    AggregationEngine source;
    populate(source);
    auto json = persistence::to_json(source);
    ASSERT_FALSE(json["players"].empty());
    json["players"][0].erase("name");

    AggregationEngine engine;
    EXPECT_FALSE(persistence::restore(engine, json, nullptr));
    EXPECT_EQ(engine.total().total_events, 0u);
}

TEST(MetricsSnapshotStoreTest, SaveLeavesNoTempFileBehind) {
    const auto dir = unique_temp_dir();
    const auto path = dir / "metrics.json";

    AggregationEngine source;
    populate(source);
    ASSERT_TRUE(persistence::save(source, path));
    EXPECT_TRUE(fs::exists(path));
    EXPECT_FALSE(fs::exists(fs::path(path.string() + ".tmp")));

    fs::remove_all(dir);
}

// A failed save must not destroy the snapshot already on disk.
TEST(MetricsSnapshotStoreTest, FailedSavePreservesThePreviousSnapshot) {
    const auto dir = unique_temp_dir();
    const auto path = dir / "metrics.json";

    AggregationEngine source;
    populate(source);
    ASSERT_TRUE(persistence::save(source, path));
    const auto good = [&] {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    ASSERT_FALSE(good.empty());

    // Make the temp path un-openable as a file by occupying it with a directory,
    // which is the most portable way to force the write to fail.
    const fs::path temp = path.string() + ".tmp";
    fs::create_directories(temp);

    std::string error;
    EXPECT_FALSE(persistence::save(source, path, &error));
    EXPECT_FALSE(error.empty());

    std::ifstream in(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(in), {}};
    EXPECT_EQ(after, good) << "the previous snapshot must survive a failed save";

    in.close();
    fs::remove_all(dir);
}

TEST(MetricsSnapshotStoreTest, LoadReportsWhyItFailed) {
    AggregationEngine engine;
    std::string error;
    EXPECT_FALSE(persistence::load(engine, unique_temp_dir() / "nope.json", &error));
    EXPECT_NE(error.find("no snapshot"), std::string::npos) << error;
}

TEST(MetricsSnapshotStoreTest, RestoreIsAdditive) {
    // Restoring into an engine that already has data accumulates (models a
    // server that restored a snapshot and then kept ingesting).
    AggregationEngine source;
    populate(source);
    const auto json = persistence::to_json(source);

    AggregationEngine engine;
    populate(engine);  // same workload again
    ASSERT_TRUE(persistence::restore(engine, json));

    EXPECT_EQ(engine.total().total_events, 2 * source.total().total_events);
}

}  // namespace
