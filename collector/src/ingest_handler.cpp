#include "pulsedb/collector/ingest_handler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

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

/// Deepest bracket nesting in @p body, counting only structural brackets
/// (those outside string literals).
///
/// This is a lexical pre-scan rather than a check on the parsed document
/// because it has to run *before* json::parse: nlohmann's parser is iterative
/// and survives extreme nesting, but it still allocates one node per level, so
/// the DOM is what needs bounding, and by then it is too late.
std::size_t max_nesting_depth(std::string_view body) {
    std::size_t depth = 0;
    std::size_t deepest = 0;
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
                deepest = std::max(deepest, ++depth);
                break;
            case ']':
            case '}':
                if (depth > 0) {
                    --depth;
                }
                break;
            default:
                break;
        }
    }
    return deepest;
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
    if (const std::size_t depth = max_nesting_depth(body);
        depth > options_.max_json_depth) {
        return reject(IngestStatus::kInvalidSchema,
                      "request body nests " + std::to_string(depth) +
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
        if (options_.require_event_type) {
            const auto it = event.find("event_type");
            if (it == event.end() || !it->is_string() ||
                it->get_ref<const std::string&>().empty()) {
                return reject(IngestStatus::kInvalidSchema,
                              at + " is missing a non-empty string \"event_type\"");
            }
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
