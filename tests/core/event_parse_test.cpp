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

// Regression: startup_time_ms, buffer_duration_ms and bitrate_kbps are stored as
// `int` and were read with get<int>(), which narrows a wider JSON integer
// modularly instead of failing. The payload therefore did not arrive wrong so
// much as arrive *believable* -- 4294967796 became 500 and was folded into the
// mean startup time as though it had been measured. Out of range is now treated
// exactly like absent, so it contributes to neither a sum nor a sample count.
TEST(EventParseTest, OutOfRangeIntPayloadsAreDroppedNotWrapped) {
    struct Case {
        const char* body;
        const char* field;
    };
    for (const auto& [body, field] :
         {Case{R"({"event_type":"startup_complete","startup_time_ms":4294967796})",
               "startup_time_ms"},
          Case{R"({"event_type":"bitrate_change","bitrate_kbps":8589934592})", "bitrate_kbps"},
          Case{R"({"event_type":"buffer_end","buffer_duration_ms":2147483648})",
               "buffer_duration_ms"},
          Case{R"({"event_type":"buffer_end","buffer_duration_ms":-2147483649})",
               "buffer_duration_ms (negative)"}}) {
        const auto event = parse_event(nlohmann::json::parse(body));
        ASSERT_TRUE(event.has_value()) << field << ": the event itself is still parsed";
        EXPECT_FALSE(event->startup_time_ms.has_value()) << field;
        EXPECT_FALSE(event->bitrate_kbps.has_value()) << field;
        EXPECT_FALSE(event->buffer_duration_ms.has_value()) << field;
    }
}

TEST(EventParseTest, InRangeIntPayloadsStillParse) {
    // The boundary must stay inclusive -- the guard rejects what cannot be
    // represented, not what merely looks large.
    const auto max_int = parse_event(nlohmann::json::parse(
        R"({"event_type":"bitrate_change","bitrate_kbps":2147483647})"));
    ASSERT_TRUE(max_int.has_value());
    ASSERT_TRUE(max_int->bitrate_kbps.has_value());
    EXPECT_EQ(*max_int->bitrate_kbps, 2147483647);

    const auto ordinary = parse_event(nlohmann::json::parse(
        R"({"event_type":"startup_complete","startup_time_ms":1800})"));
    ASSERT_TRUE(ordinary.has_value());
    ASSERT_TRUE(ordinary->startup_time_ms.has_value());
    EXPECT_EQ(*ordinary->startup_time_ms, 1800);
}

// watch_time_ms is already int64 on the wire and in the struct, so it has no
// narrowing step to get wrong. Pinned so a later change to its type cannot
// quietly reintroduce one.
TEST(EventParseTest, WatchTimeKeepsFullSixtyFourBitRange) {
    const auto event = parse_event(nlohmann::json::parse(
        R"({"event_type":"playback_end","watch_time_ms":4294967796})"));
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->watch_time_ms.has_value());
    EXPECT_EQ(*event->watch_time_ms, 4294967796LL);
}

}  // namespace
