#include "pulsedb/sdk/simulator.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "pulsedb/sdk/event_sink.hpp"
#include "pulsedb/sdk/retrying_sender.hpp"

namespace {

using namespace pulsedb::sdk;

// Captures every batch it receives so the simulator's batching and totals
// can be asserted without any network.
class CapturingSink : public EventSink {
public:
    SendResult send(const nlohmann::json& events) override {
        batch_sizes.push_back(events.size());
        total_events += events.size();
        return SendResult::success(202);
    }

    std::vector<std::size_t> batch_sizes;
    std::size_t total_events = 0;
};

TEST(SimulatorTest, DeliversAllGeneratedEvents) {
    CapturingSink sink;
    RetryingSender sender(sink, BackoffPolicy{});
    SimulatorConfig config;
    config.sessions = 10;
    config.batch_size = 8;

    Simulator sim(config, sender, SessionGenerator{2024});
    const auto stats = sim.run();

    EXPECT_EQ(stats.sessions_generated, 10u);
    EXPECT_GT(stats.events_generated, 0u);
    EXPECT_EQ(stats.batches_failed, 0u);
    // Everything generated is delivered exactly once.
    EXPECT_EQ(stats.events_sent, stats.events_generated);
    EXPECT_EQ(sink.total_events, stats.events_generated);
}

TEST(SimulatorTest, RespectsBatchSize) {
    CapturingSink sink;
    RetryingSender sender(sink, BackoffPolicy{});
    SimulatorConfig config;
    config.sessions = 5;
    config.batch_size = 4;

    Simulator sim(config, sender, SessionGenerator{7});
    sim.run();

    ASSERT_FALSE(sink.batch_sizes.empty());
    for (const auto size : sink.batch_sizes) {
        EXPECT_GE(size, 1u);
        EXPECT_LE(size, 4u);  // never exceeds the configured batch size
    }
}

TEST(SimulatorTest, CountsFailedBatches) {
    // A sink that always fails permanently (400 -> no retry).
    class FailingSink : public EventSink {
    public:
        SendResult send(const nlohmann::json&) override {
            return SendResult::failure(400, "bad");
        }
    } sink;

    RetryingSender sender(sink, BackoffPolicy{});
    SimulatorConfig config;
    config.sessions = 3;
    config.batch_size = 5;

    Simulator sim(config, sender, SessionGenerator{1});
    const auto stats = sim.run();

    EXPECT_EQ(stats.batches_sent, 0u);
    EXPECT_GT(stats.batches_failed, 0u);
    EXPECT_EQ(stats.events_sent, 0u);
}

}  // namespace
