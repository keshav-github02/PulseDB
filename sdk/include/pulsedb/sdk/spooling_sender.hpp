#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

#include "pulsedb/sdk/event_sink.hpp"
#include "pulsedb/sdk/spool_store.hpp"

namespace pulsedb::sdk {

/// An EventSink decorator that adds offline durability.
///
/// On send(), it forwards to a downstream sink; if delivery fails, the batch
/// is persisted to a SpoolStore so events survive an outage. replay() drains
/// the spool once connectivity returns.
///
/// Replay stops at the first batch that fails *transiently* -- still offline --
/// so ordering is preserved and the remainder is retried later. A batch the
/// collector rejects permanently (a 4xx: the payload itself is unacceptable) is
/// discarded instead, because retrying it can never succeed and stopping on it
/// would wedge the spool forever: nothing behind it would ever be delivered,
/// and every new offline batch would pile up behind it indefinitely.
class SpoolingSender : public EventSink {
public:
    struct ReplayResult {
        std::size_t replayed = 0;  ///< Batches delivered and removed.
        std::size_t failed = 0;    ///< Batches that still could not be sent.
        std::size_t discarded = 0; ///< Batches dropped as permanently rejected.
    };

    SpoolingSender(EventSink& downstream, SpoolStore& spool);

    /// Forward to the downstream sink; spool the batch on failure.
    [[nodiscard]] SendResult send(const nlohmann::json& events) override;

    /// Attempt to deliver every spooled batch, oldest-first. Stops at the first
    /// transient failure; corrupt and permanently-rejected batches are dropped.
    [[nodiscard]] ReplayResult replay();

    /// Number of batches this instance has spooled.
    std::size_t spooled_count() const noexcept { return spooled_; }

    /// Batches dropped because the collector rejected them permanently.
    std::size_t discarded_count() const noexcept { return discarded_; }

    /// Batches that could not even be written to the spool, and so are lost.
    /// Distinct from spooled_count(): these did *not* reach disk.
    std::size_t spool_failure_count() const noexcept { return spool_failures_; }

private:
    EventSink& downstream_;
    SpoolStore& spool_;
    std::size_t spooled_ = 0;
    std::size_t discarded_ = 0;
    std::size_t spool_failures_ = 0;
};

}  // namespace pulsedb::sdk
