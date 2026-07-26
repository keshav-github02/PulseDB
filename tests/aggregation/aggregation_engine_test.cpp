#include "pulsedb/aggregation/aggregation_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "pulsedb/core/event.hpp"
#include "pulsedb/storage/minute_key.hpp"

namespace {

using pulsedb::aggregation::AggregationEngine;
using pulsedb::core::Event;
using pulsedb::core::EventType;
using pulsedb::storage::MinuteKey;

// Two consecutive minute-aligned timestamps (kMinuteA % 60000 == 0), so
// +30s stays in the same bucket and +60s advances to the next.
constexpr std::int64_t kMinuteA = 1784556180000;
constexpr std::int64_t kMinuteB = kMinuteA + 60'000;

Event view_at(std::int64_t ts_ms) {
    Event e;
    e.type = EventType::kVideoStart;
    e.timestamp_ms = ts_ms;
    return e;
}

TEST(AggregationEngineTest, BucketsEventsByMinute) {
    AggregationEngine engine;
    engine.process(view_at(kMinuteA));
    engine.process(view_at(kMinuteA + 30'000));  // same minute
    engine.process(view_at(kMinuteB));           // next minute

    EXPECT_EQ(engine.minute_count(), 2u);

    const auto key_a = MinuteKey::from_timestamp_ms(kMinuteA);
    const auto a = engine.minute(key_a);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->total_views, 2u);

    const auto key_b = MinuteKey::from_timestamp_ms(kMinuteB);
    const auto b = engine.minute(key_b);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->total_views, 1u);
}

TEST(AggregationEngineTest, MinuteWithNoEventsIsNullopt) {
    AggregationEngine engine;
    engine.process(view_at(kMinuteA));
    EXPECT_FALSE(engine.minute(MinuteKey{1999, 1, 1, 0, 0}).has_value());
}

TEST(AggregationEngineTest, TotalSumsAllBuckets) {
    AggregationEngine engine;
    engine.process(view_at(kMinuteA));
    engine.process(view_at(kMinuteB));
    engine.process(view_at(kMinuteB));

    const auto total = engine.total();
    EXPECT_EQ(total.total_events, 3u);
    EXPECT_EQ(total.total_views, 3u);
}

TEST(AggregationEngineTest, PointsAreOrderedByTime) {
    AggregationEngine engine;
    engine.process(view_at(kMinuteB));  // insert out of order
    engine.process(view_at(kMinuteA));

    const auto points = engine.points();
    ASSERT_EQ(points.size(), 2u);
    EXPECT_LT(points[0].key, points[1].key);
    EXPECT_EQ(points[0].key, MinuteKey::from_timestamp_ms(kMinuteA));
}

TEST(AggregationEngineTest, RecentReturnsTailInOrder) {
    AggregationEngine engine;
    for (int i = 0; i < 5; ++i) {
        engine.process(view_at(kMinuteA + static_cast<std::int64_t>(i) * 60'000));
    }
    const auto recent = engine.recent(2);
    ASSERT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent[0].key, MinuteKey::from_timestamp_ms(kMinuteA + 3 * 60'000));
    EXPECT_EQ(recent[1].key, MinuteKey::from_timestamp_ms(kMinuteA + 4 * 60'000));
}

TEST(AggregationEngineTest, RangeFiltersInclusive) {
    AggregationEngine engine;
    for (int i = 0; i < 5; ++i) {
        engine.process(view_at(kMinuteA + static_cast<std::int64_t>(i) * 60'000));
    }
    const auto from = MinuteKey::from_timestamp_ms(kMinuteA + 60'000);
    const auto to = MinuteKey::from_timestamp_ms(kMinuteA + 3 * 60'000);
    const auto range = engine.range(from, to);
    EXPECT_EQ(range.size(), 3u);  // minutes 1, 2, 3
}

TEST(AggregationEngineTest, ComputesTypedMetricsPerMinute) {
    AggregationEngine engine;
    Event startup;
    startup.type = EventType::kStartupComplete;
    startup.timestamp_ms = kMinuteA;
    startup.startup_time_ms = 2000;
    engine.process(startup);

    Event buffer;
    buffer.type = EventType::kBufferStart;
    buffer.timestamp_ms = kMinuteA + 1000;
    engine.process(buffer);

    const auto m = engine.minute(MinuteKey::from_timestamp_ms(kMinuteA));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->startup_samples, 1u);
    EXPECT_DOUBLE_EQ(m->avg_startup_ms(), 2000.0);
    EXPECT_EQ(m->buffer_count, 1u);
}

Event view_on(std::int64_t ts_ms, const std::string& player, const std::string& device) {
    Event e = view_at(ts_ms);
    e.player = player;
    e.device = device;
    return e;
}

TEST(AggregationEngineTest, SegmentsByPlayer) {
    AggregationEngine engine;
    engine.process(view_on(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view_on(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view_on(kMinuteA, "AVPlayer", "iOS"));

    const auto players = engine.by_player();
    ASSERT_EQ(players.size(), 2u);
    // snapshot() returns segments sorted by name: AVPlayer, ExoPlayer.
    EXPECT_EQ(players[0].name, "AVPlayer");
    EXPECT_EQ(players[0].metrics.total_views, 1u);
    EXPECT_EQ(players[1].name, "ExoPlayer");
    EXPECT_EQ(players[1].metrics.total_views, 2u);
}

TEST(AggregationEngineTest, SegmentsByDevice) {
    AggregationEngine engine;
    engine.process(view_on(kMinuteA, "ExoPlayer", "Android"));
    engine.process(view_on(kMinuteA, "AVPlayer", "iOS"));
    engine.process(view_on(kMinuteA, "Shaka", "iOS"));

    const auto devices = engine.by_device();
    ASSERT_EQ(devices.size(), 2u);
    EXPECT_EQ(devices[0].name, "Android");
    EXPECT_EQ(devices[0].metrics.total_views, 1u);
    EXPECT_EQ(devices[1].name, "iOS");
    EXPECT_EQ(devices[1].metrics.total_views, 2u);
}

// --- Cardinality cap (C3) ---------------------------------------------------

TEST(AggregationEngineTest, CapsSegmentCardinalityAndFoldsTheRestIntoOther) {
    AggregationEngine engine{{.max_segments_per_dimension = 3}};
    for (int i = 0; i < 50; ++i) {
        engine.process(view_on(kMinuteA, "player-" + std::to_string(i), "Android"));
    }

    const auto players = engine.by_player();
    // 3 capped names + the overflow segment.
    ASSERT_EQ(players.size(), 4u);
    EXPECT_GT(engine.segment_overflows(), 0u);

    const auto overflow = std::find_if(
        players.begin(), players.end(),
        [](const auto& s) { return s.name == pulsedb::aggregation::kOverflowSegmentName; });
    ASSERT_NE(overflow, players.end()) << "expected an __other__ segment";
    EXPECT_EQ(overflow->metrics.total_views, 47u);  // 50 - 3 tracked by name

    // Folding must not lose events: totals stay exact.
    EXPECT_EQ(engine.total().total_views, 50u);
    std::uint64_t summed = 0;
    for (const auto& s : players) {
        summed += s.metrics.total_views;
    }
    EXPECT_EQ(summed, 50u);
}

TEST(AggregationEngineTest, NoOverflowSegmentWhenUnderTheCap) {
    AggregationEngine engine;  // default cap is 1000
    engine.process(view_on(kMinuteA, "ExoPlayer", "Android"));
    EXPECT_EQ(engine.segment_overflows(), 0u);
    for (const auto& s : engine.by_player()) {
        EXPECT_NE(s.name, pulsedb::aggregation::kOverflowSegmentName);
    }
}

TEST(AggregationEngineTest, CardinalityCapBoundsMemoryUnderAUniqueLabelFlood) {
    // The attack the cap exists for: a unique player name on every event.
    AggregationEngine engine{{.max_segments_per_dimension = 10}};
    for (int i = 0; i < 5'000; ++i) {
        engine.process(view_on(kMinuteA, "u-" + std::to_string(i), "d-" + std::to_string(i)));
    }
    EXPECT_EQ(engine.by_player().size(), 11u);  // 10 + __other__
    EXPECT_EQ(engine.by_device().size(), 11u);
    EXPECT_EQ(engine.total().total_views, 5'000u);
}

TEST(AggregationEngineTest, IgnoresEmptySegmentNames) {
    AggregationEngine engine;
    engine.process(view_at(kMinuteA));  // no player/device set
    EXPECT_TRUE(engine.by_player().empty());
    EXPECT_TRUE(engine.by_device().empty());
}

Event event_of(EventType type, std::int64_t ts_ms) {
    Event e;
    e.type = type;
    e.timestamp_ms = ts_ms;
    return e;
}

TEST(AggregationEngineTest, TracksActiveSessions) {
    AggregationEngine engine;
    EXPECT_EQ(engine.active_sessions(), 0);

    engine.process(event_of(EventType::kVideoStart, kMinuteA));
    engine.process(event_of(EventType::kVideoStart, kMinuteA));
    EXPECT_EQ(engine.active_sessions(), 2);

    engine.process(event_of(EventType::kPlaybackEnd, kMinuteB));
    EXPECT_EQ(engine.active_sessions(), 1);
}

TEST(AggregationEngineTest, ActiveSessionsClampAtZero) {
    AggregationEngine engine;
    engine.process(event_of(EventType::kPlaybackEnd, kMinuteA));  // end without start
    EXPECT_EQ(engine.active_sessions(), 0);
}

// Regression: the gauge used to clamp on *read* while the stored value drifted
// arbitrarily negative, so unmatched playback_end events left it reporting 0 for
// every subsequent real video_start until the drift was paid back. This is not a
// corner case -- it happens on every restart, because the gauge is not persisted
// but the sessions it was counting are still open, so their playback_end events
// all arrive unmatched.
//
// The previous test passed throughout: it asserted the observable ("reads 0")
// rather than the invariant ("never goes below zero"), so it confirmed the
// symptom instead of catching the bug.
TEST(AggregationEngineTest, ActiveSessionsRecoverAfterUnmatchedEnds) {
    AggregationEngine engine;
    for (int i = 0; i < 3; ++i) {
        engine.process(event_of(EventType::kPlaybackEnd, kMinuteA));
    }
    ASSERT_EQ(engine.active_sessions(), 0) << "floored, not driven to -3";

    engine.process(event_of(EventType::kVideoStart, kMinuteB));
    engine.process(event_of(EventType::kVideoStart, kMinuteB));
    EXPECT_EQ(engine.active_sessions(), 2)
        << "two real sessions must be visible immediately, not absorbed by drift";
}

// The floor must hold under concurrency too: a CAS loop that gave up on
// contention could still let the counter go negative.
TEST(AggregationEngineTest, ActiveSessionsNeverGoNegativeUnderConcurrency) {
    AggregationEngine engine;
    constexpr int kThreads = 8;
    constexpr int kEndsPerThread = 20'000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&engine] {
            for (int i = 0; i < kEndsPerThread; ++i) {
                engine.process(event_of(EventType::kPlaybackEnd, kMinuteA));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(engine.active_sessions(), 0);
    engine.process(event_of(EventType::kVideoStart, kMinuteB));
    EXPECT_EQ(engine.active_sessions(), 1);
}

// Regression: mean stall duration used to divide the sum of buffer_end payloads
// by the count of buffer_start events -- two different populations. A stall that
// began in one minute and ended in the next put the count in bucket A and the
// duration in bucket B, so A reported 0 ms (sum 0) and B reported 0 ms (count 0),
// and 2000 ms of measured rebuffering was invisible in both.
TEST(AggregationEngineTest, BufferAverageUsesEndSamplesNotStartCounts) {
    AggregationEngine engine;

    // Two stalls begin in minute A and finish in minute B, 1000 ms each.
    engine.process(event_of(EventType::kBufferStart, kMinuteA));
    engine.process(event_of(EventType::kBufferStart, kMinuteA));
    for (int i = 0; i < 2; ++i) {
        Event end = event_of(EventType::kBufferEnd, kMinuteB);
        end.buffer_duration_ms = 1000;
        engine.process(end);
    }

    const auto a = engine.minute(MinuteKey::from_timestamp_ms(kMinuteA));
    const auto b = engine.minute(MinuteKey::from_timestamp_ms(kMinuteB));
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    // Minute A saw the stalls begin: it counts them but has no duration samples.
    EXPECT_EQ(a->buffer_count, 2u);
    EXPECT_EQ(a->buffer_samples, 0u);
    EXPECT_DOUBLE_EQ(a->avg_buffer_ms(), 0.0) << "no samples here, so no average";

    // Minute B saw them finish: it has the samples, and reports the true mean.
    EXPECT_EQ(b->buffer_count, 0u);
    EXPECT_EQ(b->buffer_samples, 2u);
    EXPECT_DOUBLE_EQ(b->avg_buffer_ms(), 1000.0)
        << "stall time must be visible in the bucket that holds the samples";

    // And the platform total is right rather than accidentally right.
    const auto total = engine.total();
    EXPECT_EQ(total.buffer_count, 2u);
    EXPECT_EQ(total.buffer_samples, 2u);
    EXPECT_DOUBLE_EQ(total.avg_buffer_ms(), 1000.0);
}

// A stall begun but never finished (an abandoned session) must not drag the mean
// down: it contributes to buffer_count and to no sample.
TEST(AggregationEngineTest, AbandonedStallDoesNotBiasTheAverage) {
    AggregationEngine engine;
    Event end = event_of(EventType::kBufferEnd, kMinuteA);
    end.buffer_duration_ms = 800;
    engine.process(event_of(EventType::kBufferStart, kMinuteA));
    engine.process(end);
    engine.process(event_of(EventType::kBufferStart, kMinuteA));  // never ends

    const auto total = engine.total();
    EXPECT_EQ(total.buffer_count, 2u);
    EXPECT_EQ(total.buffer_samples, 1u);
    EXPECT_DOUBLE_EQ(total.avg_buffer_ms(), 800.0)
        << "dividing by buffer_count would have reported 400 ms";
}

// Regression (CB-1): a negative payload used to be static_cast to uint64_t,
// yielding ~1.8e19 and permanently destroying the average. The counters only
// grow and are never recomputed, so this was irreversible -- and it survived a
// restart, because the poisoned value is a legitimate unsigned integer that
// round-trips through the snapshot.
TEST(AggregationEngineTest, NegativePayloadsCannotPoisonCounters) {
    AggregationEngine engine;

    Event poison = event_of(EventType::kStartupComplete, kMinuteA);
    poison.startup_time_ms = -1;
    engine.process(poison);

    auto total = engine.total();
    EXPECT_EQ(total.startup_samples, 0u) << "a negative sample is not a sample";
    EXPECT_EQ(total.startup_time_ms_sum, 0u);
    EXPECT_DOUBLE_EQ(total.avg_startup_ms(), 0.0);

    // A real sample afterwards must still produce the correct average, i.e. the
    // poison left no residue.
    Event good = event_of(EventType::kStartupComplete, kMinuteA);
    good.startup_time_ms = 1500;
    engine.process(good);

    total = engine.total();
    EXPECT_EQ(total.startup_samples, 1u);
    EXPECT_DOUBLE_EQ(total.avg_startup_ms(), 1500.0);
}

// The same guard must hold for every numeric payload, since each feeds an
// unsigned counter.
TEST(AggregationEngineTest, EveryNegativeNumericPayloadIsIgnored) {
    AggregationEngine engine;

    Event buffering = event_of(EventType::kBufferEnd, kMinuteA);
    buffering.buffer_duration_ms = -5;
    Event bitrate = event_of(EventType::kBitrateChange, kMinuteA);
    bitrate.bitrate_kbps = -3000;
    Event watch = event_of(EventType::kPlaybackEnd, kMinuteA);
    watch.watch_time_ms = -60'000;

    engine.process(buffering);
    engine.process(bitrate);
    engine.process(watch);

    const auto total = engine.total();
    EXPECT_EQ(total.buffer_samples, 0u);
    EXPECT_EQ(total.buffer_duration_ms_sum, 0u);
    EXPECT_EQ(total.bitrate_samples, 0u);
    EXPECT_EQ(total.bitrate_kbps_sum, 0u);
    EXPECT_EQ(total.watch_time_ms_sum, 0u);
    // The events themselves are still counted -- they happened, their payloads
    // were just unusable.
    EXPECT_EQ(total.total_events, 3u);
}

TEST(AggregationEngineTest, IsThreadSafeAcrossMinutes) {
    AggregationEngine engine;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20'000;
    constexpr int kMinutes = 10;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&engine] {
            for (int i = 0; i < kPerThread; ++i) {
                engine.process(view_at(kMinuteA + static_cast<std::int64_t>(i % kMinutes) * 60'000));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(engine.minute_count(), static_cast<std::size_t>(kMinutes));
    EXPECT_EQ(engine.total().total_views,
              static_cast<std::uint64_t>(kThreads) * kPerThread);
}

// Regression: the store used to be guarded by std::shared_mutex, which on
// MinGW/winpthreads intermittently fails to acquire and lets readers run
// alongside an inserting writer -- yielding extra buckets, lost increments, or
// heap corruption. A single pass reproduced it only ~18% of the time, so this
// repeats: any surviving structural race shows up as a bucket count or total
// that does not match the work done.
TEST(AggregationEngineTest, ConcurrentIngestIsStructurallySoundAcrossRepeats) {
    constexpr int kRounds = 12;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5'000;
    constexpr int kMinutes = 10;

    for (int round = 0; round < kRounds; ++round) {
        AggregationEngine engine;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&engine] {
                for (int i = 0; i < kPerThread; ++i) {
                    Event e = view_at(kMinuteA +
                                      static_cast<std::int64_t>(i % kMinutes) * 60'000);
                    e.player = "ExoPlayer";  // also exercise the segment maps
                    e.device = "Android";
                    engine.process(e);
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }

        constexpr auto kExpected = static_cast<std::uint64_t>(kThreads) * kPerThread;
        ASSERT_EQ(engine.minute_count(), static_cast<std::size_t>(kMinutes))
            << "round " << round << ": unexpected bucket count";
        ASSERT_EQ(engine.total().total_views, kExpected) << "round " << round;
        ASSERT_EQ(engine.by_player().size(), 1u) << "round " << round;
        ASSERT_EQ(engine.by_player().front().metrics.total_views, kExpected)
            << "round " << round;
        ASSERT_EQ(engine.by_device().size(), 1u) << "round " << round;
    }
}

}  // namespace
