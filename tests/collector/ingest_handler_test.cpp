#include "pulsedb/collector/ingest_handler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "pulsedb/storage/minute_key.hpp"

namespace {

using namespace pulsedb::collector;
using pulsedb::core::Clock;

IngestResult run(std::string_view body, IngestOptions opts = {}) {
    return IngestHandler{opts}.handle(body, Clock::now());
}

TEST(IngestHandlerTest, AcceptsBareArray) {
    const auto r = run(R"([{"event_type":"video_start","session_id":"s1"}])");
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
    ASSERT_TRUE(r.batch.has_value());
    EXPECT_EQ(r.batch->size(), 1u);
}

TEST(IngestHandlerTest, AcceptsEnvelopeObject) {
    const auto r = run(R"({"events":[{"event_type":"pause"},{"event_type":"resume"}]})");
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
    ASSERT_TRUE(r.batch.has_value());
    EXPECT_EQ(r.batch->size(), 2u);
}

TEST(IngestHandlerTest, StampsIngestTimeOntoBatch) {
    const auto t = Clock::now();
    const auto r = IngestHandler{}.handle(R"([{"event_type":"seek"}])", t);
    ASSERT_TRUE(r.batch.has_value());
    EXPECT_EQ(r.batch->ingest_time, t);
}

TEST(IngestHandlerTest, RejectsMalformedJson) {
    const auto r = run("{not valid json");
    EXPECT_EQ(r.status, IngestStatus::kInvalidJson);
    EXPECT_FALSE(r.batch.has_value());
}

TEST(IngestHandlerTest, RejectsEnvelopeWithNonArrayEvents) {
    const auto r = run(R"({"events":{"event_type":"x"}})");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, RejectsWrongTopLevelType) {
    const auto r = run(R"("just a string")");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, RejectsEmptyBatch) {
    const auto r = run("[]");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, RejectsNonObjectEvent) {
    const auto r = run(R"([{"event_type":"a"}, 42])");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, RejectsEventMissingEventType) {
    const auto r = run(R"([{"session_id":"s1"}])");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, RejectsEmptyEventType) {
    const auto r = run(R"([{"event_type":""}])");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, AllowsMissingEventTypeWhenNotRequired) {
    IngestOptions opts;
    opts.require_event_type = false;
    const auto r = run(R"([{"session_id":"s1"}])", opts);
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
}

TEST(IngestHandlerTest, RejectsBatchOverConfiguredLimit) {
    IngestOptions opts;
    opts.max_events_per_batch = 2;
    const auto r =
        run(R"([{"event_type":"a"},{"event_type":"b"},{"event_type":"c"}])", opts);
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

// --- Resource limits (C1): bound what one untrusted request can cost --------

TEST(IngestHandlerTest, RejectsBodyOverByteCap) {
    IngestOptions opts;
    opts.max_body_bytes = 64;
    const std::string body = "[{\"event_type\":\"video_start\",\"session_id\":\"" +
                             std::string(200, 'x') + "\"}]";
    const auto r = run(body, opts);
    EXPECT_EQ(r.status, IngestStatus::kPayloadTooLarge);
    EXPECT_FALSE(r.batch.has_value());
}

TEST(IngestHandlerTest, AcceptsBodyExactlyAtByteCap) {
    const std::string body = R"([{"event_type":"video_start"}])";
    IngestOptions opts;
    opts.max_body_bytes = body.size();  // boundary is inclusive
    EXPECT_EQ(run(body, opts).status, IngestStatus::kAccepted);
}

TEST(IngestHandlerTest, RejectsExcessivelyNestedBody) {
    IngestOptions opts;
    opts.max_json_depth = 8;
    const std::string body = std::string(64, '[') + std::string(64, ']');
    const auto r = run(body, opts);
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
    EXPECT_FALSE(r.batch.has_value());
}

TEST(IngestHandlerTest, DepthScanIgnoresBracketsInsideStrings) {
    // A session id full of brackets is data, not structure, and must not be
    // mistaken for nesting -- including when the quote itself is escaped.
    const auto r = run(R"([{"event_type":"seek","session_id":"[[[{{{ \" [[["}])");
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
}

TEST(IngestHandlerTest, AllowsNestingWithinTheDepthBudget) {
    // Batch (1) -> event object (2) -> a nested value (3) stays under the cap.
    const auto r = run(R"([{"event_type":"seek","meta":{"a":1}}])");
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
}

// --- Timestamp validation (C2) ----------------------------------------------

std::int64_t epoch_ms(pulsedb::core::TimePoint t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch())
        .count();
}

TEST(IngestHandlerTest, StampsReceiptTimeOntoEventsMissingATimestamp) {
    // Without this, every timestamp-less event lands in the 1970 bucket.
    const auto t = Clock::now();
    const auto r = IngestHandler{}.handle(R"([{"event_type":"seek"}])", t);
    ASSERT_EQ(r.status, IngestStatus::kAccepted);
    ASSERT_TRUE(r.batch.has_value());
    EXPECT_EQ(r.batch->events[0].at("timestamp").get<std::int64_t>(), epoch_ms(t));
}

TEST(IngestHandlerTest, PreservesAnInRangeTimestamp) {
    const auto t = Clock::now();
    const std::int64_t ts = epoch_ms(t) - 30'000;  // 30s ago
    const auto r = IngestHandler{}.handle(
        R"([{"event_type":"seek","timestamp":)" + std::to_string(ts) + "}]", t);
    ASSERT_EQ(r.status, IngestStatus::kAccepted);
    EXPECT_EQ(r.batch->events[0].at("timestamp").get<std::int64_t>(), ts);
}

TEST(IngestHandlerTest, RejectsTimestampTooFarInThePast) {
    IngestOptions opts;
    opts.max_lateness = std::chrono::minutes(10);
    const auto t = Clock::now();
    const std::int64_t ts = epoch_ms(t) - std::chrono::milliseconds(
                                              std::chrono::hours(2)).count();
    const auto r = IngestHandler{opts}.handle(
        R"([{"event_type":"seek","timestamp":)" + std::to_string(ts) + "}]", t);
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, RejectsTimestampTooFarInTheFuture) {
    const auto t = Clock::now();
    const std::int64_t ts = epoch_ms(t) + std::chrono::milliseconds(
                                              std::chrono::hours(24)).count();
    const auto r = IngestHandler{}.handle(
        R"([{"event_type":"seek","timestamp":)" + std::to_string(ts) + "}]", t);
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
}

// The regression that motivated all of this: fed int64 extremes,
// MinuteKey::from_timestamp_ms wraps to a negative year instead of failing.
TEST(IngestHandlerTest, RejectsInt64ExtremeTimestamps) {
    const auto t = Clock::now();
    for (const char* ts : {"9223372036854775807", "-9223372036854775808", "0", "-1"}) {
        const auto r = IngestHandler{}.handle(
            std::string(R"([{"event_type":"seek","timestamp":)") + ts + "}]", t);
        EXPECT_EQ(r.status, IngestStatus::kInvalidSchema) << "timestamp " << ts;
    }
}

TEST(IngestHandlerTest, RejectsNonIntegerTimestamp) {
    for (const char* ts : {R"("nope")", "1.5", "null", "[]"}) {
        const auto r = run(std::string(R"([{"event_type":"seek","timestamp":)") + ts + "}]");
        EXPECT_EQ(r.status, IngestStatus::kInvalidSchema) << "timestamp " << ts;
    }
}

TEST(IngestHandlerTest, AcceptedTimestampsAreAlwaysDecomposable) {
    // The window must be a subset of MinuteKey's representable domain, so no
    // accepted event can reach the silent chrono wrap.
    const auto t = Clock::now();
    const auto r = IngestHandler{}.handle(R"([{"event_type":"seek"}])", t);
    ASSERT_EQ(r.status, IngestStatus::kAccepted);
    const auto ts = r.batch->events[0].at("timestamp").get<std::int64_t>();
    EXPECT_TRUE(pulsedb::storage::is_valid_timestamp_ms(ts));
    EXPECT_GT(pulsedb::storage::MinuteKey::from_timestamp_ms(ts).year, 2000);
}

// --- Label bounds (C3, ingest half) -----------------------------------------

TEST(IngestHandlerTest, RejectsOverlongSegmentLabels) {
    IngestOptions opts;
    opts.max_label_length = 16;
    for (const char* key : {"player", "device"}) {
        const auto body = std::string(R"([{"event_type":"seek",")") + key + R"(":")" +
                          std::string(64, 'x') + R"("}])";
        EXPECT_EQ(run(body, opts).status, IngestStatus::kInvalidSchema) << key;
    }
}

TEST(IngestHandlerTest, AcceptsLabelsWithinTheLengthCap) {
    const auto r = run(R"([{"event_type":"seek","player":"ExoPlayer","device":"Android"}])");
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
}

// --- Numeric payload bounds (CB-1) ------------------------------------------
//
// Every one of these fields is summed into an unsigned 64-bit counter that only
// grows and is never recomputed from source, so an out-of-range sample is
// permanent corruption of an average rather than one bad data point. A *negative*
// one converted to ~1.8e19 and destroyed the metric outright -- and because the
// poisoned value is a legitimate unsigned integer, it round-tripped through the
// snapshot and survived a restart. One unauthenticated request was enough.

TEST(IngestHandlerTest, RejectsNegativeNumericPayloads) {
    const std::pair<const char*, const char*> cases[] = {
        {"startup_complete", "startup_time_ms"},
        {"buffer_end", "buffer_duration_ms"},
        {"bitrate_change", "bitrate_kbps"},
        {"playback_end", "watch_time_ms"},
    };
    for (const auto& [type, field] : cases) {
        const auto body = std::string(R"([{"event_type":")") + type + R"(",")" + field +
                          R"(":-1}])";
        const auto r = run(body);
        EXPECT_EQ(r.status, IngestStatus::kInvalidSchema) << field;
        EXPECT_NE(r.message.find(field), std::string::npos)
            << "the rejection must name the offending field: " << r.message;
    }
}

