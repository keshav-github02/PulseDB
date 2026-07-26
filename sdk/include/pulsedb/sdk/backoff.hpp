#pragma once

#include <chrono>

namespace pulsedb::sdk {

/// Exponential-backoff schedule for retrying failed deliveries.
///
/// The delay before retry number @p attempt (0-based) is
/// `initial_delay * multiplier^attempt`, capped at `max_delay`.
struct BackoffPolicy {
    int max_attempts = 5;                       ///< Total send attempts (>= 1).
    std::chrono::milliseconds initial_delay{100};
    double multiplier = 2.0;
    std::chrono::milliseconds max_delay{5'000};

    /// Fraction of the computed delay to randomise away, in [0, 1].
    ///
    /// Without jitter every client retries on the same schedule, so a collector
    /// restart synchronises the entire fleet into retry waves at 100/200/400/...
    /// ms -- the herd that turns a brief blip into a sustained outage, because
    /// each wave arrives together and knocks the collector back down. 0 disables
    /// it (and keeps delay_for() exactly reproducible, which is what the policy
    /// tests assert).
    double jitter = 0.2;

    /// Deterministic delay before the retry indexed by @p attempt (0-based),
    /// clamped to [initial_delay, max_delay]. Jitter is *not* applied here --
    /// RetryingSender applies it, so this stays a pure function of the policy.
    std::chrono::milliseconds delay_for(int attempt) const;
};

}  // namespace pulsedb::sdk
