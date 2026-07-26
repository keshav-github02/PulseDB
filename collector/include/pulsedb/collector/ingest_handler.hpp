#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pulsedb/core/event_batch.hpp"

namespace pulsedb::collector {

/// Outcome of validating an ingest request. Each value maps to one HTTP
/// status code at the transport layer.
enum class IngestStatus {
    kAccepted,         ///< Batch is valid.            -> 202 Accepted
    kInvalidJson,      ///< Body is not valid JSON.    -> 400 Bad Request
    kInvalidSchema,    ///< JSON shape is unexpected.  -> 422 Unprocessable Entity
    kPayloadTooLarge,  ///< Body exceeds the byte cap. -> 413 Payload Too Large
};

/// Tunable validation limits applied to each incoming batch.
///
/// Every field here exists to bound the work and memory a single untrusted
/// request can cost us. They are enforced in cheapest-first order: byte size,
/// then nesting depth, then event count, then per-event checks.
struct IngestOptions {
    /// Bodies larger than this are rejected outright. Also installed on the
    /// HTTP server via set_payload_max_length(), so oversized requests are
    /// refused at the transport layer *before* the body is buffered into
    /// memory -- the count/schema limits below can only run after the whole
    /// body has already been read, so they are no defence against a large
    /// payload on their own.
    std::size_t max_body_bytes = 8u * 1024u * 1024u;  // 8 MiB

    /// Maximum JSON bracket nesting. A valid batch is an array of flat event
    /// objects (depth 2-3), so anything deeper cannot be a batch. The cap also
    /// bounds DOM amplification: each opening bracket costs one byte on the
    /// wire but a heap-allocated node once parsed, so pathological nesting
    /// inflates memory far beyond max_body_bytes.
    std::size_t max_json_depth = 32;

    /// Batches with more events than this are rejected (bounds fan-out).
    std::size_t max_events_per_batch = 10'000;

    /// How far in the past and future an event's own "timestamp" may sit,
    /// relative to the moment we received it.
    ///
    /// An event's timestamp addresses a permanent minute bucket downstream, so
    /// an unbounded timestamp is an unbounded allocation: the reachable key
    /// space is ~1.5e14 minutes and buckets are never evicted. Bounding the
    /// window also keeps every key inside storage::is_valid_timestamp_ms(), so
    /// the silent chrono wrap that yields negative years cannot be reached.
    ///
    /// A timestamp outside the window rejects the whole batch rather than
    /// dropping the offending event. That is deliberate: a client with a
    /// broken clock gets loud, immediate feedback instead of silently losing a
    /// fraction of its data. The trade-off is that one bad event costs the
    /// batch, so a per-event drop-and-count policy would be the right change
    /// if partial acceptance ever matters more than the signal.
    std::chrono::milliseconds max_lateness = std::chrono::hours(24 * 7);
    std::chrono::milliseconds max_future_skew = std::chrono::minutes(5);

    /// Cap on the length of the "player" and "device" segment labels. These
    /// become permanent map keys in the aggregation engine, so unbounded
    /// strings are unbounded memory.
    std::size_t max_label_length = 128;

    /// Inclusive upper bounds on the numeric metric payloads, all of which are
    /// rejected outright when negative.
    ///
    /// These are not cosmetic. Each payload is summed into an *unsigned* 64-bit
    /// counter that only ever grows and is never recomputed from source, so a
    /// single out-of-range sample corrupts the corresponding average forever --
    /// and a *negative* one converts to ~1.8e19 and destroys it outright. The
    /// poisoned value is a legitimate unsigned integer, so it also round-trips
    /// through the snapshot and survives a restart. One unauthenticated request
    /// was enough.
    ///
    /// The bounds are deliberately generous: they are there to exclude the
    /// physically impossible, not to second-guess a client's measurements.
    int max_startup_time_ms = 600'000;             // 10 minutes to first frame
    int max_buffer_duration_ms = 3'600'000;        // 1 hour in a single stall
    int max_bitrate_kbps = 1'000'000;              // 1 Gbps
    std::int64_t max_watch_time_ms = 86'400'000;   // 24 hours in one session

    /// When true, every event must carry a non-empty string "event_type".
    bool require_event_type = true;
};

/// The result of validating one ingest request.
struct IngestResult {
    IngestStatus status;
    std::string message;                    ///< Human-readable detail.
    std::optional<core::EventBatch> batch;  ///< Present iff status == kAccepted.
};

/// Stateless validator that turns a raw request body into an EventBatch.
///
/// This class holds no network or socket state, so it can be unit-tested
/// in isolation from the HTTP server. It accepts either a bare JSON array
/// of event objects, or an envelope object of the form {"events": [...]}.
class IngestHandler {
public:
    explicit IngestHandler(IngestOptions options = {}) : options_(options) {}

    /// Validate @p body and, on success, build a batch stamped with
    /// @p ingest_time. Never throws; malformed input is reported via the
    /// returned IngestResult::status.
    [[nodiscard]] IngestResult handle(std::string_view body,
                                     core::TimePoint ingest_time) const;

    [[nodiscard]] const IngestOptions& options() const noexcept { return options_; }

private:
    IngestOptions options_;
};

}  // namespace pulsedb::collector
