#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "pulsedb/aggregation/aggregation_engine.hpp"
#include "pulsedb/query/runtime_status.hpp"

namespace pulsedb::query {

/// HTTP server exposing the read-only metrics API.
///
/// Runs independently of the ingestion collector (separate port), so the
/// write path and the read path do not share a server. Responses carry
/// permissive CORS headers so a browser dashboard can call it directly.
/// httplib is hidden behind a PImpl, as in the collector.
class QueryServer {
public:
    struct Config {
        std::string host = "0.0.0.0";
        int port = 8081;

        /// Most minute buckets any single response will serialise.
        ///
        /// Both /metrics/live and /metrics/range took an unbounded window, so one
        /// short unauthenticated GET could ask for the entire store: measured at
        /// 5,000 buckets, `?minutes=999999999` turned a ~60-byte request into a
        /// 1.25 MB response (~20,000x amplification), and the store mutex is held
        /// for the whole traversal, so the cost lands on the ingest path too. With
        /// no authentication and no rate limiting on this port, and buckets never
        /// evicted, that amplification grows with uptime -- 90 days of buckets is
        /// ~26x the measured store.
        ///
        /// 1,440 is a day of minutes: comfortably more than any dashboard asks for
        /// (the shipped one requests 15) while keeping a worst-case response in the
        /// hundreds of kilobytes. Callers wanting more must page via
        /// /metrics/range, which bounds the *window*, not just the reply.
        std::size_t max_response_minutes = 1'440;
    };

    /// @param runtime optional source of live operational metrics for the
    ///        GET /status endpoint; if null, /status reports 503.
    QueryServer(Config config, const aggregation::AggregationEngine& engine,
                const RuntimeStatus* runtime = nullptr);
    ~QueryServer();

    QueryServer(const QueryServer&) = delete;
    QueryServer& operator=(const QueryServer&) = delete;

    /// Bind to the configured host/port and serve until stop(). Blocking.
    [[nodiscard]] bool listen();

    /// Bind to an OS-assigned port (for tests); returns the port or -1.
    [[nodiscard]] int bind_to_any_port();
    void listen_after_bind();

    void stop();
    [[nodiscard]] bool is_running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulsedb::query
