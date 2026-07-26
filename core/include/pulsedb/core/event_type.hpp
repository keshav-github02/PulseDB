#pragma once

#include <optional>
#include <string_view>

namespace pulsedb::core {

/// The playback event types PulseDB understands, shared across the SDK,
/// the processor and (later) the aggregation engine.
enum class EventType {
    kVideoStart,       ///< Playback requested (a "view").
    kStartupComplete,  ///< First frame rendered (startup-time sample).
    kBufferStart,      ///< Rebuffering began (a stall).
    kBufferEnd,        ///< Rebuffering ended (stall-duration sample).
    kPause,            ///< Viewer paused.
    kResume,           ///< Viewer resumed.
    kSeek,             ///< Viewer sought.
    kBitrateChange,    ///< Adaptive bitrate switched (bitrate sample).
    kDrmError,         ///< DRM/license failure (an error).
    kPlaybackEnd,      ///< Playback finished (watch-time sample).
};

/// The canonical wire name for an event type.
constexpr std::string_view to_string(EventType type) {
    switch (type) {
        case EventType::kVideoStart:      return "video_start";
        case EventType::kStartupComplete: return "startup_complete";
        case EventType::kBufferStart:     return "buffer_start";
        case EventType::kBufferEnd:       return "buffer_end";
        case EventType::kPause:           return "pause";
        case EventType::kResume:          return "resume";
        case EventType::kSeek:            return "seek";
        case EventType::kBitrateChange:   return "bitrate_change";
        case EventType::kDrmError:        return "drm_error";
        case EventType::kPlaybackEnd:     return "playback_end";
    }
    return "unknown";
}

/// Parse a wire name back into an EventType, or std::nullopt if unknown.
constexpr std::optional<EventType> event_type_from_string(std::string_view name) {
    if (name == "video_start")      return EventType::kVideoStart;
    if (name == "startup_complete") return EventType::kStartupComplete;
    if (name == "buffer_start")     return EventType::kBufferStart;
    if (name == "buffer_end")       return EventType::kBufferEnd;
    if (name == "pause")            return EventType::kPause;
    if (name == "resume")           return EventType::kResume;
    if (name == "seek")             return EventType::kSeek;
    if (name == "bitrate_change")   return EventType::kBitrateChange;
    if (name == "drm_error")        return EventType::kDrmError;
    if (name == "playback_end")     return EventType::kPlaybackEnd;
    return std::nullopt;
}

}  // namespace pulsedb::core
