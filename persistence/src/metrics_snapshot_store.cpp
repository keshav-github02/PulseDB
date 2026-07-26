#include "pulsedb/persistence/metrics_snapshot_store.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "pulsedb/processor/metrics.hpp"
#include "pulsedb/storage/minute_key.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pulsedb::persistence {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

/// One persisted counter: its wire name, the field it maps to, and the snapshot
/// version that introduced it.
///
/// A member pointer rather than an index-to-pointer switch, so adding a counter
/// is a single line here instead of two places that can silently disagree, and
/// so the mapping cannot have an unreachable `default` that returns nullptr.
struct CounterField {
    const char* key;
    std::uint64_t processor::MetricsSnapshot::* member;
    int since_version;  ///< Absent is legal when reading an older snapshot.
};

// The raw counters, in one place, so the writer and the reader cannot drift.
constexpr CounterField kCounterFields[] = {
    {"total_events", &processor::MetricsSnapshot::total_events, 1},
    {"total_views", &processor::MetricsSnapshot::total_views, 1},
    {"startup_samples", &processor::MetricsSnapshot::startup_samples, 1},
    {"startup_time_ms_sum", &processor::MetricsSnapshot::startup_time_ms_sum, 1},
    {"buffer_count", &processor::MetricsSnapshot::buffer_count, 1},
    {"buffer_samples", &processor::MetricsSnapshot::buffer_samples, 2},
    {"buffer_duration_ms_sum", &processor::MetricsSnapshot::buffer_duration_ms_sum, 1},
    {"error_count", &processor::MetricsSnapshot::error_count, 1},
    {"watch_time_ms_sum", &processor::MetricsSnapshot::watch_time_ms_sum, 1},
    {"bitrate_samples", &processor::MetricsSnapshot::bitrate_samples, 1},
    {"bitrate_kbps_sum", &processor::MetricsSnapshot::bitrate_kbps_sum, 1},
};

// Raw counters only (not the derived rates) so the round trip is exact.
json metrics_to_json(const processor::MetricsSnapshot& m) {
    json j;
    for (const auto& field : kCounterFields) {
        j[field.key] = m.*field.member;
    }
    return j;
}

/// Strict inverse of metrics_to_json: every counter the snapshot's own version
/// defines must be present and a non-negative integer. Lenient defaulting would
/// turn a corrupt file into plausible-looking but wrong metrics, which is worse
/// than refusing it.
///
/// The one permitted omission is a counter introduced *after* @p version. A v1
/// snapshot genuinely does not know buffer_samples, so it restores as 0, which
/// makes avg_buffer_ms() report "no data" for historical buckets rather than
/// inventing a number from a mismatched denominator. That is the honest answer,
/// and it is what lets an upgrade keep its history instead of discarding it.
bool parse_metrics(const json& j, int version, processor::MetricsSnapshot& out,
                   std::string& error) {
    if (!j.is_object()) {
        error = "metrics entry is not an object";
        return false;
    }
    for (const auto& field : kCounterFields) {
        const auto it = j.find(field.key);
        if (it == j.end() && field.since_version > version) {
            out.*field.member = 0;
            continue;
        }
        if (it == j.end() || !it->is_number_unsigned()) {
            error = std::string("metrics field '") + field.key +
                    "' is missing or not a non-negative integer";
            return false;
        }
        out.*field.member = it->get<std::uint64_t>();
    }
    return true;
}

