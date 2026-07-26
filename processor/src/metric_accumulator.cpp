#include "pulsedb/processor/metric_accumulator.hpp"

#include <optional>

namespace pulsedb::processor {
namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

void add(std::atomic<std::uint64_t>& counter, std::uint64_t delta) {
    counter.fetch_add(delta, kRelaxed);
}

/// A sample payload that is present and non-negative, as an unsigned value.
///
/// Returns nullopt for both "absent" and "negative", so a negative payload is
/// treated exactly like a missing one -- it contributes to neither the sum nor
/// the sample count, leaving the average derived only from valid samples. The
/// alternative, static_cast'ing a negative int to uint64_t, wraps to ~1.8e19 and
/// destroys the metric permanently.
template <typename T>
std::optional<std::uint64_t> valid_sample(const std::optional<T>& payload) {
    if (!payload || *payload < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*payload);
}

}  // namespace

void MetricAccumulator::add(const core::Event& event) {
    processor::add(total_events_, 1);

    switch (event.type) {
        case core::EventType::kVideoStart:
            processor::add(total_views_, 1);
            break;
        case core::EventType::kStartupComplete:
            if (const auto sample = valid_sample(event.startup_time_ms)) {
                processor::add(startup_samples_, 1);
                processor::add(startup_time_ms_sum_, *sample);
            }
            break;
        case core::EventType::kBufferStart:
            processor::add(buffer_count_, 1);
            break;
        case core::EventType::kBufferEnd:
            // Counted as its own sample: buffer_count tracks stalls *begun* and
            // this tracks stalls *finished*, and the two populations differ
            // (boundary-crossing stalls, abandoned sessions). avg_buffer_ms()
            // divides by this counter, not by buffer_count.
            if (const auto sample = valid_sample(event.buffer_duration_ms)) {
                processor::add(buffer_samples_, 1);
                processor::add(buffer_duration_ms_sum_, *sample);
            }
            break;
        case core::EventType::kBitrateChange:
            if (const auto sample = valid_sample(event.bitrate_kbps)) {
                processor::add(bitrate_samples_, 1);
                processor::add(bitrate_kbps_sum_, *sample);
            }
            break;
        case core::EventType::kDrmError:
            processor::add(error_count_, 1);
            break;
        case core::EventType::kPlaybackEnd:
            if (const auto sample = valid_sample(event.watch_time_ms)) {
                processor::add(watch_time_ms_sum_, *sample);
            }
            break;
        case core::EventType::kPause:
        case core::EventType::kResume:
        case core::EventType::kSeek:
            // Counted in total_events only; no dedicated metric yet.
            break;
    }
}

void MetricAccumulator::add_snapshot(const MetricsSnapshot& s) {
    processor::add(total_events_, s.total_events);
    processor::add(total_views_, s.total_views);
    processor::add(startup_samples_, s.startup_samples);
    processor::add(startup_time_ms_sum_, s.startup_time_ms_sum);
    processor::add(buffer_count_, s.buffer_count);
    processor::add(buffer_samples_, s.buffer_samples);
    processor::add(buffer_duration_ms_sum_, s.buffer_duration_ms_sum);
    processor::add(error_count_, s.error_count);
    processor::add(watch_time_ms_sum_, s.watch_time_ms_sum);
    processor::add(bitrate_samples_, s.bitrate_samples);
    processor::add(bitrate_kbps_sum_, s.bitrate_kbps_sum);
}

MetricsSnapshot MetricAccumulator::snapshot() const {
    MetricsSnapshot s;
    s.total_events = total_events_.load(kRelaxed);
    s.total_views = total_views_.load(kRelaxed);
    s.startup_samples = startup_samples_.load(kRelaxed);
    s.startup_time_ms_sum = startup_time_ms_sum_.load(kRelaxed);
    s.buffer_count = buffer_count_.load(kRelaxed);
    s.buffer_samples = buffer_samples_.load(kRelaxed);
    s.buffer_duration_ms_sum = buffer_duration_ms_sum_.load(kRelaxed);
    s.error_count = error_count_.load(kRelaxed);
    s.watch_time_ms_sum = watch_time_ms_sum_.load(kRelaxed);
    s.bitrate_samples = bitrate_samples_.load(kRelaxed);
    s.bitrate_kbps_sum = bitrate_kbps_sum_.load(kRelaxed);
    return s;
}

}  // namespace pulsedb::processor
