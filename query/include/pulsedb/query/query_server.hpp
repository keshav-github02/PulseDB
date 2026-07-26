#pragma once

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
