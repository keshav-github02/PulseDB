#include "pulsedb/query/query_server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "pulsedb/aggregation/aggregation_engine.hpp"
#include "pulsedb/core/event.hpp"

namespace {

using namespace std::chrono_literals;
using pulsedb::aggregation::AggregationEngine;
using pulsedb::core::Event;
using pulsedb::core::EventType;
using pulsedb::query::QueryServer;

constexpr std::int64_t kMinuteA = 1784556180000;

Event view(std::int64_t ts, const std::string& player, const std::string& device) {
    Event e;
    e.type = EventType::kVideoStart;
    e.timestamp_ms = ts;
    e.player = player;
    e.device = device;
    return e;
}

// Serves a real QueryServer over loopback and drives it with an HTTP client.
class QueryServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.process(view(kMinuteA, "ExoPlayer", "Android"));
        engine_.process(view(kMinuteA + 60'000, "AVPlayer", "iOS"));

        QueryServer::Config cfg;
        cfg.host = "127.0.0.1";
        server_ = std::make_unique<QueryServer>(cfg, engine_);
        port_ = server_->bind_to_any_port();
        ASSERT_GT(port_, 0);
        thread_ = std::thread([this] { server_->listen_after_bind(); });
        for (int i = 0; i < 200 && !server_->is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_TRUE(server_->is_running());
        client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    AggregationEngine engine_;
    std::unique_ptr<QueryServer> server_;
    std::unique_ptr<httplib::Client> client_;
    std::thread thread_;
    int port_ = -1;
};

TEST_F(QueryServerTest, MetricsEndpointReturnsTotals) {
    const auto res = client_->Get("/metrics");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->get_header_value("Access-Control-Allow-Origin"), "*");

    const auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["totals"]["total_views"], 2);
    EXPECT_EQ(j["minutes_tracked"], 2);
}

TEST_F(QueryServerTest, LiveEndpointReturnsMinutes) {
    const auto res = client_->Get("/metrics/live?minutes=5");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    const auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["minutes"].size(), 2u);
}

TEST_F(QueryServerTest, PlayerAndDeviceEndpoints) {
    const auto players = client_->Get("/metrics/player");
    ASSERT_TRUE(players);
    EXPECT_EQ(players->status, 200);
    EXPECT_EQ(nlohmann::json::parse(players->body)["players"].size(), 2u);

    const auto devices = client_->Get("/metrics/device");
    ASSERT_TRUE(devices);
    EXPECT_EQ(nlohmann::json::parse(devices->body)["devices"].size(), 2u);
}

TEST_F(QueryServerTest, RangeRequiresBounds) {
    const auto bad = client_->Get("/metrics/range");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);

    // A window around the two seeded buckets, narrow enough to sit inside
    // max_response_minutes (this used to span the entire epoch).
    const auto ok = client_->Get("/metrics/range?from=" +
                                 std::to_string(kMinuteA - 60'000) + "&to=" +
                                 std::to_string(kMinuteA + 120'000));
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(nlohmann::json::parse(ok->body)["minutes"].size(), 2u);
}

// --- Response-size bounds ---------------------------------------------------
// Both read endpoints accepted an unbounded window, so a single ~60-byte
// unauthenticated GET could ask the server to serialise every bucket it holds
// -- while holding the store mutex that the ingest path also needs.

TEST_F(QueryServerTest, LiveClampsAnAbsurdMinutesParameter) {
    // The request is answered (it is still "the most recent N"), just capped.
    const auto res = client_->Get("/metrics/live?minutes=999999999");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    const auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["minutes"].size(), 2u) << "only two buckets exist to return";
}

TEST_F(QueryServerTest, LiveNeverReturnsMoreThanTheConfiguredCap) {
    // Seed more buckets than the cap allows, then confirm the reply is bounded
    // by the cap and not by what the client asked for.
    QueryServer::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.max_response_minutes = 5;

    AggregationEngine engine;
    for (int i = 0; i < 40; ++i) {
        engine.process(view(kMinuteA + static_cast<std::int64_t>(i) * 60'000, "P", "D"));
    }

    QueryServer server{cfg, engine};
    const int port = server.bind_to_any_port();
    ASSERT_GT(port, 0);
    std::thread t([&server] { server.listen_after_bind(); });
    for (int i = 0; i < 200 && !server.is_running(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(server.is_running());

    httplib::Client client("127.0.0.1", port);
    const auto res = client.Get("/metrics/live?minutes=999999999");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(nlohmann::json::parse(res->body)["minutes"].size(), 5u);

    server.stop();
    if (t.joinable()) {
        t.join();
    }
}

TEST_F(QueryServerTest, RangeRejectsAWindowOverTheCap) {
    // Rejected rather than truncated: a partial slice would read as "these are
    // all the buckets in that window".
    const auto res = client_->Get("/metrics/range?from=0&to=253402300799999");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    const auto body = nlohmann::json::parse(res->body);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_NE(body["error"].get<std::string>().find("narrower"), std::string::npos)
        << body["error"];
}

}  // namespace