TEST(IngestHandlerTest, RejectsNumericPayloadsAboveTheirCeiling) {
    IngestOptions opts;
    opts.max_startup_time_ms = 1000;
    EXPECT_EQ(run(R"([{"event_type":"startup_complete","startup_time_ms":1001}])", opts).status,
              IngestStatus::kInvalidSchema);
    EXPECT_EQ(run(R"([{"event_type":"startup_complete","startup_time_ms":1000}])", opts).status,
              IngestStatus::kAccepted)
        << "the bound is inclusive";
}

TEST(IngestHandlerTest, RejectsInt64ExtremeNumericPayloads) {
    // A value that would overflow the int the Event field is parsed into.
    EXPECT_EQ(run(R"([{"event_type":"bitrate_change","bitrate_kbps":9223372036854775807}])")
                  .status,
              IngestStatus::kInvalidSchema);
    EXPECT_EQ(run(R"([{"event_type":"playback_end","watch_time_ms":-9223372036854775808}])")
                  .status,
              IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, RejectsNonIntegerNumericPayloads) {
    EXPECT_EQ(run(R"([{"event_type":"startup_complete","startup_time_ms":"fast"}])").status,
              IngestStatus::kInvalidSchema);
    EXPECT_EQ(run(R"([{"event_type":"startup_complete","startup_time_ms":12.5}])").status,
              IngestStatus::kInvalidSchema);
}

TEST(IngestHandlerTest, AcceptsPlausibleNumericPayloads) {
    const auto r = run(R"([{"event_type":"startup_complete","startup_time_ms":1800},
                           {"event_type":"buffer_end","buffer_duration_ms":0},
                           {"event_type":"bitrate_change","bitrate_kbps":3000},
                           {"event_type":"playback_end","watch_time_ms":600000}])");
    EXPECT_EQ(r.status, IngestStatus::kAccepted) << r.message;
}

TEST(IngestHandlerTest, IgnoresNumericFieldsIrrelevantToTheEventType) {
    // The bounds are enforced per field, not per event type, so a stray field
    // is still validated -- but an absent one is simply not checked.
    EXPECT_EQ(run(R"([{"event_type":"seek","seek_to_ms":123456}])").status,
              IngestStatus::kAccepted);
}

}  // namespace
