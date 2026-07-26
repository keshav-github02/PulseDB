#include "pulsedb/sdk/simulator.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace pulsedb::sdk {
namespace {

std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Simulator::Simulator(SimulatorConfig config, EventSink& sink,
                     SessionGenerator generator)
    : config_(config), sink_(sink), generator_(std::move(generator)) {}

void Simulator::flush(nlohmann::json& batch, SimulatorStats& stats) {
    if (batch.empty()) {
        return;
    }
    const std::size_t count = batch.size();
    const SendResult result = sink_.send(batch);
    if (result.ok) {
        ++stats.batches_sent;
        stats.events_sent += count;
    } else {
        ++stats.batches_failed;
    }
    batch = nlohmann::json::array();
}

SimulatorStats Simulator::run() {
    const std::size_t batch_size = config_.batch_size == 0 ? 1 : config_.batch_size;
    SimulatorStats stats;
    nlohmann::json batch = nlohmann::json::array();

    // Spread session start times across the ~10 minutes leading up to now,
    // so the resulting events land in a realistic recent range of minute
    // buckets rather than all at a single instant.
    constexpr std::int64_t kWindowMs = 10 * 60 * 1000;
    const std::int64_t now_ms = now_epoch_ms();
    const std::int64_t step_ms =
        config_.sessions > 1
            ? kWindowMs / static_cast<std::int64_t>(config_.sessions - 1)
            : 0;

    for (std::size_t s = 0; s < config_.sessions; ++s) {
        const std::int64_t start_ms =
            now_ms - kWindowMs + static_cast<std::int64_t>(s) * step_ms;
        auto events = generator_.generate_session(start_ms);

        ++stats.sessions_generated;
        stats.events_generated += events.size();

        for (auto& event : events) {
            batch.push_back(std::move(event));
            if (batch.size() >= batch_size) {
                flush(batch, stats);
            }
        }

        if (config_.session_delay.count() > 0) {
            std::this_thread::sleep_for(config_.session_delay);
        }
    }

    flush(batch, stats);  // send any remainder
    return stats;
}

}  // namespace pulsedb::sdk
