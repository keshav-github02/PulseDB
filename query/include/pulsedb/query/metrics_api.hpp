#pragma once

#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "pulsedb/aggregation/aggregation_engine.hpp"

namespace pulsedb::query {

/// Turns aggregation-engine queries into JSON responses.
///
/// This layer holds no HTTP or socket state, so every response shape can be
/// unit-tested directly. QueryServer is the thin adapter that binds these
/// methods to HTTP routes.
class MetricsApi {
public:
    explicit MetricsApi(const aggregation::AggregationEngine& engine) : engine_(engine) {}

    /// GET /metrics -- platform-wide totals plus the number of minutes tracked.
    nlohmann::json overall() const;

    /// GET /metrics/live -- the @p minutes most recent per-minute buckets.
    nlohmann::json live(std::size_t minutes) const;

    /// GET /metrics/range -- per-minute buckets within [from_ms, to_ms].
    nlohmann::json range(std::int64_t from_ms, std::int64_t to_ms) const;

    /// GET /metrics/player -- per-player metric breakdown.
    nlohmann::json by_player() const;

    /// GET /metrics/device -- per-device metric breakdown.
    nlohmann::json by_device() const;

private:
    const aggregation::AggregationEngine& engine_;
};

}  // namespace pulsedb::query
