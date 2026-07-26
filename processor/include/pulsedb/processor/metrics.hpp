#pragma once

#include <cstdint>

namespace pulsedb::processor {

/// A point-in-time copy of the aggregate metrics derived from events.
///
/// Sums and sample counts are kept separately so averages can be computed
/// without storing per-event history.
struct MetricsSnapshot {
    std::uint64_t total_events = 0;           ///< Every processed event.
    std::uint64_t total_views = 0;            ///< video_start count.
    std::uint64_t startup_samples = 0;        ///< startup_complete count.
    std::uint64_t startup_time_ms_sum = 0;    ///< Sum of startup times.
    std::uint64_t buffer_count = 0;           ///< buffer_start count (stalls begun).
    std::uint64_t buffer_samples = 0;         ///< buffer_end count carrying a duration.
    std::uint64_t buffer_duration_ms_sum = 0; ///< Sum of stall durations.
    std::uint64_t error_count = 0;            ///< drm_error count.
    std::uint64_t watch_time_ms_sum = 0;      ///< Sum of watch times.
    std::uint64_t bitrate_samples = 0;        ///< bitrate_change count.
    std::uint64_t bitrate_kbps_sum = 0;       ///< Sum of observed bitrates.

    /// Mean startup time (ms), or 0 when there are no samples.
    double avg_startup_ms() const {
        return startup_samples ? static_cast<double>(startup_time_ms_sum) /
                                     static_cast<double>(startup_samples)
                               : 0.0;
    }

    /// Mean observed bitrate (kbps), or 0 when there are no samples.
    double avg_bitrate_kbps() const {
        return bitrate_samples ? static_cast<double>(bitrate_kbps_sum) /
                                     static_cast<double>(bitrate_samples)
                               : 0.0;
    }

    /// Mean stall duration (ms), or 0 when there are no samples.
    ///
    /// Divides by buffer_samples, NOT buffer_count: the two count different
    /// populations. buffer_count counts `buffer_start` events (stalls begun);
    /// buffer_duration_ms_sum accumulates `buffer_end` payloads (stalls
    /// finished). They differ whenever a stall crosses a bucket boundary or a
    /// session is abandoned mid-stall -- so dividing the sum by buffer_count
    /// silently under-reports, and within a single minute bucket could report 0
    /// for a bucket holding real stall time.
    double avg_buffer_ms() const {
        return buffer_samples ? static_cast<double>(buffer_duration_ms_sum) /
                                    static_cast<double>(buffer_samples)
                              : 0.0;
    }

    /// Rebuffers per view -- a common quality-of-experience indicator.
    double buffer_ratio() const {
        return total_views ? static_cast<double>(buffer_count) /
                                 static_cast<double>(total_views)
                           : 0.0;
    }
};

}  // namespace pulsedb::processor
