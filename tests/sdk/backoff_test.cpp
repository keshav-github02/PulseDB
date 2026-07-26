#include "pulsedb/sdk/backoff.hpp"

#include <gtest/gtest.h>

namespace {

using pulsedb::sdk::BackoffPolicy;
using namespace std::chrono_literals;

TEST(BackoffPolicyTest, FirstAttemptUsesInitialDelay) {
    BackoffPolicy p;
    p.initial_delay = 100ms;
    EXPECT_EQ(p.delay_for(0), 100ms);
}

TEST(BackoffPolicyTest, GrowsExponentially) {
    BackoffPolicy p;
    p.initial_delay = 100ms;
    p.multiplier = 2.0;
    p.max_delay = 10'000ms;
    EXPECT_EQ(p.delay_for(0), 100ms);
    EXPECT_EQ(p.delay_for(1), 200ms);
    EXPECT_EQ(p.delay_for(2), 400ms);
    EXPECT_EQ(p.delay_for(3), 800ms);
}

TEST(BackoffPolicyTest, ClampsToMaxDelay) {
    BackoffPolicy p;
    p.initial_delay = 100ms;
    p.multiplier = 2.0;
    p.max_delay = 500ms;
    EXPECT_EQ(p.delay_for(10), 500ms);
    EXPECT_LE(p.delay_for(3), 500ms);
}

TEST(BackoffPolicyTest, NegativeAttemptTreatedAsFirst) {
    BackoffPolicy p;
    p.initial_delay = 100ms;
    EXPECT_EQ(p.delay_for(-5), 100ms);
}

}  // namespace
