#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pulsedb::sdk {

/// Generates realistic playback sessions as sequences of raw event objects.
///
/// Each call to generate_session() produces one viewer's session: it always
/// begins with a `video_start`, follows with `startup_complete` and a
/// randomised body of playback activity (bitrate changes, rebuffering,
/// pause/resume, seeks, occasional DRM errors), and ends with a
/// `playback_end`. Event timestamps increase monotonically.
///
/// Generation is fully driven by a seeded PRNG, so a given seed always
/// yields the same sessions -- which keeps tests deterministic. One
/// generator instance is not thread-safe; use one per producer thread.
class SessionGenerator {
public:
    explicit SessionGenerator(std::uint64_t seed = 0) : rng_(seed) {}

    /// Produce one session's events, with timestamps (epoch milliseconds)
    /// starting at @p start_time_ms.
    std::vector<nlohmann::json> generate_session(std::int64_t start_time_ms);

private:
    /// Per-session identity shared by every event in the session.
    struct SessionContext {
        std::string session_id;
        std::string player;
        std::string device;
    };

    static nlohmann::json make_event(const SessionContext& ctx,
                                     std::string_view type, std::int64_t ts_ms);

    std::string new_session_id();

    std::mt19937_64 rng_;
};

}  // namespace pulsedb::sdk
