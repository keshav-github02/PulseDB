#include "pulsedb/processor/metrics_processor.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "pulsedb/core/event.hpp"

namespace {

using pulsedb::core::Event;
using pulsedb::core::EventType;
using pulsedb::processor::MetricsProcessor;

Event make(EventType type) {
    Event e;
    e.type = type;
    return e;
}

TEST(MetricsProcessorTest, CountsViewsAndTotals) {
    MetricsProcessor p;
    p.process(make(EventType::kVideoStart));
    p.process(make(EventType::kVideoStart));
    p.process(make(EventType::kPause));

    const auto s = p.snapshot();
    EXPECT_EQ(s.total_events, 3u);
    EXPECT_EQ(s.total_views, 2u);
}

TEST(MetricsProcessorTest, AveragesStartupTime) {
    MetricsProcessor p;
    Event a = make(EventType::kStartupComplete);
    a.startup_time_ms = 1000;
    Event b = make(EventType::kStartupComplete);
    b.startup_time_ms = 3000;
    p.process(a);
    p.process(b);

    const auto s = p.snapshot();
    EXPECT_EQ(s.startup_samples, 2u);
    EXPECT_EQ(s.startup_time_ms_sum, 4000u);
    EXPECT_DOUBLE_EQ(s.avg_startup_ms(), 2000.0);
}

TEST(MetricsProcessorTest, TracksBufferingErrorsWatchAndBitrate) {
    MetricsProcessor p;

    p.process(make(EventType::kBufferStart));
    p.process(make(EventType::kBufferStart));
    Event be = make(EventType::kBufferEnd);
    be.buffer_duration_ms = 750;
    p.process(be);

    p.process(make(EventType::kDrmError));

    Event end = make(EventType::kPlaybackEnd);
    end.watch_time_ms = 600000;
    p.process(end);

    Event br1 = make(EventType::kBitrateChange);
    br1.bitrate_kbps = 3000;
    Event br2 = make(EventType::kBitrateChange);
    br2.bitrate_kbps = 5000;
    p.process(br1);
    p.process(br2);

    const auto s = p.snapshot();
    EXPECT_EQ(s.buffer_count, 2u) << "two stalls began";
    EXPECT_EQ(s.buffer_samples, 1u) << "only one finished, so only one duration sample";
    EXPECT_EQ(s.buffer_duration_ms_sum, 750u);
    // The mean divides by the samples, not the starts: dividing by buffer_count
    // would report 375 ms for a single measured 750 ms stall.
    EXPECT_DOUBLE_EQ(s.avg_buffer_ms(), 750.0);
    EXPECT_EQ(s.error_count, 1u);
    EXPECT_EQ(s.watch_time_ms_sum, 600000u);
    EXPECT_EQ(s.bitrate_samples, 2u);
    EXPECT_DOUBLE_EQ(s.avg_bitrate_kbps(), 4000.0);
}

TEST(MetricsProcessorTest, IgnoresMissingPayloads) {
    MetricsProcessor p;
    p.process(make(EventType::kStartupComplete));  // no startup_time_ms
    p.process(make(EventType::kBitrateChange));    // no bitrate_kbps

    const auto s = p.snapshot();
    EXPECT_EQ(s.total_events, 2u);
    EXPECT_EQ(s.startup_samples, 0u);
    EXPECT_EQ(s.bitrate_samples, 0u);
}

TEST(MetricsProcessorTest, IsThreadSafeUnderConcurrentUpdates) {
    MetricsProcessor p;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50'000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&p] {
            for (int i = 0; i < kPerThread; ++i) {
                p.process(make(EventType::kVideoStart));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    const auto s = p.snapshot();
    EXPECT_EQ(s.total_views, static_cast<std::uint64_t>(kThreads) * kPerThread);
    EXPECT_EQ(s.total_events, static_cast<std::uint64_t>(kThreads) * kPerThread);
}

}  // namespace
