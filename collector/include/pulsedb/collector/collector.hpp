#pragma once

#include <memory>
#include <string>

#include "pulsedb/collector/ingest_handler.hpp"
#include "pulsedb/core/event_batch.hpp"
#include "pulsedb/queue/bounded_blocking_queue.hpp"

namespace pulsedb::collector {

/// The queue type shared between the collector (producer) and the worker
/// pool (consumer).
using EventQueue = queue::BoundedBlockingQueue<core::EventBatch>;

/// Runtime configuration for the collector server.
struct CollectorConfig {
    std::string host = "0.0.0.0";        ///< Interface to bind.
    int port = 8080;                     ///< TCP port to bind.
    std::string ingest_path = "/v1/events";  ///< POST route for batches.
    IngestOptions ingest{};              ///< Validation limits.
};

/// HTTP ingestion server.
///
/// Receives POSTed event batches on `ingest_path`, validates them via
/// IngestHandler, stamps them with an ingestion time, and enqueues them
/// for the worker pool. Responds:
///   * 202 Accepted        when the batch is queued,
///   * 400 / 422           when the body is malformed,
///   * 503 Service Unavailable when the queue is saturated, so that SDK
///     clients back off and retry rather than the acceptor thread stalling.
///
/// The httplib/socket machinery is hidden behind a PImpl, keeping this
/// header free of any networking dependency.
class Collector {
public:
    Collector(CollectorConfig config, EventQueue& queue);
    ~Collector();

    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    /// Bind to the configured host/port and serve until stop(). Blocks the
    /// calling thread. @return false if binding or listening failed.
    [[nodiscard]] bool listen();

    /// Bind to the configured host but an OS-assigned ("any") port and
    /// return the chosen port, or -1 on failure. Intended for tests; pair
    /// with listen_after_bind().
    [[nodiscard]] int bind_to_any_port();

    /// Serve until stop(), following a successful bind_to_any_port().
    /// Blocks the calling thread.
    void listen_after_bind();

    /// Ask the server to stop, unblocking listen()/listen_after_bind().
    /// Thread-safe and idempotent.
    void stop();

    /// True while the server is accepting connections.
    [[nodiscard]] bool is_running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulsedb::collector