bool parse_int_field(const json& entry, const char* key, int lo, int hi, int& out,
                     std::string& error) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_number_integer()) {
        error = std::string("minute entry field '") + key + "' is missing or not an integer";
        return false;
    }
    const auto value = it->get<std::int64_t>();
    if (value < lo || value > hi) {
        error = std::string("minute entry field '") + key + "' = " +
                std::to_string(value) + " is out of range [" + std::to_string(lo) + ", " +
                std::to_string(hi) + "]";
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

/// Calendar fields are range-checked so a corrupt file cannot inject a bucket
/// that to_iso() would render as a malformed timestamp.
bool parse_minute_key(const json& entry, storage::MinuteKey& out, std::string& error) {
    return parse_int_field(entry, "y", 1970, 9999, out.year, error) &&
           parse_int_field(entry, "mo", 1, 12, out.month, error) &&
           parse_int_field(entry, "d", 1, 31, out.day, error) &&
           parse_int_field(entry, "h", 0, 23, out.hour, error) &&
           parse_int_field(entry, "mi", 0, 59, out.minute, error);
}

using SegmentEntries = std::vector<std::pair<std::string, processor::MetricsSnapshot>>;

bool parse_segments(const json& snapshot, const char* key, int version,
                    SegmentEntries& out, std::string& error) {
    const auto it = snapshot.find(key);
    if (it == snapshot.end()) {
        return true;  // absent is legal; an empty engine writes an empty array
    }
    if (!it->is_array()) {
        error = std::string("'") + key + "' is not an array";
        return false;
    }
    for (const auto& entry : *it) {
        if (!entry.is_object()) {
            error = std::string("'") + key + "' contains a non-object entry";
            return false;
        }
        const auto name = entry.find("name");
        const auto metrics = entry.find("m");
        if (name == entry.end() || !name->is_string()) {
            error = std::string("'") + key + "' entry is missing a string 'name'";
            return false;
        }
        if (metrics == entry.end()) {
            error = std::string("'") + key + "' entry is missing 'm'";
            return false;
        }
        processor::MetricsSnapshot parsed;
        if (!parse_metrics(*metrics, version, parsed, error)) {
            return false;
        }
        out.emplace_back(name->get<std::string>(), parsed);
    }
    return true;
}

/// Everything a snapshot describes, fully validated and not yet applied.
struct ParsedSnapshot {
    std::vector<std::pair<storage::MinuteKey, processor::MetricsSnapshot>> minutes;
    SegmentEntries players;
    SegmentEntries devices;
};

std::optional<ParsedSnapshot> parse_snapshot(const json& snapshot, std::string& error) {
    if (!snapshot.is_object()) {
        error = "snapshot root is not an object";
        return std::nullopt;
    }
    const auto version = snapshot.find("version");
    if (version == snapshot.end() || !version->is_number_integer()) {
        error = "snapshot is missing an integer 'version'";
        return std::nullopt;
    }
    const int file_version = version->get<int>();
    if (file_version < kMinSupportedSnapshotVersion || file_version > kSnapshotVersion) {
        error = "unsupported snapshot version " + std::to_string(file_version) +
                " (this build writes version " + std::to_string(kSnapshotVersion) +
                " and reads versions " + std::to_string(kMinSupportedSnapshotVersion) +
                "-" + std::to_string(kSnapshotVersion) + ")";
        return std::nullopt;
    }

    ParsedSnapshot parsed;
    if (const auto it = snapshot.find("minutes"); it != snapshot.end()) {
        if (!it->is_array()) {
            error = "'minutes' is not an array";
            return std::nullopt;
        }
        for (const auto& entry : *it) {
            if (!entry.is_object()) {
                error = "'minutes' contains a non-object entry";
                return std::nullopt;
            }
            const auto metrics = entry.find("m");
            if (metrics == entry.end()) {
                error = "minute entry is missing 'm'";
                return std::nullopt;
            }
            storage::MinuteKey key;
            processor::MetricsSnapshot m;
            if (!parse_minute_key(entry, key, error) ||
                !parse_metrics(*metrics, file_version, m, error)) {
                return std::nullopt;
            }
            parsed.minutes.emplace_back(key, m);
        }
    }
    if (!parse_segments(snapshot, "players", file_version, parsed.players, error) ||
        !parse_segments(snapshot, "devices", file_version, parsed.devices, error)) {
        return std::nullopt;
    }
    return parsed;
}

void set_error(std::string* sink, std::string message) {
    if (sink != nullptr) {
        *sink = std::move(message);
    }
}

/// Force this file's contents out of the OS cache onto stable storage.
bool flush_to_disk(std::FILE* file) {
    if (std::fflush(file) != 0) {
        return false;
    }
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

/// Persist the rename itself. Without this the new name can be lost on a crash
/// even though the file's contents were durable. Windows offers no directory
/// handle to flush, so this is a POSIX-only guarantee.
void sync_directory([[maybe_unused]] const fs::path& dir) {
#ifndef _WIN32
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

}  // namespace

json to_json(const aggregation::AggregationEngine& engine) {
    json minutes = json::array();
    for (const auto& point : engine.points()) {
        minutes.push_back({
            {"y", point.key.year},
            {"mo", point.key.month},
            {"d", point.key.day},
            {"h", point.key.hour},
            {"mi", point.key.minute},
            {"m", metrics_to_json(point.metrics)},
        });
    }

    json players = json::array();
    for (const auto& segment : engine.by_player()) {
        players.push_back({{"name", segment.name}, {"m", metrics_to_json(segment.metrics)}});
    }

    json devices = json::array();
    for (const auto& segment : engine.by_device()) {
        devices.push_back({{"name", segment.name}, {"m", metrics_to_json(segment.metrics)}});
    }

    return json{{"version", kSnapshotVersion},
                {"minutes", minutes},
                {"players", players},
                {"devices", devices}};
}

bool restore(aggregation::AggregationEngine& engine, const json& snapshot,
             std::string* error) {
    std::string reason;
    const auto parsed = parse_snapshot(snapshot, reason);
    if (!parsed) {
        set_error(error, std::move(reason));
        return false;
    }

    // Validation is complete, so from here nothing can fail partway and leave
    // the engine holding a fraction of a snapshot.
    for (const auto& [key, metrics] : parsed->minutes) {
        engine.restore_minute(key, metrics);
    }
    for (const auto& [name, metrics] : parsed->players) {
        engine.restore_player(name, metrics);
    }
    for (const auto& [name, metrics] : parsed->devices) {
        engine.restore_device(name, metrics);
    }
    return true;
}

bool save(const aggregation::AggregationEngine& engine, const fs::path& path,
          std::string* error) {
    const fs::path temp = path.string() + ".tmp";
    try {
        if (path.has_parent_path()) {
            fs::create_directories(path.parent_path());
        }

        const std::string payload = to_json(engine).dump();

        // Written through a FILE* rather than an ofstream so the descriptor is
        // reachable for fsync, and so a short write is detectable.
        std::FILE* file = std::fopen(temp.string().c_str(), "wb");
        if (file == nullptr) {
            set_error(error, "could not open " + temp.string() + " for writing");
            return false;
        }
        const std::size_t written =
            payload.empty() ? 0 : std::fwrite(payload.data(), 1, payload.size(), file);
        const bool write_ok = written == payload.size();
        const bool flush_ok = write_ok && flush_to_disk(file);
        const bool close_ok = std::fclose(file) == 0;

        if (!write_ok || !flush_ok || !close_ok) {
            // Leave the previous snapshot in place; a truncated file must never
            // be promoted over a good one.
            std::error_code ignored;
            fs::remove(temp, ignored);
            set_error(error, !write_ok ? "short write to " + temp.string() +
                                             " (wrote " + std::to_string(written) + " of " +
                                             std::to_string(payload.size()) + " bytes)"
                                       : "failed to flush " + temp.string() + " to disk");
            return false;
        }

        fs::rename(temp, path);
        if (path.has_parent_path()) {
            sync_directory(path.parent_path());
        }
        return true;
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        set_error(error, std::string("snapshot save failed: ") + e.what());
        return false;
    }
}

bool load(aggregation::AggregationEngine& engine, const fs::path& path, std::string* error) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        set_error(error, "no snapshot at " + path.string());
        return false;
    }
    json snapshot;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            set_error(error, "could not open " + path.string());
            return false;
        }
        snapshot = json::parse(in);
    } catch (const std::exception& e) {
        set_error(error, "snapshot at " + path.string() + " is not valid JSON: " + e.what());
        return false;
    }
    return restore(engine, snapshot, error);
}

}  // namespace pulsedb::persistence
