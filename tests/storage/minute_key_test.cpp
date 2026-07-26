#include "pulsedb/storage/minute_key.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using pulsedb::storage::MinuteKey;

TEST(MinuteKeyTest, DecomposesKnownTimestamp) {
    // 1700000000000 ms == 2023-11-14T22:13:20 UTC.
    const auto key = MinuteKey::from_timestamp_ms(1700000000000);
    EXPECT_EQ(key.year, 2023);
    EXPECT_EQ(key.month, 11);
    EXPECT_EQ(key.day, 14);
    EXPECT_EQ(key.hour, 22);
    EXPECT_EQ(key.minute, 13);
}

TEST(MinuteKeyTest, DecomposesUnixEpoch) {
    const auto key = MinuteKey::from_timestamp_ms(0);
    EXPECT_EQ(key.year, 1970);
    EXPECT_EQ(key.month, 1);
    EXPECT_EQ(key.day, 1);
    EXPECT_EQ(key.hour, 0);
    EXPECT_EQ(key.minute, 0);
}

TEST(MinuteKeyTest, TimestampsInSameMinuteShareKey) {
    const auto base = 1700000000000;               // ...T22:13:20
    const auto same = MinuteKey::from_timestamp_ms(base + 30'000);   // +30s -> :13:50
    const auto next = MinuteKey::from_timestamp_ms(base + 60'000);   // +60s -> :14:20
    EXPECT_EQ(MinuteKey::from_timestamp_ms(base), same);
    EXPECT_NE(MinuteKey::from_timestamp_ms(base), next);
}

TEST(MinuteKeyTest, FormatsIso) {
    const MinuteKey key{2026, 7, 18, 14, 3};
    EXPECT_EQ(key.to_iso(), "2026-07-18T14:03");
}

TEST(MinuteKeyTest, ValidatesTheRepresentableDomain) {
    using pulsedb::storage::is_valid_timestamp_ms;
    using pulsedb::storage::kMaxTimestampMs;
    using pulsedb::storage::kMinTimestampMs;

    EXPECT_TRUE(is_valid_timestamp_ms(kMinTimestampMs));
    EXPECT_TRUE(is_valid_timestamp_ms(kMaxTimestampMs));
    EXPECT_TRUE(is_valid_timestamp_ms(1700000000000));
    EXPECT_TRUE(is_valid_timestamp_ms(9999999999999));  // year 2286, a wide query bound

    EXPECT_FALSE(is_valid_timestamp_ms(-1));
    EXPECT_FALSE(is_valid_timestamp_ms(kMaxTimestampMs + 1));
    EXPECT_FALSE(is_valid_timestamp_ms(std::numeric_limits<std::int64_t>::max()));
    EXPECT_FALSE(is_valid_timestamp_ms(std::numeric_limits<std::int64_t>::min()));
}

// Documents the hazard the domain bound guards: from_timestamp_ms cannot fail,
// so out-of-domain input wraps silently instead of erroring.
TEST(MinuteKeyTest, OutOfDomainTimestampsWrapAndMustBeRejectedByCallers) {
    const auto wrapped =
        MinuteKey::from_timestamp_ms(std::numeric_limits<std::int64_t>::max());
    EXPECT_LT(wrapped.year, 0) << "INT64_MAX is expected to wrap to a negative year";
    EXPECT_FALSE(pulsedb::storage::is_valid_timestamp_ms(
        std::numeric_limits<std::int64_t>::max()));
}

TEST(MinuteKeyTest, RendersWellFormedIsoAcrossTheWholeDomain) {
    for (const std::int64_t ms : {pulsedb::storage::kMinTimestampMs,
                                  std::int64_t{1700000000000},
                                  pulsedb::storage::kMaxTimestampMs}) {
        const auto iso = MinuteKey::from_timestamp_ms(ms).to_iso();
        EXPECT_EQ(iso.size(), 16u) << iso;  // YYYY-MM-DDTHH:MM
        EXPECT_NE(iso.front(), '-') << iso;
    }
}

TEST(MinuteKeyTest, OrdersChronologically) {
    EXPECT_LT((MinuteKey{2026, 7, 18, 14, 3}), (MinuteKey{2026, 7, 18, 14, 4}));
    EXPECT_LT((MinuteKey{2026, 7, 18, 23, 59}), (MinuteKey{2026, 7, 19, 0, 0}));
    EXPECT_LT((MinuteKey{2025, 12, 31, 23, 59}), (MinuteKey{2026, 1, 1, 0, 0}));
}

}  // namespace
