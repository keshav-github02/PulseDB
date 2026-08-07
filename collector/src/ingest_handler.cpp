#include "pulsedb/collector/ingest_handler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "pulsedb/core/event_type.hpp"

namespace pulsedb::collector {
namespace {

using nlohmann::json;

IngestResult reject(IngestStatus status, std::string message) {
    return IngestResult{status, std::move(message), std::nullopt};
}

// Saturating int64 arithmetic (std::add_sat is C++26). The freshness window is
// derived from configured durations, and computing it must not be the thing
// that invokes signed overflow -- otherwise the bound-check meant to stop
// hostile timestamps would itself be undefined behaviour.
constexpr std::int64_t kMinI64 = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMaxI64 = std::numeric_limits<std::int64_t>::max();

constexpr std::int64_t add_sat(std::int64_t a, std::int64_t b) {
    if (b > 0 && a > kMaxI64 - b) return kMaxI64;
    if (b < 0 && a < kMinI64 - b) return kMinI64;
    return a + b;
}

constexpr std::int64_t sub_sat(std::int64_t a, std::int64_t b) {
    if (b > 0 && a < kMinI64 + b) return kMinI64;
    if (b < 0 && a > kMaxI64 + b) return kMaxI64;
    return a - b;
}

/// What a lexical pre-scan can tell us about a body before it is parsed.
struct BodyShape {
    std::size_t deepest = 0;           ///< Deepest bracket nesting.
    bool over_token_budget = false;    ///< Scan aborted: too many structural tokens.
};

/// Measure @p body's nesting depth and structural token count without parsing it.
///
/// Both bounds have to be established *before* json::parse, because the DOM is
/// the resource being protected and by the time parse returns it has already
/// been built. nlohmann's parser is iterative, so it survives hostile input --
/// it just allocates while doing so.
///
/// Depth bounds nesting. The token count bounds *breadth*, which depth cannot:
/// a flat array is depth 1 no matter how long it is. max_events_per_batch was
/// supposed to bound that, but it is only knowable after parsing, so the entire
/// cost was paid before the check that rejects it. Measured: an 8 MiB body of
/// `[0,0,0,...]` (4.2M elements, depth 1) occupied a request thread for **6.66
/// seconds** and was then answered 422 -- against 0.64s for a legitimate 9,000
/// event batch. httplib serves one thread per connection from a fixed pool, so a
/// handful of concurrent such requests starves ingestion of every thread it has,
/// at a cost to the sender of nothing but bandwidth.
///
/// The scan aborts the moment the budget is exceeded, so a hostile body is not
/// even fully walked.
BodyShape scan_structure(std::string_view body, std::size_t token_budget) {
    BodyShape shape;
    std::size_t depth = 0;
    std::size_t tokens = 0;
    bool in_string = false;
    bool escaped = false;

    for (const char c : body) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        switch (c) {
            case '"':
                in_string = true;
                break;
            case '[':
            case '{':
                shape.deepest = std::max(shape.deepest, ++depth);
                ++tokens;
                break;
            case ']':
            case '}':
                if (depth > 0) {
                    --depth;
                }
                ++tokens;
                break;
            case ',':
                ++tokens;
                break;
            default:
                break;
        }
        if (tokens > token_budget) {
            shape.over_token_budget = true;
            return shape;
        }
    }
    return shape;
}

}  // namespace

