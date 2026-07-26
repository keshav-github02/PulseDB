#include "pulsedb/queue/bounded_blocking_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using pulsedb::queue::BoundedBlockingQueue;
using namespace std::chrono_literals;

TEST(BoundedBlockingQueueTest, ThrowsOnZeroCapacity) {
    EXPECT_THROW((BoundedBlockingQueue<int>{0}), std::invalid_argument);
}

TEST(BoundedBlockingQueueTest, PreservesFifoOrder) {
    BoundedBlockingQueue<int> queue{8};
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(queue.push(i));
    }
    EXPECT_EQ(queue.size(), 5u);

    for (int i = 0; i < 5; ++i) {
        auto value = queue.pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
    EXPECT_TRUE(queue.empty());
}

TEST(BoundedBlockingQueueTest, TryPushFailsWhenFull) {
    BoundedBlockingQueue<int> queue{2};
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3));
    EXPECT_EQ(queue.size(), 2u);
}

TEST(BoundedBlockingQueueTest, TryPopReturnsNulloptWhenEmpty) {
    BoundedBlockingQueue<int> queue{2};
    EXPECT_EQ(queue.try_pop(), std::nullopt);

    ASSERT_TRUE(queue.push(9));
    EXPECT_EQ(queue.try_pop(), std::optional{9});
}

TEST(BoundedBlockingQueueTest, PopBlocksUntilItemIsPushed) {
    BoundedBlockingQueue<int> queue{4};

    auto consumer = std::async(std::launch::async, [&queue] { return queue.pop(); });
    EXPECT_EQ(consumer.wait_for(50ms), std::future_status::timeout);

    ASSERT_TRUE(queue.push(42));
    auto value = consumer.get();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);
}

TEST(BoundedBlockingQueueTest, PushBlocksWhenFullUntilSpaceIsFreed) {
    BoundedBlockingQueue<int> queue{1};
    ASSERT_TRUE(queue.push(1));

    auto producer = std::async(std::launch::async, [&queue] { return queue.push(2); });
    EXPECT_EQ(producer.wait_for(50ms), std::future_status::timeout);

    EXPECT_EQ(queue.pop(), std::optional{1});
    EXPECT_TRUE(producer.get());
    EXPECT_EQ(queue.pop(), std::optional{2});
}

TEST(BoundedBlockingQueueTest, CloseWakesBlockedConsumers) {
    BoundedBlockingQueue<int> queue{4};

    auto consumer = std::async(std::launch::async, [&queue] { return queue.pop(); });
    EXPECT_EQ(consumer.wait_for(50ms), std::future_status::timeout);

    queue.close();
    EXPECT_EQ(consumer.get(), std::nullopt);
}

TEST(BoundedBlockingQueueTest, CloseWakesBlockedProducers) {
    BoundedBlockingQueue<int> queue{1};
    ASSERT_TRUE(queue.push(1));

    auto producer = std::async(std::launch::async, [&queue] { return queue.push(2); });
    EXPECT_EQ(producer.wait_for(50ms), std::future_status::timeout);

    queue.close();
    EXPECT_FALSE(producer.get());
}

TEST(BoundedBlockingQueueTest, PopDrainsRemainingItemsAfterClose) {
    BoundedBlockingQueue<int> queue{4};
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));

    queue.close();
    EXPECT_TRUE(queue.closed());

    EXPECT_FALSE(queue.push(3));
    EXPECT_FALSE(queue.try_push(3));

    EXPECT_EQ(queue.pop(), std::optional{1});
    EXPECT_EQ(queue.pop(), std::optional{2});
    EXPECT_EQ(queue.pop(), std::nullopt);
}

TEST(BoundedBlockingQueueTest, SupportsMoveOnlyTypes) {
    BoundedBlockingQueue<std::unique_ptr<int>> queue{2};
    ASSERT_TRUE(queue.push(std::make_unique<int>(7)));

    auto value = queue.pop();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(**value, 7);
}

TEST(BoundedBlockingQueueTest, HandlesManyProducersAndConsumers) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kItemsPerProducer = 10'000;
    constexpr long long kTotalItems =
        static_cast<long long>(kProducers) * kItemsPerProducer;

    BoundedBlockingQueue<int> queue{128};
    std::atomic<long long> consumed_count{0};
    std::atomic<long long> consumed_sum{0};

    std::vector<std::jthread> consumers;
    consumers.reserve(kConsumers);
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&queue, &consumed_count, &consumed_sum] {
            while (auto item = queue.pop()) {
                consumed_count.fetch_add(1, std::memory_order_relaxed);
                consumed_sum.fetch_add(*item, std::memory_order_relaxed);
            }
        });
    }

    {
        std::vector<std::jthread> producers;
        producers.reserve(kProducers);
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&queue, p] {
                for (int i = 0; i < kItemsPerProducer; ++i) {
                    EXPECT_TRUE(queue.push(p * kItemsPerProducer + i));
                }
            });
        }
    }  // producers joined here

    queue.close();
    consumers.clear();  // consumers joined here

    EXPECT_EQ(consumed_count.load(), kTotalItems);
    // Values form 0..N-1 exactly once, so their sum is N*(N-1)/2.
    EXPECT_EQ(consumed_sum.load(), kTotalItems * (kTotalItems - 1) / 2);
}

}  // namespace
