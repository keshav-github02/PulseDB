#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <tuple>

namespace pulsedb::storage {

/// Inclusive bounds of the epoch-millisecond domain PulseDB will decompose
/// into a MinuteKey: 1970-01-01T00:00:00Z .. 9999-12-31T23:59:59.999Z.
///
/// These exist because from_timestamp_ms() cannot fail. Fed a value near the
/// int64 limits it does not throw or saturate -- the day count overflows
/// inside chrono and silently wraps, so INT64_MAX decomposes to the year -6911
/// and to_iso() then emits a malformed ISO-8601 string with a negative year.
/// Callers taking a timestamp from an untrusted source must reject it with
/// is_valid_timestamp_ms() first.
///
/// The upper bound is the last instant to_iso()'s "{:04}" year field can render
/// as well-formed ISO-8601; the lower bound excludes pre-epoch times, which are
/// meaningless for telemetry. This is deliberately a *representability* bound
/// and nothing more -- it is wide enough for any legitimate range query. What
/// counts as a plausible event time is a much tighter, separate question,
/// answered by IngestOptions' freshness window on the write path.
inline constexpr std::int64_t kMinTimestampMs = 0;
inline constexpr std::int64_t kMaxTimestampMs = 253'402'300'799'999;

/// True if @p ms lies within the representable domain above.
constexpr bool is_valid_timestamp_ms(std::int64_t ms) {
    return ms >= kMinTimestampMs && ms <= kMaxTimestampMs;
}

/// Identifies a one-minute time bucket in UTC calendar terms.
///
/// This is the granularity at which PulseDB aggregates metrics and the
/// address into the Year -> Month -> Day -> Hour -> Minute store.
struct MinuteKey {
    int year = 1970;
    int month = 1;   // 1-12
    int day = 1;     // 1-31
    int hour = 0;    // 0-23
    int minute = 0;  // 0-59

    /// Decompose an epoch-milliseconds timestamp into its UTC minute bucket.
    static MinuteKey from_timestamp_ms(std::int64_t ms) {
        using namespace std::chrono;
        const sys_time<milliseconds> tp{milliseconds{ms}};
        const auto day_point = floor<days>(tp);
        const year_month_day ymd{day_point};
        const hh_mm_ss<milliseconds> tod{tp - day_point};
        return MinuteKey{
            static_cast<int>(ymd.year()),
            static_cast<int>(static_cast<unsigned>(ymd.month())),
            static_cast<int>(static_cast<unsigned>(ymd.day())),
            static_cast<int>(tod.hours().count()),
            static_cast<int>(tod.minutes().count()),
        };
    }

    /// ISO-8601-ish label, e.g. "2026-07-18T14:32".
    std::string to_iso() const {
        return std::format("{:04}-{:02}-{:02}T{:02}:{:02}", year, month, day, hour, minute);
    }

    friend bool operator==(const MinuteKey& a, const MinuteKey& b) {
        return std::tie(a.year, a.month, a.day, a.hour, a.minute) ==
               std::tie(b.year, b.month, b.day, b.hour, b.minute);
    }

    friend bool operator<(const MinuteKey& a, const MinuteKey& b) {
        return std::tie(a.year, a.month, a.day, a.hour, a.minute) <
               std::tie(b.year, b.month, b.day, b.hour, b.minute);
    }
};

}  // namespace pulsedb::storage
