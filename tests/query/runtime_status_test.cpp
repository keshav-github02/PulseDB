#include "pulsedb/query/runtime_status.hpp"

#include <gtest/gtest.h>

#include "pulsedb/query/system_stats.hpp"

namespace {

using pulsedb::query::RuntimeStatus;
using pulsedb::query::RuntimeStatusSnapshot;
using pulsedb::query::SystemStats;

TEST(RuntimeStatusTest, GetReturnsLastSet) {
    RuntimeStatus status;
    // Defaults before any set().
    EXPECT_EQ(status.get().total_events, 0u);

    RuntimeStatusSnapshot snap;
    snap.queue_depth = 7;
    snap.events_per_sec = 123.5;
    snap.active_sessions = 4;
    snap.total_events = 1000;
    snap.error_rate = 0.01;
    status.set(snap);

    const auto got = status.get();
    EXPECT_EQ(got.queue_depth, 7u);
    EXPECT_DOUBLE_EQ(got.events_per_sec, 123.5);
    EXPECT_EQ(got.active_sessions, 4);
    EXPECT_EQ(got.total_events, 1000u);
    EXPECT_DOUBLE_EQ(got.error_rate, 0.01);
}

TEST(SystemStatsTest, MemoryIsPositive) {
    // This process is running, so its resident memory must be non-zero.
    EXPECT_GT(SystemStats::memory_bytes(), 0u);
}

TEST(SystemStatsTest, FirstCpuSampleIsZeroThenNonNegative) {
    SystemStats stats;
    EXPECT_DOUBLE_EQ(stats.cpu_percent(), 0.0);  // no baseline yet
    // Do a little work so a second sample has something to measure.
    volatile double acc = 0.0;
    for (int i = 0; i < 1'000'000; ++i) {
        acc += static_cast<double>(i) * 0.5;
    }
    (void)acc;
    EXPECT_GE(stats.cpu_percent(), 0.0);
}

}  // namespace
