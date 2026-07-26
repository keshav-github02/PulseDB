#pragma once

#include <chrono>
#include <cstddef>

#include <nlohmann/json.hpp>

namespace pulsedb::core {

/// Wall-clock source used for ingestion timestamps across the platform.
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

/// A batch of playback events received from an SDK, stamped with the time the
/// collector ingested it.
///
/// The events are held as a parsed JSON DOM, not as typed Events: the collector
/// validates structure and defers *interpretation* (JSON -> typed Event ->
/// metrics) to the worker pool.
///
/// Be clear about what this does and does not buy, because the obvious reading
/// -- "the collector does no heavy work" -- is not true. json::parse already ran
/// on the acceptor thread inside IngestHandler, so the expensive part (DOM
/// construction and allocation) is already paid there; only the cheap part
/// (string-keyed field extraction) is deferred. The DOM also costs roughly
/// 10-20x the wire bytes, so a queue bounded by batch count has a far higher
/// memory ceiling than the count suggests.
///
/// Carrying the raw body (a std::string) instead, and parsing in the workers,
/// would move all parsing off the acceptor thread and shrink queue memory to
/// wire size. The cost is that schema rejections become asynchronous, so a
/// malformed event could no longer be answered with a 422. That trade has not
/// been made yet; this comment exists so the current split is not mistaken for
/// the one it was originally described as.
struct EventBatch {
    nlohmann::json events;    ///< JSON array of raw event objects.
    TimePoint ingest_time{};  ///< Server-side ingestion timestamp.

    /// Number of events in the batch (0 if the payload is not an array).
    std::size_t size() const {
        return events.is_array() ? events.size() : 0;
    }

    /// True when the batch carries no events.
    bool empty() const { return size() == 0; }
};

}  // namespace pulsedb::core
