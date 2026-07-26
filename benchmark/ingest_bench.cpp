#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "bench_util.hpp"
#include "pulsedb/collector/ingest_handler.hpp"
#include "pulsedb/core/event_batch.hpp"

using pulsedb::bench::header;
using pulsedb::bench::keep;
using pulsedb::bench::report;
using pulsedb::bench::time_it;
using pulsedb::collector::IngestHandler;
using pulsedb::collector::IngestStatus;

namespace {

// A realistic JSON batch body of @p n playback events.
//
// Timestamps are anchored to "now" rather than a fixed epoch so the batch stays
// inside the handler's freshness window; a hardcoded past timestamp would be
// rejected and this would measure the reject path instead of the accept path.
std::string make_batch(int n) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            pulsedb::core::Clock::now().time_since_epoch())
                            .count();
    nlohmann::json events = nlohmann::json::array();
    for (int i = 0; i < n; ++i) {
        events.push_back({
            {"event_type", (i % 5 == 0) ? "bitrate_change" : "video_start"},
            {"session_id", "sess-" + std::to_string(i)},
            {"player", "ExoPlayer"},
            {"device", "Android"},
            {"timestamp", now_ms - i},
            {"bitrate_kbps", 3000},
        });
    }
    return events.dump();
}

// Validation throughput for a given batch size: raw JSON -> EventBatch.
void bench_batch(int n) {
    const std::string body = make_batch(n);
    const IngestHandler handler;
    const auto ingest_time = pulsedb::core::Clock::now();

    const long long iterations = 2'000'000 / n;  // ~constant total events
    const long long total_events = iterations * n;

    const double seconds = time_it([&] {
        for (long long i = 0; i < iterations; ++i) {
            auto result = handler.handle(body, ingest_time);
            keep(result.status);
        }
    });

    // Sanity: the body must actually validate.
    if (handler.handle(body, ingest_time).status != IngestStatus::kAccepted) {
        std::printf("  ERROR: batch of %d failed validation\n", n);
        return;
    }

    char label[48];
    std::snprintf(label, sizeof(label), "validate batch of %d", n);
    report(label, total_events, seconds, iterations * static_cast<long long>(body.size()));
}

}  // namespace

int main() {
    header("IngestHandler throughput (raw JSON batch -> validated EventBatch)");
    for (const int n : {1, 10, 50, 200}) {
        bench_batch(n);
    }
    return 0;
}
