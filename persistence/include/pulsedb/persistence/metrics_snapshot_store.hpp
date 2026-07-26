#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "pulsedb/aggregation/aggregation_engine.hpp"

namespace pulsedb::persistence {

/// Snapshot format version written by to_json().
///
/// Version history:
///   1 -- ten raw counters per bucket/segment.
///   2 -- adds `buffer_samples`, so mean stall duration divides by the count of
///        stalls that *finished* rather than the count that *began*. A v1 file
///        restores with `buffer_samples` = 0, which reports mean stall duration
///        as "no data" for those buckets rather than a number derived from a
///        mismatched denominator.
///
/// Bump this when the on-disk shape changes. Purely additive changes should also
/// leave kMinSupportedSnapshotVersion alone so history survives the upgrade;
/// raise it only for a change that genuinely cannot be read.
inline constexpr int kSnapshotVersion = 2;

/// Oldest snapshot version restore() will accept. Files outside
/// [kMinSupportedSnapshotVersion, kSnapshotVersion] are rejected rather than
/// silently misread.
inline constexpr int kMinSupportedSnapshotVersion = 1;

/// Serialize the engine's full aggregate state (raw counters for every
/// minute bucket plus the player/device segments) to JSON.
[[nodiscard]] nlohmann::json to_json(const aggregation::AggregationEngine& engine);

/// Restore state from JSON by additively folding it into @p engine. Intended
/// for a freshly constructed engine at startup.
///
/// All-or-nothing: the snapshot is fully parsed and validated before any of it
/// is applied, so a file that goes bad partway through leaves @p engine
/// untouched rather than half-restored with permanently wrong totals.
///
/// Counters introduced after the snapshot's own version restore as 0; see
/// kSnapshotVersion for why that is the correct reading rather than a rejection.
///
/// @param error optional; set to a human-readable reason when false is returned.
/// @return false if the snapshot is malformed or its version is unsupported.
[[nodiscard]] bool restore(aggregation::AggregationEngine& engine,
                           const nlohmann::json& snapshot, std::string* error = nullptr);

/// Durably and atomically write a snapshot of @p engine to @p path.
///
/// Writes a temp file, verifies every write succeeded, flushes it to stable
/// storage, and only then renames it over @p path -- so a failure partway
/// through (a full disk, most obviously) leaves the previous snapshot intact
/// instead of replacing it with a truncated one.
///
/// @param error optional; set to a human-readable reason when false is returned.
/// @return false on any I/O error, with @p path left as it was.
[[nodiscard]] bool save(const aggregation::AggregationEngine& engine,
                        const std::filesystem::path& path, std::string* error = nullptr);

/// Load a snapshot from @p path into @p engine.
///
/// @param error optional; set to a human-readable reason when false is returned.
/// @return false if the file is absent, unreadable, not valid JSON, or fails
///         validation. The engine is left untouched in every failure case.
[[nodiscard]] bool load(aggregation::AggregationEngine& engine,
                        const std::filesystem::path& path, std::string* error = nullptr);

}  // namespace pulsedb::persistence
