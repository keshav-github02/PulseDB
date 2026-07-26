#include "pulsedb/sdk/retrying_sender.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <utility>
#include <vector>

#include "pulsedb/sdk/event_sink.hpp"

namespace {

using namespace pulsedb::sdk;
using namespace std::chrono_literals;

// A sink whose responses are scripted, so retry behaviour is deterministic.
class ScriptedSink : public EventSink {
public:
    explicit ScriptedSink(std::vector<SendResult> script) : script_(std::move(script)) {}

    SendResult send(const nlohmann::json&) override {
        ++calls;
        if (next_ < script_.size()) {
            return script_[next_++];
        }
        return script_.empty() ? SendResult::failure(500, "err") : script_.back();
    }

    int calls = 0;

private:
    std::vector<SendResult> script_;
    std::size_t next_ = 0;
};

BackoffPolicy fast_policy(int attempts) {
    BackoffPolicy p;
    p.max_attempts = attempts;
    p.initial_delay = 1ms;
    p.max_delay = 1ms;
    return p;
}

TEST(IsRetryableTest, TransportAnd5xxAreRetryable) {
    EXPECT_TRUE(is_retryable(SendResult::failure(0, "transport")));
    EXPECT_TRUE(is_retryable(SendResult::failure(500, "server")));
    EXPECT_TRUE(is_retryable(SendResult::failure(503, "busy")));
}

TEST(IsRetryableTest, SuccessAnd4xxAreNotRetryable) {
    EXPECT_FALSE(is_retryable(SendResult::success(202)));
    EXPECT_FALSE(is_retryable(SendResult::failure(400, "bad")));
    EXPECT_FALSE(is_retryable(SendResult::failure(422, "schema")));
    EXPECT_FALSE(is_retryable(SendResult::failure(404, "no route")));
    EXPECT_FALSE(is_retryable(SendResult::failure(413, "too large")));
}

// Regression: 429 was classified permanent, and SpoolingSender *discards*
// anything permanent -- so a rate-limited batch was deleted at exactly the moment
// the collector was asking the client to slow down. Backpressure that destroys
// the payload is not backpressure.
TEST(IsRetryableTest, TransientFourXxCodesAreRetryable) {
    EXPECT_TRUE(is_retryable(SendResult::failure(408, "request timeout")));
    EXPECT_TRUE(is_retryable(SendResult::failure(425, "too early")));
    EXPECT_TRUE(is_retryable(SendResult::failure(429, "rate limited")));
}

TEST(RetryingSenderTest, RetriesARateLimitedBatch) {
    ScriptedSink sink{{SendResult::failure(429, "slow down"), SendResult::success(202)}};
    int sleeps = 0;
    RetryingSender sender(sink, fast_policy(5), [&](std::chrono::milliseconds) { ++sleeps; });

    const auto r = sender.send(nlohmann::json::array());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(sink.calls, 2);
    EXPECT_EQ(sleeps, 1);
}

// --- Jitter ------------------------------------------------------------------
//
// Without jitter every client retries on the same schedule, so a collector
// restart synchronises the fleet into waves that keep knocking it back down.

TEST(RetryingSenderTest, JitterStaysWithinTheBaseDelayAndVaries) {
    BackoffPolicy policy;
    policy.initial_delay = 1000ms;
    policy.max_delay = 1000ms;
    policy.jitter = 0.5;

    ScriptedSink sink{{SendResult::success(202)}};
    RetryingSender sender(sink, policy, [](std::chrono::milliseconds) {}, /*seed=*/12345);

    std::set<long long> observed;
    for (int i = 0; i < 200; ++i) {
        const auto delay = sender.jittered_delay_for(0);
        // Randomised downward only: scaling up would break max_delay's promise
        // to be a hard ceiling.
        EXPECT_GE(delay, 500ms);
        EXPECT_LE(delay, 1000ms);
        observed.insert(delay.count());
    }
    EXPECT_GT(observed.size(), 10u) << "delays must actually spread, not be constant";
}

TEST(RetryingSenderTest, ZeroJitterIsExactlyThePolicyDelay) {
    BackoffPolicy policy;
    policy.initial_delay = 100ms;
    policy.max_delay = 5000ms;
    policy.jitter = 0.0;

    ScriptedSink sink{{SendResult::success(202)}};
    RetryingSender sender(sink, policy, [](std::chrono::milliseconds) {}, /*seed=*/7);

    for (int attempt = 0; attempt < 4; ++attempt) {
        EXPECT_EQ(sender.jittered_delay_for(attempt), policy.delay_for(attempt)) << attempt;
    }
}

TEST(RetryingSenderTest, JitterIsReproducibleForAFixedSeed) {
    BackoffPolicy policy;
    policy.initial_delay = 800ms;
    policy.max_delay = 800ms;
    policy.jitter = 0.4;

    const auto sample = [&policy] {
        ScriptedSink sink{{SendResult::success(202)}};
        RetryingSender sender(sink, policy, [](std::chrono::milliseconds) {}, /*seed=*/99);
        std::vector<long long> delays;
        for (int i = 0; i < 8; ++i) {
            delays.push_back(sender.jittered_delay_for(1).count());
        }
        return delays;
    };
    EXPECT_EQ(sample(), sample());
}

TEST(RetryingSenderTest, SucceedsOnFirstTryWithoutSleeping) {
    ScriptedSink sink{{SendResult::success(202)}};
    int sleeps = 0;
    RetryingSender sender(sink, fast_policy(5), [&](std::chrono::milliseconds) { ++sleeps; });

    const auto r = sender.send(nlohmann::json::array());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(sink.calls, 1);
    EXPECT_EQ(sleeps, 0);
    EXPECT_EQ(sender.total_retries(), 0);
}

TEST(RetryingSenderTest, RetriesTransientFailureThenSucceeds) {
    ScriptedSink sink{{SendResult::failure(503, "busy"),
                       SendResult::failure(0, "transport"),
                       SendResult::success(202)}};
    int sleeps = 0;
    RetryingSender sender(sink, fast_policy(5), [&](std::chrono::milliseconds) { ++sleeps; });

    const auto r = sender.send(nlohmann::json::array());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(sink.calls, 3);
    EXPECT_EQ(sleeps, 2);
    EXPECT_EQ(sender.total_retries(), 2);
}

TEST(RetryingSenderTest, DoesNotRetryPermanentFailure) {
    ScriptedSink sink{{SendResult::failure(400, "bad json")}};
    int sleeps = 0;
    RetryingSender sender(sink, fast_policy(5), [&](std::chrono::milliseconds) { ++sleeps; });

    const auto r = sender.send(nlohmann::json::array());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(sink.calls, 1);
    EXPECT_EQ(sleeps, 0);
}

TEST(RetryingSenderTest, GivesUpAfterMaxAttempts) {
    ScriptedSink sink{{SendResult::failure(503, "busy")}};  // always busy
    int sleeps = 0;
    RetryingSender sender(sink, fast_policy(3), [&](std::chrono::milliseconds) { ++sleeps; });

    const auto r = sender.send(nlohmann::json::array());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(sink.calls, 3);   // 3 attempts total
    EXPECT_EQ(sleeps, 2);       // slept between the 3 attempts
}

}  // namespace
