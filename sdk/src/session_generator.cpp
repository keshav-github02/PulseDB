#include "pulsedb/sdk/session_generator.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace pulsedb::sdk {
namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 5> kPlayers{
    "ExoPlayer", "AVPlayer", "Shaka", "HLS.js", "VideoJS"};
constexpr std::array<std::string_view, 6> kDevices{
    "Android", "iOS", "Web", "Roku", "SmartTV", "FireTV"};
constexpr std::array<int, 6> kBitrateLadderKbps{300, 800, 1500, 3000, 5000, 8000};
constexpr std::array<std::string_view, 3> kDrmErrors{
    "DRM_LICENSE_EXPIRED", "DRM_DEVICE_REVOKED", "DRM_KEY_ERROR"};

}  // namespace

json SessionGenerator::make_event(const SessionContext& ctx,
                                  std::string_view type, std::int64_t ts_ms) {
    json event;
    event["event_type"] = std::string(type);
    event["session_id"] = ctx.session_id;
    event["player"] = ctx.player;
    event["device"] = ctx.device;
    event["timestamp"] = ts_ms;
    return event;
}

std::string SessionGenerator::new_session_id() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::uniform_int_distribution<int> nibble(0, 15);
    std::string id(16, '0');
    for (char& c : id) {
        c = kHex[static_cast<std::size_t>(nibble(rng_))];
    }
    return id;
}

std::vector<json> SessionGenerator::generate_session(std::int64_t start_time_ms) {
    const auto uniform = [this](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng_);
    };
    const auto chance = [this](double p) {
        return std::bernoulli_distribution(p)(rng_);
    };
    const auto pick = [&uniform](const auto& arr) {
        return arr[static_cast<std::size_t>(uniform(0, static_cast<int>(arr.size()) - 1))];
    };

    SessionContext ctx;
    ctx.session_id = new_session_id();
    ctx.player = std::string(pick(kPlayers));
    ctx.device = std::string(pick(kDevices));

    std::vector<json> events;
    std::int64_t t = start_time_ms;

    events.push_back(make_event(ctx, "video_start", t));

    const int startup_ms = uniform(400, 4000);
    t += startup_ms;
    {
        json e = make_event(ctx, "startup_complete", t);
        e["startup_time_ms"] = startup_ms;
        events.push_back(std::move(e));
    }

    const int segments = uniform(3, 10);
    for (int i = 0; i < segments; ++i) {
        t += uniform(5'000, 30'000);  // playback progresses 5-30s per segment

        if (chance(0.50)) {
            json e = make_event(ctx, "bitrate_change", t);
            e["bitrate_kbps"] = pick(kBitrateLadderKbps);
            events.push_back(std::move(e));
        }
        if (chance(0.20)) {  // rebuffering
            events.push_back(make_event(ctx, "buffer_start", t));
            const int stall_ms = uniform(200, 3000);
            t += stall_ms;
            json e = make_event(ctx, "buffer_end", t);
            e["buffer_duration_ms"] = stall_ms;
            events.push_back(std::move(e));
        }
        if (chance(0.15)) {  // pause then resume
            events.push_back(make_event(ctx, "pause", t));
            t += uniform(1'000, 60'000);
            events.push_back(make_event(ctx, "resume", t));
        }
        if (chance(0.10)) {
            json e = make_event(ctx, "seek", t);
            e["seek_to_ms"] = uniform(0, 3'600'000);
            events.push_back(std::move(e));
        }
    }

    if (chance(0.05)) {  // occasional DRM failure
        json e = make_event(ctx, "drm_error", t);
        e["error_code"] = std::string(pick(kDrmErrors));
        events.push_back(std::move(e));
    }

    t += uniform(5'000, 30'000);
    {
        json e = make_event(ctx, "playback_end", t);
        e["watch_time_ms"] = t - start_time_ms;
        events.push_back(std::move(e));
    }

    return events;
}

}  // namespace pulsedb::sdk
