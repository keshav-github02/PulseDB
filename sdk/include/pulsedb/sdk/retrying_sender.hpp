#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <random>

#include <nlohmann/json.hpp>

#include "pulsedb/sdk/backoff.hpp"
#include "pulsedb/sdk/event_sink.hpp"

namespace pulsedb::sdk {

/// Whether a failed send is worth retrying: transport errors (status 0) and
/// server-side errors (5xx) are transient, as are 408/425/429. Every other 4xx
/// is permanent -- the payload itself is unacceptable, so retrying cannot help.
///
/// This classification decides whether SpoolingSender keeps a batch or discards
/// it, so a code misfiled as permanent is data thrown away, not just a retry
/// skipped.
[[nodiscard]] bool is_retryable(const SendResult& result);

/// Wraps an EventSink and retries transient failures using a BackoffPolicy.
///
/// The wait between attempts is performed through an injectable sleeper,
/// so the retry behaviour can be unit-tested without real delays. The
/// default sleeper uses std::this_thread::sleep_for.
///
/// Delays are jittered (see BackoffPolicy::jitter) so a fleet of clients does
/// not retry in lockstep after a shared outage. Not thread-safe: one instance
/// per producer thread.
class RetryingSender : public EventSink {
public:
    using Sleeper = std::function<void(std::chrono::milliseconds)>;

    /// @param seed  PRNG seed for jitter; 0 draws one from std::random_device so
    ///              separate processes do not share a retry schedule. Pass a
    ///              fixed value to make jitter reproducible in a test.
    RetryingSender(EventSink& sink, BackoffPolicy policy, Sleeper sleeper = {},
                   std::uint64_t seed = 0);

    /// Attempt delivery, retrying transient failures per the policy.
    /// Returns the last result (success, or the final failure).
    [[nodiscard]] SendResult send(const nlohmann::json& events) override;

    /// Number of retries (attempts beyond the first) performed so far.
    int total_retries() const noexcept { return total_retries_; }

    /// The policy delay for @p attempt with jitter applied. Exposed so the
    /// jitter distribution itself is testable rather than only observable
    /// through sleep side effects.
    [[nodiscard]] std::chrono::milliseconds jittered_delay_for(int attempt);

private:
    EventSink& sink_;
    BackoffPolicy policy_;
    Sleeper sleeper_;
    std::mt19937_64 rng_;
    int total_retries_ = 0;
};

}  // namespace pulsedb::sdk
