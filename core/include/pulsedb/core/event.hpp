#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "pulsedb/core/event_type.hpp"

namespace pulsedb::core {

/// A single playback event, parsed from raw JSON into typed fields.
///
/// Only fields relevant to metrics are extracted; type-specific payloads
/// are optional and present only for the event types that carry them.
struct Event {
    EventType type{};
    std::string session_id;
    std::int64_t timestamp_ms = 0;
    std::string player;
    std::string device;

    std::optional<int> startup_time_ms;         ///< startup_complete
    std::optional<int> buffer_duration_ms;      ///< buffer_end
    std::optional<int> bitrate_kbps;            ///< bitrate_change
    std::optional<std::int64_t> watch_time_ms;  ///< playback_end
    std::optional<std::string> error_code;      ///< drm_error
};

/// Parse one raw event object into a typed Event.
///
/// Returns std::nullopt when the value is not an object or carries no
/// recognised `event_type`; every other field is best-effort and simply
/// left unset if absent or of the wrong type. Keeping this permissive lets
/// the worker pool count a malformed event as a parse failure without
/// throwing on the hot path.
inline std::optional<Event> parse_event(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    const auto type_it = value.find("event_type");
    if (type_it == value.end() || !type_it->is_string()) {
        return std::nullopt;
    }
    const auto type = event_type_from_string(type_it->get_ref<const std::string&>());
    if (!type) {
        return std::nullopt;
    }

    Event event;
    event.type = *type;

    const auto get_string = [&](const char* key, std::string& out) {
        if (const auto it = value.find(key); it != value.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    get_string("session_id", event.session_id);
    get_string("player", event.player);
    get_string("device", event.device);

    if (const auto it = value.find("timestamp");
        it != value.end() && it->is_number_integer()) {
        event.timestamp_ms = it->get<std::int64_t>();
    }

    /// Read an integer payload that is stored as `int`, leaving it unset if the
    /// value will not fit.
    ///
    /// These were read with get<int>(), which narrows a wider JSON integer
    /// modularly rather than failing -- so the payload did not merely arrive
    /// wrong, it arrived *plausible*: 4294967796 became 500 and was folded into
    /// the mean startup time as though it had been measured. 8589934592 became 0,
    /// and 2147483648 became negative.
    ///
    /// The ingest layer range-checks these fields and answers 422, so an HTTP
    /// client cannot reach this. parse_event() is public core API though, and
    /// this mirrors what MetricAccumulator already does for negative samples:
    /// enforce the invariant where the value is interpreted, so it holds for
    /// callers that never went through HTTP. Out of range is treated exactly like
    /// absent -- it contributes to neither a sum nor a sample count.
    const auto get_int = [&](const char* key, std::optional<int>& out) {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_number_integer()) {
            return;
        }
        const auto raw = it->get<std::int64_t>();
        if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
            return;
        }
        out = static_cast<int>(raw);
    };
    get_int("startup_time_ms", event.startup_time_ms);
    get_int("buffer_duration_ms", event.buffer_duration_ms);
    get_int("bitrate_kbps", event.bitrate_kbps);
    if (const auto it = value.find("watch_time_ms");
        it != value.end() && it->is_number_integer()) {
        event.watch_time_ms = it->get<std::int64_t>();
    }
    if (const auto it = value.find("error_code");
        it != value.end() && it->is_string()) {
        event.error_code = it->get<std::string>();
    }

    return event;
}

}  // namespace pulsedb::core
