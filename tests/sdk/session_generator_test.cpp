#include "pulsedb/sdk/session_generator.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using pulsedb::sdk::SessionGenerator;

const std::set<std::string>& valid_event_types() {
    static const std::set<std::string> kTypes{
        "video_start", "startup_complete", "buffer_start", "buffer_end",
        "pause", "resume", "seek", "bitrate_change", "drm_error", "playback_end"};
    return kTypes;
}

TEST(SessionGeneratorTest, SameSeedProducesIdenticalSessions) {
    SessionGenerator a{12345};
    SessionGenerator b{12345};
    EXPECT_EQ(a.generate_session(0), b.generate_session(0));
}

TEST(SessionGeneratorTest, DifferentSeedsDiverge) {
    SessionGenerator a{1};
    SessionGenerator b{2};
    // Overwhelmingly likely to differ; guards against a broken/ignored seed.
    EXPECT_NE(a.generate_session(0), b.generate_session(0));
}

TEST(SessionGeneratorTest, StartsWithVideoStartAndEndsWithPlaybackEnd) {
    SessionGenerator gen{7};
    const auto events = gen.generate_session(1'000);
    ASSERT_GE(events.size(), 3u);
    EXPECT_EQ(events.front().at("event_type"), "video_start");
    EXPECT_EQ(events.back().at("event_type"), "playback_end");
}

TEST(SessionGeneratorTest, AllEventsShareOneSessionAndKnownFields) {
    SessionGenerator gen{99};
    const auto events = gen.generate_session(5'000);
    const std::string session_id = events.front().at("session_id");
    const std::string player = events.front().at("player");
    const std::string device = events.front().at("device");

    for (const auto& e : events) {
        EXPECT_EQ(e.at("session_id"), session_id);
        EXPECT_EQ(e.at("player"), player);
        EXPECT_EQ(e.at("device"), device);
        EXPECT_TRUE(valid_event_types().count(e.at("event_type").get<std::string>()))
            << "unexpected event_type: " << e.at("event_type");
    }
}

TEST(SessionGeneratorTest, TimestampsAreMonotonicAndStartAtOffset) {
    SessionGenerator gen{42};
    const std::int64_t start = 1'700'000'000'000;
    const auto events = gen.generate_session(start);

    EXPECT_EQ(events.front().at("timestamp").get<std::int64_t>(), start);
    std::int64_t prev = start;
    for (const auto& e : events) {
        const auto ts = e.at("timestamp").get<std::int64_t>();
        EXPECT_GE(ts, prev);
        prev = ts;
    }
}

TEST(SessionGeneratorTest, StartupCompleteCarriesStartupTime) {
    SessionGenerator gen{3};
    const auto events = gen.generate_session(0);
    ASSERT_GE(events.size(), 2u);
    const auto& startup = events[1];
    ASSERT_EQ(startup.at("event_type"), "startup_complete");
    EXPECT_TRUE(startup.contains("startup_time_ms"));
    EXPECT_GT(startup.at("startup_time_ms").get<int>(), 0);
}

TEST(SessionGeneratorTest, PlaybackEndCarriesWatchTime) {
    SessionGenerator gen{55};
    const auto events = gen.generate_session(0);
    const auto& last = events.back();
    ASSERT_EQ(last.at("event_type"), "playback_end");
    EXPECT_TRUE(last.contains("watch_time_ms"));
    EXPECT_GT(last.at("watch_time_ms").get<std::int64_t>(), 0);
}

}  // namespace
