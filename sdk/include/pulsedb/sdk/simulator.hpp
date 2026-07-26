#pragma once

#include <chrono>
#include <cstddef>

#include "pulsedb/sdk/event_sink.hpp"
#include "pulsedb/sdk/session_generator.hpp"

namespace pulsedb::sdk {

/// How much traffic to generate and how to batch it.
struct SimulatorConfig {
    std::size_t sessions = 20;                    ///< Sessions to simulate.
    std::size_t batch_size = 50;                  ///< Events per POST (>= 1).
    std::chrono::milliseconds session_delay{0};   ///< Pause between sessions.
};

/// Running totals reported by a simulation run.
struct SimulatorStats {
    std::size_t sessions_generated = 0;
    std::size_t events_generated = 0;
    std::size_t batches_sent = 0;
    std::size_t batches_failed = 0;
    std::size_t events_sent = 0;
};

/// Drives the end-to-end producer path: generate sessions, accumulate their
/// events into fixed-size batches, and deliver each batch through an
/// EventSink. Delivery transport is fully injected (a retrying/spooling
/// sender in production, a fake in tests), so the simulator itself stays
/// agnostic to how batches reach the collector.
class Simulator {
public:
    Simulator(SimulatorConfig config, EventSink& sink, SessionGenerator generator);

    /// Generate and deliver all configured sessions. Blocking.
    SimulatorStats run();

private:
    void flush(nlohmann::json& batch, SimulatorStats& stats);

    SimulatorConfig config_;
    EventSink& sink_;
    SessionGenerator generator_;
};

}  // namespace pulsedb::sdk