IngestResult IngestHandler::handle(std::string_view body,
                                   core::TimePoint ingest_time) const {
    // Cheapest checks first: reject on size and shape before paying for a parse.
    // The server also caps this at the transport layer, but IngestHandler is
    // used directly by tests and benchmarks, so it enforces its own bound too.
    if (body.size() > options_.max_body_bytes) {
        return reject(IngestStatus::kPayloadTooLarge,
                      "request body exceeds max_body_bytes (" +
                          std::to_string(options_.max_body_bytes) + ")");
    }
    const BodyShape shape = scan_structure(body, options_.max_structural_tokens);
    if (shape.over_token_budget) {
        return reject(IngestStatus::kInvalidSchema,
                      "request body exceeds max_structural_tokens (" +
                          std::to_string(options_.max_structural_tokens) +
                          "); it encodes far more JSON values than "
                          "max_events_per_batch (" +
                          std::to_string(options_.max_events_per_batch) +
                          ") events could contain");
    }
    if (shape.deepest > options_.max_json_depth) {
        return reject(IngestStatus::kInvalidSchema,
                      "request body nests " + std::to_string(shape.deepest) +
                          " levels deep, exceeding max_json_depth (" +
                          std::to_string(options_.max_json_depth) + ")");
    }

    json parsed = json::parse(body, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return reject(IngestStatus::kInvalidJson, "request body is not valid JSON");
    }

    // Accept either a bare array of events, or an envelope {"events": [...]}.
    json events;
    if (parsed.is_array()) {
        events = std::move(parsed);
    } else if (parsed.is_object() && parsed.contains("events")) {
        events = std::move(parsed["events"]);
    } else {
        return reject(IngestStatus::kInvalidSchema,
                      "expected a JSON array of events or an object with an "
                      "\"events\" array");
    }

    if (!events.is_array()) {
        return reject(IngestStatus::kInvalidSchema, "\"events\" must be an array");
    }
    if (events.empty()) {
        return reject(IngestStatus::kInvalidSchema, "events array is empty");
    }
    if (events.size() > options_.max_events_per_batch) {
        return reject(IngestStatus::kInvalidSchema,
                      "batch exceeds max_events_per_batch (" +
                          std::to_string(options_.max_events_per_batch) + ")");
    }

    // Freshness window for event-supplied timestamps, computed once per batch.
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ingest_time.time_since_epoch())
            .count();
    const std::int64_t oldest_ms = sub_sat(now_ms, options_.max_lateness.count());
    const std::int64_t newest_ms = add_sat(now_ms, options_.max_future_skew.count());

    for (std::size_t i = 0; i < events.size(); ++i) {
        json& event = events[i];
        const std::string at = "event at index " + std::to_string(i);

        if (!event.is_object()) {
            return reject(IngestStatus::kInvalidSchema, at + " is not an object");
        }
        // The event type must not merely be present -- it must name a type the
        // pipeline can actually fold into a metric.
        //
        // Presence alone was the whole check, and that made the 202 a lie: an
        // unrecognised type passed ingest, then parse_event() returned nullopt in
        // the worker and the event was counted as a parse failure and dropped. The
        // client was told "accepted" for data that was silently thrown away, which
        // is the worst of both worlds -- no data and no error. A typo'd or
        // version-skewed event type is exactly the case that needs to be loud,
        // because the sender is the only party that can fix it.
        //
        // Validating here rather than in the worker keeps the rejection
        // synchronous, so it can still be answered with a 422.
        if (const auto it = event.find("event_type"); it != event.end()) {
            if (!it->is_string() || it->get_ref<const std::string&>().empty()) {
                return reject(IngestStatus::kInvalidSchema,
                              at + " is missing a non-empty string \"event_type\"");
            }
            const std::string& name = it->get_ref<const std::string&>();
            if (!core::event_type_from_string(name)) {
                return reject(IngestStatus::kInvalidSchema,
                              at + " has unrecognised \"event_type\" \"" + name +
                                  "\"; the event would be accepted and then dropped");
            }
        } else if (options_.require_event_type) {
            return reject(IngestStatus::kInvalidSchema,
                          at + " is missing a non-empty string \"event_type\"");
        }

        // Timestamps address a permanent minute bucket, so they are validated
        // here rather than trusted downstream. An absent timestamp is stamped
        // with the receipt time -- what a collector should do, and better than
        // the alternative of every such event landing in the 1970 bucket.
        if (const auto it = event.find("timestamp"); it == event.end()) {
            event["timestamp"] = now_ms;
        } else if (!it->is_number_integer()) {
            return reject(IngestStatus::kInvalidSchema,
                          at + " has a non-integer \"timestamp\"");
        } else if (const std::int64_t ts = it->get<std::int64_t>();
                   ts < oldest_ms || ts > newest_ms) {
            return reject(IngestStatus::kInvalidSchema,
                          at + " has timestamp " + std::to_string(ts) +
                              " outside the accepted window [" +
                              std::to_string(oldest_ms) + ", " +
                              std::to_string(newest_ms) + "]");
        }

        // Segment labels become permanent keys in the aggregation engine.
        for (const char* key : {"player", "device"}) {
            if (const auto it = event.find(key);
                it != event.end() && it->is_string() &&
                it->get_ref<const std::string&>().size() > options_.max_label_length) {
                return reject(IngestStatus::kInvalidSchema,
                              at + " has a \"" + key + "\" label longer than " +
                                  std::to_string(options_.max_label_length) +
                                  " characters");
            }
        }

        // Numeric payloads are summed into unsigned counters that are never
        // recomputed, so an out-of-range sample is permanent corruption rather
        // than one bad data point. Validate before it can reach an accumulator.
        const std::pair<const char*, std::int64_t> numeric_limits[] = {
            {"startup_time_ms", options_.max_startup_time_ms},
            {"buffer_duration_ms", options_.max_buffer_duration_ms},
            {"bitrate_kbps", options_.max_bitrate_kbps},
            {"watch_time_ms", options_.max_watch_time_ms},
        };
        for (const auto& [key, max_value] : numeric_limits) {
            const auto it = event.find(key);
            if (it == event.end()) {
                continue;
            }
            if (!it->is_number_integer()) {
                return reject(IngestStatus::kInvalidSchema,
                              at + " has a non-integer \"" + key + "\"");
            }
            if (const std::int64_t value = it->get<std::int64_t>();
                value < 0 || value > max_value) {
                return reject(IngestStatus::kInvalidSchema,
                              at + " has \"" + key + "\" = " + std::to_string(value) +
                                  " outside the accepted range [0, " +
                                  std::to_string(max_value) + "]");
            }
        }
    }

    const std::size_t count = events.size();
    core::EventBatch batch{std::move(events), ingest_time};
    return IngestResult{IngestStatus::kAccepted,
                        "accepted " + std::to_string(count) + " event(s)",
                        std::move(batch)};
}

}  // namespace pulsedb::collector
