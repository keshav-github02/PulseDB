#include "pulsedb/sdk/spooling_sender.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "pulsedb/sdk/event_sink.hpp"
#include "pulsedb/sdk/spool_store.hpp"

namespace {

using pulsedb::sdk::EventSink;
using pulsedb::sdk::SendResult;
using pulsedb::sdk::SpoolingSender;
using pulsedb::sdk::SpoolStore;
namespace fs = std::filesystem;

fs::path unique_temp_dir() {
    static std::atomic<int> counter{0};
    const auto dir = fs::temp_directory_path() /
                     ("pulsedb_spooling_test_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(dir);
    return dir;
}

// A sink whose connectivity can be toggled, to model an outage and recovery.
class ToggleSink : public EventSink {
public:
    SendResult send(const nlohmann::json&) override {
        ++sends;
        return online ? SendResult::success(202)
                      : SendResult::failure(0, "offline");
    }
    bool online = true;
    int sends = 0;
};

// A sink that always answers with one fixed status, to pin down which statuses
// cause a batch to be kept versus destroyed.
class FixedStatusSink : public EventSink {
public:
    explicit FixedStatusSink(int status) : status_(status) {}
    SendResult send(const nlohmann::json&) override {
        ++sends;
        return SendResult::failure(status_, "fixed");
    }
    int sends = 0;

private:
    int status_;
};

nlohmann::json batch(const std::string& id) {
    return nlohmann::json::array({{{"event_type", "video_start"}, {"session_id", id}}});
}

class SpoolingSenderTest : public ::testing::Test {
protected:
    void SetUp() override { dir_ = unique_temp_dir(); }
    void TearDown() override { fs::remove_all(dir_); }
    fs::path dir_;
};

TEST_F(SpoolingSenderTest, PassesThroughWhenOnline) {
    ToggleSink sink;
    SpoolStore spool{dir_};
    SpoolingSender sender{sink, spool};

    const auto result = sender.send(batch("a"));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(sender.spooled_count(), 0u);
    EXPECT_EQ(spool.count(), 0u);
}

TEST_F(SpoolingSenderTest, SpoolsWhenOffline) {
    ToggleSink sink;
    sink.online = false;
    SpoolStore spool{dir_};
    SpoolingSender sender{sink, spool};

    const auto result = sender.send(batch("a"));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(sender.spooled_count(), 1u);
    EXPECT_EQ(spool.count(), 1u);
}

TEST_F(SpoolingSenderTest, DoesNotSpoolAPermanentlyRejectedBatch) {
    // 422 means the payload itself is unacceptable, so no retry can deliver it.
    // Spooling it only to have replay() discard it on the same verdict wastes a
    // capped slot.
    FixedStatusSink rejected{422};
    SpoolStore spool{dir_};
    SpoolingSender sender{rejected, spool};

    const auto result = sender.send(batch("bad"));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(sender.spooled_count(), 0u);
    EXPECT_EQ(sender.discarded_count(), 1u);
    EXPECT_EQ(spool.count(), 0u) << "nothing undeliverable should reach disk";
}

TEST_F(SpoolingSenderTest, RejectedBatchesDoNotEvictRecoverableOnes) {
    // The consequence that makes this a bug and not just waste: the spool is
    // bounded and evicts oldest-first, so undeliverable batches displace ones
    // that were merely offline. A client with a schema bug destroys its own
    // recoverable backlog.
    SpoolStore spool{dir_, /*max_batches=*/2};

    ToggleSink offline;
    offline.online = false;
    SpoolingSender offline_sender{offline, spool};
    EXPECT_FALSE(offline_sender.send(batch("keep-me")).ok);
    ASSERT_EQ(spool.count(), 1u);

    // Now push more permanently-rejected batches than the spool can hold.
    FixedStatusSink rejected{422};
    SpoolingSender rejecting_sender{rejected, spool};
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(rejecting_sender.send(batch("junk")).ok);
    }

    ASSERT_EQ(spool.count(), 1u) << "the spool should still hold only the good batch";

    // The survivor must be the recoverable one: bring the sink back and confirm
    // it replays.
    offline.online = true;
    const auto replayed = offline_sender.replay();
    EXPECT_EQ(replayed.replayed, 1u);
    EXPECT_EQ(replayed.discarded, 0u) << "the offline batch was evicted by junk";
    EXPECT_EQ(spool.count(), 0u);
}

TEST_F(SpoolingSenderTest, ReplayDrainsSpoolAfterRecovery) {
    ToggleSink sink;
    sink.online = false;
    SpoolStore spool{dir_};
    SpoolingSender sender{sink, spool};

    EXPECT_FALSE(sender.send(batch("a")).ok);
    EXPECT_FALSE(sender.send(batch("b")).ok);
    ASSERT_EQ(spool.count(), 2u);

    sink.online = true;  // collector comes back
    const auto replayed = sender.replay();
    EXPECT_EQ(replayed.replayed, 2u);
    EXPECT_EQ(replayed.failed, 0u);
    EXPECT_EQ(spool.count(), 0u);
}

TEST_F(SpoolingSenderTest, ReplayStopsWhileStillOffline) {
    ToggleSink sink;
    sink.online = false;
    SpoolStore spool{dir_};
    SpoolingSender sender{sink, spool};

    EXPECT_FALSE(sender.send(batch("a")).ok);
    EXPECT_FALSE(sender.send(batch("b")).ok);

    const auto replayed = sender.replay();  // still offline
    EXPECT_EQ(replayed.replayed, 0u);
    EXPECT_GE(replayed.failed, 1u);
    EXPECT_EQ(spool.count(), 2u);  // nothing lost
}

// --- Poison-pill handling (M5) ----------------------------------------------

// A sink that permanently rejects one specific batch and accepts everything
// else, modelling a collector answering 422 for one malformed payload.
class RejectOneSink : public EventSink {
public:
    explicit RejectOneSink(std::string poison) : poison_(std::move(poison)) {}

