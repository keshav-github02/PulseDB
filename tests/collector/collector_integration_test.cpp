#include "pulsedb/collector/collector.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "pulsedb/core/event_batch.hpp"

namespace {

using namespace std::chrono_literals;
using pulsedb::collector::Collector;
using pulsedb::collector::CollectorConfig;
using pulsedb::collector::EventQueue;

// Spins up a real collector on a loopback ephemeral port and drives it
// through an httplib client, exercising the full ingest path end-to-end.
class CollectorIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.host = "127.0.0.1";
        // Small enough that a test can exceed it without allocating megabytes;
        // every other test here posts a body far below this.
        config_.ingest.max_body_bytes = 1024;
        collector_ = std::make_unique<Collector>(config_, queue_);

        port_ = collector_->bind_to_any_port();
        ASSERT_GT(port_, 0) << "failed to bind an ephemeral port";

        server_thread_ = std::thread([this] { collector_->listen_after_bind(); });
        for (int i = 0; i < 200 && !collector_->is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_TRUE(collector_->is_running());

        client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
    }

    void TearDown() override {
        collector_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        queue_.close();
    }

    // Pop with a short poll to absorb the produce/consume race.
    std::optional<pulsedb::core::EventBatch> pop_soon() {
        for (int i = 0; i < 200; ++i) {
            if (auto b = queue_.try_pop()) {
                return b;
            }
            std::this_thread::sleep_for(5ms);
        }
        return std::nullopt;
    }

    EventQueue queue_{128};
    CollectorConfig config_{};
    std::unique_ptr<Collector> collector_;
    std::unique_ptr<httplib::Client> client_;
    std::thread server_thread_;
    int port_ = -1;
};

TEST_F(CollectorIntegrationTest, AcceptsValidBatchAndEnqueuesIt) {
    const auto res = client_->Post("/v1/events",
                                   R"([{"event_type":"video_start"}])",
                                   "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202);

    const auto batch = pop_soon();
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ(batch->size(), 1u);
}

TEST_F(CollectorIntegrationTest, RejectsMalformedJsonWith400) {
    const auto res = client_->Post("/v1/events", "{bad", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(CollectorIntegrationTest, RejectsBadSchemaWith422) {
    const auto res = client_->Post("/v1/events", "[]", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 422);
}

TEST_F(CollectorIntegrationTest, RejectsOversizedBodyWith413) {
    // httplib enforces the cap while reading, so the body is refused before it
    // is ever buffered -- and before any of our validation runs.
    const std::string oversized =
        R"([{"event_type":"video_start","session_id":")" + std::string(4096, 'x') + R"("}])";
    const auto res = client_->Post("/v1/events", oversized, "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 413);
    EXPECT_FALSE(pop_soon().has_value()) << "oversized body must not be enqueued";
}

TEST_F(CollectorIntegrationTest, RejectsDeeplyNestedBodyWith422) {
    const std::string nested = std::string(200, '[') + std::string(200, ']');
    const auto res = client_->Post("/v1/events", nested, "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 422);
}

TEST_F(CollectorIntegrationTest, HealthEndpointReturns200) {
    const auto res = client_->Get("/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(CollectorIntegrationTest, ReturnsServiceUnavailableWhenQueueIsFull) {
    // Saturate the queue directly, then confirm the collector sheds load.
    for (std::size_t i = 0; i < queue_.capacity(); ++i) {
        pulsedb::core::EventBatch b;
        b.events = nlohmann::json::array();
        b.events.push_back({{"event_type", "filler"}});
        ASSERT_TRUE(queue_.try_push(std::move(b)));
    }

    const auto res = client_->Post("/v1/events",
                                   R"([{"event_type":"pause"}])",
                                   "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
}

}  // namespace
