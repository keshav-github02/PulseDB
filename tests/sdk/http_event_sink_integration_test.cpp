#include "pulsedb/sdk/http_event_sink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "pulsedb/collector/collector.hpp"
#include "pulsedb/core/event_batch.hpp"

namespace {

using namespace std::chrono_literals;
using pulsedb::collector::Collector;
using pulsedb::collector::CollectorConfig;
using pulsedb::collector::EventQueue;
using pulsedb::sdk::HttpEventSink;

// Drives the real HttpEventSink against a real in-process collector,
// proving the SDK -> HTTP -> collector -> queue path end to end.
class HttpEventSinkIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.host = "127.0.0.1";
        collector_ = std::make_unique<Collector>(config_, queue_);
        port_ = collector_->bind_to_any_port();
        ASSERT_GT(port_, 0);
        server_thread_ = std::thread([this] { collector_->listen_after_bind(); });
        for (int i = 0; i < 200 && !collector_->is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_TRUE(collector_->is_running());
    }

    void TearDown() override {
        collector_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        queue_.close();
    }

    EventQueue queue_{128};
    CollectorConfig config_{};
    std::unique_ptr<Collector> collector_;
    std::thread server_thread_;
    int port_ = -1;
};

TEST_F(HttpEventSinkIntegrationTest, DeliversBatchAndCollectorEnqueuesIt) {
    HttpEventSink sink("127.0.0.1", port_, "/v1/events");

    nlohmann::json events = nlohmann::json::array();
    events.push_back({{"event_type", "video_start"}, {"session_id", "s1"}});
    events.push_back({{"event_type", "playback_end"}, {"session_id", "s1"}});

    const auto result = sink.send(events);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.status, 202);

    std::optional<pulsedb::core::EventBatch> batch;
    for (int i = 0; i < 200 && !batch; ++i) {
        batch = queue_.try_pop();
        if (!batch) {
            std::this_thread::sleep_for(5ms);
        }
    }
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ(batch->size(), 2u);
}

TEST_F(HttpEventSinkIntegrationTest, ReportsTransportErrorWhenNothingListening) {
    // Port 1 is not served here; the client should fail cleanly, not throw.
    HttpEventSink sink("127.0.0.1", 1, "/v1/events");
    const auto result = sink.send(nlohmann::json::array({{{"event_type", "x"}}}));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.status, 0);
}

}  // namespace