    SendResult send(const nlohmann::json& events) override {
        const bool is_poison = !events.empty() && events[0].contains("session_id") &&
                               events[0]["session_id"] == poison_;
        if (is_poison) {
            ++rejections;
            return SendResult::failure(422, "unprocessable");
        }
        delivered.push_back(events[0]["session_id"].get<std::string>());
        return SendResult::success(202);
    }

    std::vector<std::string> delivered;
    int rejections = 0;

private:
    std::string poison_;
};

// The regression: replay() used to break on *any* failure. A batch the collector
// permanently rejects is never retryable and was never removed, so it wedged the
// spool forever -- nothing behind it could ever be delivered.
TEST_F(SpoolingSenderTest, ReplayDiscardsPermanentlyRejectedBatchAndContinues) {
    ToggleSink offline;
    offline.online = false;
    SpoolStore spool{dir_};
    {
        SpoolingSender spooler{offline, spool};
        EXPECT_FALSE(spooler.send(batch("good-1")).ok);
        EXPECT_FALSE(spooler.send(batch("poison")).ok);
        EXPECT_FALSE(spooler.send(batch("good-2")).ok);
    }
    ASSERT_EQ(spool.count(), 3u);

    RejectOneSink sink{"poison"};
    SpoolingSender sender{sink, spool};
    const auto replayed = sender.replay();

    EXPECT_EQ(replayed.replayed, 2u);
    EXPECT_EQ(replayed.discarded, 1u);
    EXPECT_EQ(replayed.failed, 0u);
    EXPECT_EQ(sender.discarded_count(), 1u);
    EXPECT_EQ(spool.count(), 0u) << "the poison batch must not wedge the spool";
    EXPECT_EQ(sink.rejections, 1) << "a permanent rejection must not be retried";
    // Crucially, the batch queued *behind* the poison one got through.
    ASSERT_EQ(sink.delivered.size(), 2u);
    EXPECT_EQ(sink.delivered[0], "good-1");
    EXPECT_EQ(sink.delivered[1], "good-2");
}

// Regression (CB-8): 429 was misfiled as a permanent failure, so replay()
// discarded the batch. Rate limiting is the collector asking for a slower
// client, not a verdict that the payload is unacceptable -- deleting the data in
// response is silent loss precisely under load.
TEST_F(SpoolingSenderTest, ReplayKeepsARateLimitedBatchInsteadOfDiscardingIt) {
    ToggleSink offline;
    offline.online = false;
    SpoolStore spool{dir_};
    {
        SpoolingSender spooler{offline, spool};
        EXPECT_FALSE(spooler.send(batch("a")).ok);
        EXPECT_FALSE(spooler.send(batch("b")).ok);
    }
    ASSERT_EQ(spool.count(), 2u);

    FixedStatusSink rate_limited{429};
    SpoolingSender sender{rate_limited, spool};
    const auto replayed = sender.replay();

    EXPECT_EQ(replayed.replayed, 0u);
    EXPECT_EQ(replayed.discarded, 0u) << "a 429 must not destroy the batch";
    EXPECT_GE(replayed.failed, 1u);
    EXPECT_EQ(spool.count(), 2u) << "both batches must still be on disk for a later retry";
    EXPECT_EQ(rate_limited.sends, 1) << "replay stops at the first transient failure";

    // And once the limit lifts, the retained batches drain in order.
    ToggleSink recovered;
    SpoolingSender resumed{recovered, spool};
    const auto after = resumed.replay();
    EXPECT_EQ(after.replayed, 2u);
    EXPECT_EQ(spool.count(), 0u);
}

TEST_F(SpoolingSenderTest, ReplayStillDiscardsAGenuinelyPermanentRejection) {
    ToggleSink offline;
    offline.online = false;
    SpoolStore spool{dir_};
    {
        SpoolingSender spooler{offline, spool};
        EXPECT_FALSE(spooler.send(batch("a")).ok);
    }
    ASSERT_EQ(spool.count(), 1u);

    FixedStatusSink unprocessable{422};
    SpoolingSender sender{unprocessable, spool};
    const auto replayed = sender.replay();

    EXPECT_EQ(replayed.discarded, 1u);
    EXPECT_EQ(spool.count(), 0u) << "retrying a 422 forever would wedge the spool";
}

TEST_F(SpoolingSenderTest, ReplayDropsCorruptSpoolFileAndContinues) {
    ToggleSink offline;
    offline.online = false;
    SpoolStore spool{dir_};
    {
        SpoolingSender spooler{offline, spool};
        EXPECT_FALSE(spooler.send(batch("a")).ok);
        EXPECT_FALSE(spooler.send(batch("b")).ok);
    }
    ASSERT_EQ(spool.count(), 2u);

    // Corrupt the first spooled file in place.
    const auto files = spool.list();
    ASSERT_FALSE(files.empty());
    {
        std::ofstream out(files.front(), std::ios::binary | std::ios::trunc);
        out << "{ not json";
    }

    ToggleSink sink;
    SpoolingSender sender{sink, spool};
    const auto replayed = sender.replay();
    EXPECT_EQ(replayed.discarded, 1u);
    EXPECT_EQ(replayed.replayed, 1u);
    EXPECT_EQ(spool.count(), 0u);
}

}  // namespace
