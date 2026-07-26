#include "pulsedb/workers/worker_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "pulsedb/core/event.hpp"
#include "pulsedb/core/event_batch.hpp"
#include "pulsedb/processor/event_processor.hpp"

namespace {

using pulsedb::core::EventBatch;
using pulsedb::processor::EventProcessor;
using pulsedb::workers::EventQueue;
using pulsedb::workers::WorkerPool;

// Thread-safe processor that just counts the events it receives.
class CountingProcessor : public EventProcessor {
public:
    void process(const pulsedb::core::Event&) override {
        count.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<std::uint64_t> count{0};
};

EventBatch make_batch(std::size_t n) {
    EventBatch batch;
    batch.events = nlohmann::json::array();
    for (std::size_t i = 0; i < n; ++i) {
        batch.events.push_back({{"event_type", "video_start"},
                                {"session_id", "s" + std::to_string(i)}});
    }
    return batch;
}

TEST(WorkerPoolTest, ProcessesAllEnqueuedEvents) {
    EventQueue queue{1024};
    CountingProcessor processor;

    constexpr std::size_t kBatches = 40;
    constexpr std::size_t kPerBatch = 25;
    for (std::size_t i = 0; i < kBatches; ++i) {
        ASSERT_TRUE(queue.try_push(make_batch(kPerBatch)));
    }

    WorkerPool pool{queue, processor, 4};
    pool.start();
    pool.stop();  // closes queue, drains everything, joins

    EXPECT_EQ(processor.count.load(), kBatches * kPerBatch);

    const auto stats = pool.stats();
    EXPECT_EQ(stats.batches_processed, kBatches);
    EXPECT_EQ(stats.events_processed, kBatches * kPerBatch);
    EXPECT_EQ(stats.parse_failures, 0u);
}

TEST(WorkerPoolTest, CountsUnparseableEventsAsFailures) {
    EventQueue queue{16};
    CountingProcessor processor;

    EventBatch batch;
    batch.events = nlohmann::json::array();
    batch.events.push_back({{"event_type", "video_start"}});  // ok
    batch.events.push_back({{"no_type", "x"}});               // parse failure
    batch.events.push_back(42);                               // parse failure
    ASSERT_TRUE(queue.try_push(std::move(batch)));

    WorkerPool pool{queue, processor, 2};
    pool.start();
    pool.stop();

    const auto stats = pool.stats();
    EXPECT_EQ(stats.events_processed, 1u);
    EXPECT_EQ(stats.parse_failures, 2u);
    EXPECT_EQ(processor.count.load(), 1u);
}

TEST(WorkerPoolTest, HandlesConcurrentProducerWithManyWorkers) {
    EventQueue queue{256};
    CountingProcessor processor;

    WorkerPool pool{queue, processor, 8};
    pool.start();

    // Produce while the workers are already draining.
    constexpr std::size_t kBatches = 500;
    constexpr std::size_t kPerBatch = 10;
    for (std::size_t i = 0; i < kBatches; ++i) {
        ASSERT_TRUE(queue.push(make_batch(kPerBatch)));
    }

    pool.stop();  // drains the remainder and joins

    EXPECT_EQ(processor.count.load(), kBatches * kPerBatch);
    EXPECT_EQ(pool.stats().events_processed, kBatches * kPerBatch);
}

TEST(WorkerPoolTest, DefaultWorkerCountIsAtLeastOne) {
    EXPECT_GE(WorkerPool::default_worker_count(), 1u);
}

// A processor that throws on some events, modelling business logic (or JSON
// access) failing on pathological input.
class ThrowingProcessor : public pulsedb::processor::EventProcessor {
public:
    void process(const pulsedb::core::Event& event) override {
        if (event.session_id == "boom") {
            throw std::runtime_error("processing failed");
        }
        ++handled;
    }
    std::atomic<int> handled{0};
};

// Regression: process() is a virtual call into arbitrary code, and an escaping
// exception used to propagate out of the thread function and terminate the
// process -- so a single bad event took down the server and everything queued.
TEST(WorkerPoolTest, SurvivesAThrowingProcessorAndKeepsDraining) {
    EventQueue queue{64};
    ThrowingProcessor processor;
    WorkerPool pool{queue, processor, 2};
    pool.start();

    constexpr int kGood = 40;
    for (int i = 0; i < kGood; ++i) {
        pulsedb::core::EventBatch batch;
        batch.events = nlohmann::json::array();
        batch.events.push_back({{"event_type", "video_start"}, {"session_id", "ok"}});
        batch.events.push_back({{"event_type", "video_start"}, {"session_id", "boom"}});
        ASSERT_TRUE(queue.push(std::move(batch)));
    }

    pool.stop();  // drains and joins; must not have terminated

    const auto stats = pool.stats();
    EXPECT_EQ(processor.handled.load(), kGood) << "good events must still be processed";
    EXPECT_EQ(stats.process_errors, static_cast<std::uint64_t>(kGood))
        << "each throwing event must be counted, not fatal";
    EXPECT_EQ(stats.events_processed, static_cast<std::uint64_t>(kGood));
    EXPECT_EQ(stats.batches_processed, static_cast<std::uint64_t>(kGood));
}

}  // namespace
