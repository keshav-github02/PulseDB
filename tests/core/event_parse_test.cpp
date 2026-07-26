#include "pulsedb/core/event.hpp"
#include "pulsedb/core/event_type.hpp"

#include <gtest/gtest.h>

namespace {

using namespace pulsedb::core;

TEST(EventTypeTest, RoundTripsThroughStrings) {
    for (const auto type : {EventType::kVideoStart, EventType::kStartupComplete,
                            EventType::kBufferStart, EventType::kBufferEnd,
                            EventType::kPause, EventType::kResume, EventType::kSeek,
                            EventType::kBitrateChange, EventType::kDrmError,
                            EventType::kPlaybackEnd}) {
        const auto name = to_string(type);
        const auto parsed = event_type_from_string(name);
        ASSERT_TRUE(parsed.has_value()) << name;
        EXPECT_EQ(*parsed, type);
    }
}

TEST(EventTypeTest, UnknownStringIsNullopt) {
    EXPECT_FALSE(event_type_from_string("not_a_real_event").has_value());
    EXPECT_FALSE(event_type_from_string("").has_value());
}

TEST(ParseEventTest, ParsesCommonFields) {
    const auto json = nlohmann::json::parse(R"({
        "event_type": "video_start",
        "session_id": "sess-1",
        "timestamp": 1700000000000,
        "player": "ExoPlayer",
        "device": "Android"
    })");
    const auto event = parse_event(json);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, EventType::kVideoStart);
    EXPECT_EQ(event->session_id, "sess-1");
    EXPECT_EQ(event->timestamp_ms, 1700000000000);
    EXPECT_EQ(event->player, "ExoPlayer");
    EXPECT_EQ(event->device, "Android");
}

TEST(ParseEventTest, ExtractsTypeSpecificPayloads) {
    const auto startup = parse_event(nlohmann::json::parse(
        R"({"event_type":"startup_complete","startup_time_ms":1234})"));
    ASSERT_TRUE(startup.has_value());
    ASSERT_TRUE(startup->startup_time_ms.has_value());
    EXPECT_EQ(*startup->startup_time_ms, 1234);

    const auto buffer = parse_event(nlohmann::json::parse(
        R"({"event_type":"buffer_end","buffer_duration_ms":500})"));
    ASSERT_TRUE(buffer->buffer_duration_ms.has_value());
    EXPECT_EQ(*buffer->buffer_duration_ms, 500);

    const auto bitrate = parse_event(nlohmann::json::parse(
        R"({"event_type":"bitrate_change","bitrate_kbps":3000})"));
    ASSERT_TRUE(bitrate->bitrate_kbps.has_value());
    EXPECT_EQ(*bitrate->bitrate_kbps, 3000);

    const auto ended = parse_event(nlohmann::json::parse(
        R"({"event_type":"playback_end","watch_time_ms":600000})"));
    ASSERT_TRUE(ended->watch_time_ms.has_value());
    EXPECT_EQ(*ended->watch_time_ms, 600000);

    const auto drm = parse_event(nlohmann::json::parse(
        R"({"event_type":"drm_error","error_code":"DRM_KEY_ERROR"})"));
    ASSERT_TRUE(drm->error_code.has_value());
    EXPECT_EQ(*drm->error_code, "DRM_KEY_ERROR");
}

TEST(ParseEventTest, LeavesAbsentOptionalsUnset) {
    const auto event = parse_event(nlohmann::json::parse(R"({"event_type":"pause"})"));
    ASSERT_TRUE(event.has_value());
    EXPECT_FALSE(event->startup_time_ms.has_value());
    EXPECT_FALSE(event->watch_time_ms.has_value());
    EXPECT_EQ(event->timestamp_ms, 0);
}

TEST(ParseEventTest, RejectsMissingOrUnknownType) {
    EXPECT_FALSE(parse_event(nlohmann::json::parse(R"({"session_id":"s"})")).has_value());
    EXPECT_FALSE(parse_event(nlohmann::json::parse(R"({"event_type":"bogus"})")).has_value());
    EXPECT_FALSE(parse_event(nlohmann::json::parse(R"({"event_type":123})")).has_value());
}

TEST(ParseEventTest, RejectsNonObject) {
    EXPECT_FALSE(parse_event(nlohmann::json::parse("[1,2,3]")).has_value());
    EXPECT_FALSE(parse_event(nlohmann::json::parse("42")).has_value());
}

}  // namespace
