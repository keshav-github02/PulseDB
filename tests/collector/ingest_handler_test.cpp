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
    // The first event carries a *valid* type so the rejection can only be caused
    // by the bare 42 -- otherwise the unrecognised-type check fires at index 0
    // and this passes without ever exercising the non-object path.
    const auto r = run(R"([{"event_type":"seek"}, 42])");
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
    // Valid event types, so this tests the count cap rather than accidentally
    // depending on the per-event checks running first.
    const auto r = run(
        R"([{"event_type":"pause"},{"event_type":"resume"},{"event_type":"seek"}])", opts);
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

// --- Breadth bound (parse-cost DoS) -----------------------------------------
// max_events_per_batch is only knowable after json::parse has built the DOM, so
// it could not bound parsing cost -- the work was already done by the time it
// rejected the batch. max_json_depth did not help either: a flat array is depth
// 1 however long it grows. Measured before the fix, an 8 MiB body of
// "[0,0,0,...]" (4.2M elements) held a request thread for 6.66 seconds and was
// then answered 422, against 0.64s for a legitimate 9,000-event batch.

TEST(IngestHandlerTest, RejectsAFlatBodyWithTooManyValuesBeforeParsing) {
    // Depth 1, so the nesting guard passes it; breadth is the whole attack.
    std::string body = "[0";
    for (int i = 0; i < 50'000; ++i) {
        body += ",0";
    }
    body += "]";

    IngestOptions opts;
    opts.max_structural_tokens = 1'000;
    const auto r = run(body, opts);
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
    EXPECT_NE(r.message.find("max_structural_tokens"), std::string::npos) << r.message;
}

TEST(IngestHandlerTest, BreadthBoundIsCheckedBeforeTheEventCount) {
    // Both limits would reject this body. The breadth bound has to be the one
    // that fires, because it is the only one that runs before the parse.
    // Sized past max_structural_tokens, not merely past max_events_per_batch --
    // 50k elements clears the event cap but sits well inside the token budget,
    // so it would prove nothing about which guard ran first.
    IngestOptions opts;
    std::string body = "[0";
    for (std::size_t i = 0; i < opts.max_structural_tokens + 1'000; ++i) {
        body += ",0";
    }
    body += "]";

    const auto r = run(body);
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
    EXPECT_NE(r.message.find("max_structural_tokens"), std::string::npos)
        << "the pre-parse guard must fire, not the post-parse event count: " << r.message;
}

TEST(IngestHandlerTest, AFullSizeLegitimateBatchStillFitsTheBreadthBound) {
    // The guard is worthless if it rejects real traffic. A maximum-size batch of
    // realistic events must pass with room to spare under the default.
    //
    // No "timestamp" field: an absent one is stamped with the receipt time, so
    // this cannot rot. A literal epoch would drift out of the max_lateness
    // freshness window as the calendar moves and fail months from now for a
    // reason that has nothing to do with what is being tested.
    IngestOptions opts;
    std::string body = "[";
    for (std::size_t i = 0; i < opts.max_events_per_batch; ++i) {
        if (i > 0) {
            body += ',';
        }
        body +=
            R"({"event_type":"startup_complete","session_id":"abcdef0123456789",)"
            R"("player":"ExoPlayer","device":"Android","startup_time_ms":1800})";
    }
    body += "]";
    ASSERT_LE(body.size(), opts.max_body_bytes) << "test batch must fit the byte cap";

    const auto r = run(body);
    EXPECT_EQ(r.status, IngestStatus::kAccepted) << r.message;
    ASSERT_TRUE(r.batch.has_value());
    EXPECT_EQ(r.batch->size(), opts.max_events_per_batch);
}

TEST(IngestHandlerTest, CommasInsideStringsDoNotCountTowardBreadth) {
    // The scan must ignore structural characters inside string literals, or a
    // legitimate label full of commas would be read as thousands of values.
    IngestOptions opts;
    opts.max_structural_tokens = 20;
    const auto r = run(
        R"([{"event_type":"seek","session_id":"a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p"}])", opts);
    EXPECT_EQ(r.status, IngestStatus::kAccepted) << r.message;
}

// --- Event-type whitelist ---------------------------------------------------
// Presence used to be the entire check, so an unrecognised type was answered
// with 202 and then dropped by the worker as a parse failure: the client was
// told its data was accepted while it was in fact discarded.

TEST(IngestHandlerTest, RejectsUnrecognisedEventType) {
    const auto r = run(R"([{"event_type":"not_a_real_event_type","session_id":"s1"}])");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
    EXPECT_FALSE(r.batch.has_value());
    EXPECT_NE(r.message.find("not_a_real_event_type"), std::string::npos)
        << "the rejection must name the offending type so the sender can fix it: "
        << r.message;
}

TEST(IngestHandlerTest, RejectsUnrecognisedEventTypeAnywhereInTheBatch) {
    // A single bad event must not ride in on the back of valid ones.
    const auto r = run(R"([{"event_type":"video_start"},
                           {"event_type":"vidoe_start"},
                           {"event_type":"playback_end"}])");
    EXPECT_EQ(r.status, IngestStatus::kInvalidSchema);
    EXPECT_NE(r.message.find("index 1"), std::string::npos) << r.message;
}

TEST(IngestHandlerTest, RejectsUnrecognisedEventTypeEvenWhenNotRequired) {
    // require_event_type governs whether the field may be *absent*. It does not
    // license a present-but-meaningless value, which would still be dropped
    // downstream.
    IngestOptions opts;
    opts.require_event_type = false;
    EXPECT_EQ(run(R"([{"event_type":"bogus"}])", opts).status,
              IngestStatus::kInvalidSchema);
    EXPECT_EQ(run(R"([{"session_id":"s1"}])", opts).status, IngestStatus::kAccepted);
}

TEST(IngestHandlerTest, AcceptsEveryKnownEventType) {
    // Guards against the whitelist and the enum drifting apart: a type the
    // pipeline understands must never be rejected at the door.
    for (const char* type : {"video_start", "startup_complete", "buffer_start",
                             "buffer_end", "pause", "resume", "seek",
                             "bitrate_change", "drm_error", "playback_end"}) {
        const auto body = std::string(R"([{"event_type":")") + type + R"("}])";
        EXPECT_EQ(run(body).status, IngestStatus::kAccepted) << type;
    }
}

}  // namespace
