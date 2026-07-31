#include "pulsedb/query/system_stats.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using pulsedb::query::SystemStats;

// One second of wall time, in nanoseconds, as the sampling interval.
constexpr std::uint64_t kSecond = 1'000'000'000ULL;

TEST(SystemStatsTest, ComputesPercentageFromTwoSamples) {
    // Half a second of CPU over one second of wall time = 50% of one core.
    EXPECT_DOUBLE_EQ(
        SystemStats::percent_from_samples(kSecond / 2, kSecond, 0, 0), 50.0);
}

TEST(SystemStatsTest, ReportsOverOneHundredPercentForMultipleCores) {
    // Four cores fully busy for a second. 100% means one core, so this must not
    // be clamped -- the server is multi-threaded and that is the useful signal.
    EXPECT_DOUBLE_EQ(
        SystemStats::percent_from_samples(4 * kSecond, kSecond, 0, 0), 400.0);
}

// Regression: the CPU counter is unsigned, and process_cpu_ns() returns 0 when it
// cannot read the counter at all. Subtracting the previous sample from that 0
// wrapped to ~1.8e19 instead of yielding "unknown", so a transient failure to
// read /proc/self/stat (or a failed GetProcessTimes) surfaced on the dashboard as
// an astronomical CPU percentage.
TEST(SystemStatsTest, DoesNotUnderflowWhenTheCpuCounterGoesBackwards) {
    const double percent =
        SystemStats::percent_from_samples(/*cpu_ns=*/0, /*wall_ns=*/2 * kSecond,
                                          /*last_cpu_ns=*/5 * kSecond,
                                          /*last_wall_ns=*/kSecond);
    EXPECT_DOUBLE_EQ(percent, 0.0);
}

TEST(SystemStatsTest, TreatsAZeroWidthIntervalAsUnknown) {
    // Dividing by a zero-length wall interval would be inf/NaN.
    EXPECT_DOUBLE_EQ(
        SystemStats::percent_from_samples(2 * kSecond, kSecond, kSecond, kSecond), 0.0);
    // A wall clock that went backwards is equally unusable.
    EXPECT_DOUBLE_EQ(
        SystemStats::percent_from_samples(2 * kSecond, kSecond, kSecond, 2 * kSecond), 0.0);
}

TEST(SystemStatsTest, FirstCallReportsZeroAndEstablishesABaseline) {
    SystemStats stats;
    EXPECT_DOUBLE_EQ(stats.cpu_percent(), 0.0)
        << "there is no interval to measure over yet";
}

TEST(SystemStatsTest, LiveSamplingStaysInAPlausibleRange) {
    // Not asserting a specific figure -- this pins that the live path yields a
    // finite, non-negative number rather than the wrapped value the bug produced.
    SystemStats stats;
    (void)stats.cpu_percent();
    for (int i = 0; i < 3; ++i) {
        const double percent = stats.cpu_percent();
        EXPECT_GE(percent, 0.0);
        EXPECT_LT(percent, 100.0 * 1024.0) << "implausible for any real machine";
    }
}

TEST(SystemStatsTest, ReportsNonZeroResidentMemory) {
    EXPECT_GT(SystemStats::memory_bytes(), 0u)
        << "this process is running, so it has resident pages";
}

}  // namespace
