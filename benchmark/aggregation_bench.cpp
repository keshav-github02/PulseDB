#include <array>
#include <cstdint>
#include <thread>
#include <vector>

#include "bench_util.hpp"
#include "pulsedb/aggregation/aggregation_engine.hpp"
#include "pulsedb/core/event.hpp"

using pulsedb::aggregation::AggregationEngine;
using pulsedb::bench::header;
using pulsedb::bench::keep;
using pulsedb::bench::report;
using pulsedb::bench::time_it;
using pulsedb::core::Event;
using pulsedb::core::EventType;

namespace {

constexpr std::int64_t kBaseMs = 1'700'000'000'000;
constexpr std::array<const char*, 5> kPlayers{"ExoPlayer", "AVPlayer", "Shaka", "HLS.js", "VideoJS"};
constexpr std::array<const char*, 6> kDevices{"Android", "iOS", "Web", "Roku", "SmartTV", "FireTV"};

// A representative event whose minute/player/device vary with i, so the
// aggregation touches many buckets and segments (as real traffic would).
Event make_event(long long i) {
    Event e;
    e.type = (i % 7 == 0) ? EventType::kBufferStart : EventType::kVideoStart;
    e.timestamp_ms = kBaseMs + (i % 15) * 60'000;  // spread over 15 minutes
    e.player = kPlayers[static_cast<std::size_t>(i) % kPlayers.size()];
    e.device = kDevices[static_cast<std::size_t>(i) % kDevices.size()];
    e.bitrate_kbps = 3000;
    return e;
}

// Single-thread event-folding throughput.
void bench_single() {
    constexpr long long kN = 2'000'000;
    AggregationEngine engine;
    const double seconds = time_it([&] {
        for (long long i = 0; i < kN; ++i) {
            engine.process(make_event(i));
        }
    });
    report("process (1 thread)", kN, seconds);
    keep(engine.total().total_events);
}

// Concurrent folding into one shared engine (mirrors the worker pool).
void bench_concurrent(int threads) {
    constexpr long long kPerThread = 250'000;
    const long long total = static_cast<long long>(threads) * kPerThread;

    AggregationEngine engine;
    const double seconds = time_it([&] {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&engine, t] {
                const long long base = static_cast<long long>(t) * kPerThread;
                for (long long i = 0; i < kPerThread; ++i) {
                    engine.process(make_event(base + i));
                }
            });
        }
        workers.clear();  // join
    });

    char label[48];
    std::snprintf(label, sizeof(label), "process (%d threads)", threads);
    report(label, total, seconds);
    keep(engine.total().total_events);
}

}  // namespace

int main() {
    header("AggregationEngine throughput (event -> per-minute bucket)");
    bench_single();
    for (const int threads : {1, 2, 4, 8}) {
        bench_concurrent(threads);
    }
    return 0;
}
