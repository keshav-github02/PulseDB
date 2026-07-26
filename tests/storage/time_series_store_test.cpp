#include "pulsedb/storage/time_series_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "pulsedb/storage/minute_key.hpp"

namespace {

using pulsedb::storage::MinuteKey;
using pulsedb::storage::TimeSeriesStore;

// A minimal atomic bucket to exercise the store's concurrency contract.
struct CountBucket {
    std::atomic<int> count{0};
};

TEST(TimeSeriesStoreTest, GetOrCreateReturnsSameBucketForSameMinute) {
    TimeSeriesStore<CountBucket> store;
    const MinuteKey key{2026, 7, 18, 14, 3};

    auto& a = store.get_or_create(key);
    auto& b = store.get_or_create(key);
    EXPECT_EQ(&a, &b);  // stable reference to the same bucket

    a.count.fetch_add(5);
    EXPECT_EQ(b.count.load(), 5);
    EXPECT_EQ(store.minute_count(), 1u);
}

TEST(TimeSeriesStoreTest, DistinctMinutesGetDistinctBuckets) {
    TimeSeriesStore<CountBucket> store;
    store.get_or_create({2026, 7, 18, 14, 3});
    store.get_or_create({2026, 7, 18, 14, 4});
    store.get_or_create({2026, 7, 18, 15, 3});
    EXPECT_EQ(store.minute_count(), 3u);
}

TEST(TimeSeriesStoreTest, WithBucketReportsPresence) {
    TimeSeriesStore<CountBucket> store;
    const MinuteKey present{2026, 7, 18, 14, 3};
    const MinuteKey absent{2026, 7, 18, 14, 4};
    store.get_or_create(present).count.store(9);

    int seen = -1;
    EXPECT_TRUE(store.with_bucket(present, [&](const CountBucket& b) {
        seen = b.count.load();
    }));
    EXPECT_EQ(seen, 9);
    EXPECT_FALSE(store.with_bucket(absent, [](const CountBucket&) {}));
}

TEST(TimeSeriesStoreTest, ForEachVisitsEveryBucket) {
    TimeSeriesStore<CountBucket> store;
    store.get_or_create({2026, 7, 18, 14, 3}).count.store(1);
    store.get_or_create({2026, 7, 18, 14, 4}).count.store(2);

    int total = 0;
    std::size_t visited = 0;
    store.for_each([&](const MinuteKey&, const CountBucket& b) {
        total += b.count.load();
        ++visited;
    });
    EXPECT_EQ(visited, 2u);
    EXPECT_EQ(total, 3);
}

TEST(TimeSeriesStoreTest, ConcurrentGetOrCreateAndUpdateIsConsistent) {
    TimeSeriesStore<CountBucket> store;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20'000;
    constexpr int kMinutes = 16;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store] {
            for (int i = 0; i < kPerThread; ++i) {
                MinuteKey key{2026, 7, 18, 12, i % kMinutes};
                store.get_or_create(key).count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(store.minute_count(), static_cast<std::size_t>(kMinutes));
    long long total = 0;
    store.for_each([&](const MinuteKey&, const CountBucket& b) {
        total += b.count.load();
    });
    EXPECT_EQ(total, static_cast<long long>(kThreads) * kPerThread);
}

}  // namespace
