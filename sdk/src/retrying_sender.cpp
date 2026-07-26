#include "pulsedb/sdk/retrying_sender.hpp"

#include <algorithm>
#include <random>
#include <thread>
#include <utility>

namespace pulsedb::sdk {

bool is_retryable(const SendResult& result) {
    if (result.ok) {
        return false;
    }
    // Transport failures and server-side errors are transient.
    if (result.status == 0 || result.status >= 500) {
        return true;
    }
    // Three 4xx codes are transient despite the class, and treating them as
    // permanent is data loss, not a missed optimisation: SpoolingSender discards
    // anything is_retryable() rejects, so a rate-limited batch used to be thrown
    // away at exactly the moment the collector was asking the client to slow
    // down -- shedding load by deleting the payload.
    //   408 Request Timeout  -- the request never got a verdict.
    //   425 Too Early        -- replay protection; the same bytes may be accepted.
    //   429 Too Many Requests-- explicit backpressure; retry after a delay.
    return result.status == 408 || result.status == 425 || result.status == 429;
}

RetryingSender::RetryingSender(EventSink& sink, BackoffPolicy policy, Sleeper sleeper,
                               std::uint64_t seed)
    : sink_(sink), policy_(policy), sleeper_(std::move(sleeper)) {
    if (!sleeper_) {
        sleeper_ = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
    }
    if (seed == 0) {
        std::random_device rd;
        seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }
    rng_.seed(seed);
}

std::chrono::milliseconds RetryingSender::jittered_delay_for(int attempt) {
    const auto base = policy_.delay_for(attempt);
    const double ratio = std::clamp(policy_.jitter, 0.0, 1.0);
    if (ratio <= 0.0 || base.count() <= 0) {
        return base;
    }
    // Randomise downward only, into [base * (1 - jitter), base]. Scaling up
    // would let a retry wait longer than max_delay, which the policy promises
    // is a hard ceiling.
    const double span = static_cast<double>(base.count()) * ratio;
    const double offset = std::uniform_real_distribution<double>(0.0, span)(rng_);
    const auto delay = base - std::chrono::milliseconds{static_cast<long long>(offset)};
    return std::max(delay, std::chrono::milliseconds{0});
}

SendResult RetryingSender::send(const nlohmann::json& events) {
    SendResult result;
    for (int attempt = 0; attempt < policy_.max_attempts; ++attempt) {
        result = sink_.send(events);
        if (result.ok || !is_retryable(result)) {
            return result;
        }
        // Failure is retryable; wait before the next attempt (if any).
        if (attempt + 1 < policy_.max_attempts) {
            ++total_retries_;
            sleeper_(jittered_delay_for(attempt));
        }
    }
    return result;  // attempts exhausted
}

}  // namespace pulsedb::sdk
