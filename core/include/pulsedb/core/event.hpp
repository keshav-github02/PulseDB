#pragma once

#include <cstdint>
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
    if (const auto it = value.find("startup_time_ms");
        it != value.end() && it->is_number_integer()) {
        event.startup_time_ms = it->get<int>();
    }
    if (const auto it = value.find("buffer_duration_ms");
        it != value.end() && it->is_number_integer()) {
        event.buffer_duration_ms = it->get<int>();
    }
    if (const auto it = value.find("bitrate_kbps");
        it != value.end() && it->is_number_integer()) {
        event.bitrate_kbps = it->get<int>();
    }
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
