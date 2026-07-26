// PulseDB server entry point.
//
// Wires the full ingestion + processing pipeline:
//   HTTP collector -> bounded queue -> worker pool -> metrics processor
// and periodically reports the live metrics derived from the event stream.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pulsedb/aggregation/aggregation_engine.hpp"
#include "pulsedb/collector/collector.hpp"
#include "pulsedb/persistence/metrics_snapshot_store.hpp"
#include "pulsedb/processor/metrics.hpp"
#include "pulsedb/query/query_server.hpp"
#include "pulsedb/query/runtime_status.hpp"
#include "pulsedb/query/system_stats.hpp"
#include "pulsedb/workers/worker_pool.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int /*signum*/) { g_stop.store(true); }

struct AppConfig {
    std::size_t queue_size = 100'000;
    std::size_t workers = 0;  // 0 => hardware_concurrency()
    std::string host = "0.0.0.0";
    int port = 8080;        // ingestion (collector)
    int query_port = 8081;  // read-only metrics API
    std::string snapshot_path = "data/metrics.json";  // "" disables persistence
    int snapshot_interval_sec = 10;                   // 0 disables periodic saves
};

/// Reads an integer field, range-checked.
///
/// Every value is read as int64 *before* being narrowed, because the previous
/// code went straight to get<std::size_t>(): a negative value wrapped to a huge
/// unsigned one, so "queue_size": -1 silently produced a queue of capacity
/// SIZE_MAX and disabled backpressure entirely -- the safety mechanism the
/// whole ingest path depends on, switched off by a typo, with no diagnostic.
template <typename T>
void read_int(const nlohmann::json& j, const char* key, std::int64_t lo, std::int64_t hi,
              T& out, std::vector<std::string>& errors) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return;
    }
    if (!it->is_number_integer()) {
        errors.emplace_back(std::string(key) + ": must be an integer");
        return;
    }
    const auto value = it->get<std::int64_t>();
    if (value < lo || value > hi) {
        errors.emplace_back(std::string(key) + ": " + std::to_string(value) +
                            " is out of range [" + std::to_string(lo) + ", " +
                            std::to_string(hi) + "]");
        return;
    }
    out = static_cast<T>(value);
}

void read_string(const nlohmann::json& j, const char* key, bool allow_empty,
                 std::string& out, std::vector<std::string>& errors) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return;
    }
    if (!it->is_string()) {
        errors.emplace_back(std::string(key) + ": must be a string");
        return;
    }
    auto value = it->get<std::string>();
    if (value.empty() && !allow_empty) {
        errors.emplace_back(std::string(key) + ": must not be empty");
        return;
    }
    out = std::move(value);
}

/// Load and validate the config, or report every problem found.
///
/// Returns std::nullopt on any invalid value so the caller can exit instead of
/// starting up mis-configured. A missing file is fine (defaults apply), but a
/// file that exists and is wrong is fatal: silently falling back to defaults
/// meant the server ran with settings the operator did not choose and had no
/// way to notice.
std::optional<AppConfig> load_config(const std::string& path) {
    AppConfig cfg;
    std::ifstream in(path);
    if (!in) {
        std::clog << "[server] config '" << path << "' not found; using defaults\n";
        return cfg;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        std::cerr << "[server] ERROR: config '" << path << "' is not valid JSON: " << e.what()
                  << '\n';
        return std::nullopt;
    }
    if (!j.is_object()) {
        std::cerr << "[server] ERROR: config '" << path << "' must contain a JSON object\n";
        return std::nullopt;
    }

    std::vector<std::string> errors;

    // Upper bounds are sanity limits, not capability claims: they exist so a
    // fat-fingered value fails loudly at startup rather than as an OOM or a
    // thread-spawn storm later.
    read_int(j, "queue_size", 1, 10'000'000, cfg.queue_size, errors);
    read_int(j, "workers", 0, 1024, cfg.workers, errors);  // 0 => hardware_concurrency()
    read_int(j, "port", 1, 65535, cfg.port, errors);
    read_int(j, "query_port", 1, 65535, cfg.query_port, errors);
    read_int(j, "snapshot_interval_sec", 0, 86400, cfg.snapshot_interval_sec, errors);
    read_string(j, "host", /*allow_empty=*/false, cfg.host, errors);
    read_string(j, "snapshot_path", /*allow_empty=*/true, cfg.snapshot_path, errors);

    if (cfg.port == cfg.query_port) {
        errors.emplace_back("port and query_port must differ (both " +
                            std::to_string(cfg.port) + ")");
    }

    // Unknown keys are rejected rather than ignored, so a typo like
    // "queue_siz" fails at startup instead of quietly leaving the default.
    static constexpr std::string_view kKnownKeys[] = {
        "queue_size", "workers", "host", "port", "query_port",
        "snapshot_path", "snapshot_interval_sec",
    };
    for (const auto& [key, _] : j.items()) {
        if (std::find(std::begin(kKnownKeys), std::end(kKnownKeys), key) ==
            std::end(kKnownKeys)) {
            errors.emplace_back("unknown key '" + key + "'");
        }
    }

    if (!errors.empty()) {
        std::cerr << "[server] ERROR: invalid config '" << path << "':\n";
        for (const auto& error : errors) {
            std::cerr << "  - " << error << '\n';
        }
        return std::nullopt;
    }
    return cfg;
}

void report(const pulsedb::processor::MetricsSnapshot& m,
            const pulsedb::workers::WorkerStats& w, std::size_t queue_depth,
            std::size_t minute_buckets) {
    std::clog << std::fixed << std::setprecision(1)
              << "[metrics] events=" << m.total_events
              << " views=" << m.total_views
              << " startup(avg)=" << m.avg_startup_ms() << "ms"
              << " buffers=" << m.buffer_count
              << " buf(avg)=" << m.avg_buffer_ms() << "ms"
              << " errors=" << m.error_count
              << " watch=" << (static_cast<double>(m.watch_time_ms_sum) / 60'000.0) << "min"
              << " bitrate(avg)=" << m.avg_bitrate_kbps() << "kbps"
              << " | queue=" << queue_depth
              << " minutes=" << minute_buckets
              << " workers_done=" << w.events_processed << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = (argc > 1) ? argv[1] : "config/pulsedb.json";
    const auto loaded = load_config(config_path);
    if (!loaded) {
        return 2;  // fail fast rather than serve with settings nobody chose
    }
    const AppConfig cfg = *loaded;

    pulsedb::collector::EventQueue queue{cfg.queue_size};
    pulsedb::aggregation::AggregationEngine engine;

    // Restore metrics from the previous run, if a snapshot exists.
    const bool persistence_on = !cfg.snapshot_path.empty();
    if (persistence_on) {
        std::string error;
        if (pulsedb::persistence::load(engine, cfg.snapshot_path, &error)) {
            std::clog << "[server] restored metrics from " << cfg.snapshot_path << " ("
                      << engine.minute_count() << " minute buckets, "
                      << engine.total().total_events << " events)\n";
        } else {
            // A corrupt snapshot used to be indistinguishable from no snapshot.
            std::clog << "[server] starting with empty metrics: " << error << '\n';
        }
    }

    pulsedb::workers::WorkerPool pool{queue, engine, cfg.workers};
    pool.start();

    pulsedb::collector::CollectorConfig collector_cfg;
    collector_cfg.host = cfg.host;
    collector_cfg.port = cfg.port;
    pulsedb::collector::Collector collector{collector_cfg, queue};

    pulsedb::query::RuntimeStatus runtime_status;

    pulsedb::query::QueryServer::Config query_cfg;
    query_cfg.host = cfg.host;
    query_cfg.port = cfg.query_port;
    pulsedb::query::QueryServer query{query_cfg, engine, &runtime_status};

    const auto start_time = std::chrono::steady_clock::now();

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::thread server_thread([&collector, &cfg, &pool] {
        std::clog << "[server] ingest  on http://" << cfg.host << ':' << cfg.port
                  << "  (" << pool.worker_count() << " workers, POST /v1/events)\n";
        if (!collector.listen()) {
            std::cerr << "[server] ERROR: failed to bind " << cfg.host << ':' << cfg.port
                      << '\n';
            g_stop.store(true);
        }
    });

    std::thread query_thread([&query, &cfg] {
        std::clog << "[server] metrics on http://" << cfg.host << ':' << cfg.query_port
                  << "  (GET /metrics, /metrics/live, /metrics/player, /metrics/device, /status)\n";
        if (!query.listen()) {
            std::cerr << "[server] ERROR: failed to bind query port " << cfg.query_port << '\n';
            g_stop.store(true);
        }
    });

    // Live metrics reporter: every ~2s, publish operational stats (for the
    // GET /status endpoint and the dashboard) and print a summary line.
    std::thread reporter([&] {
        pulsedb::query::SystemStats sys;
        int ticks = 0;
        std::uint64_t last_events = 0;
        auto last_sample = std::chrono::steady_clock::now();
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (++ticks % 10 != 0) {
                continue;
            }
            const auto metrics = engine.total();
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last_sample).count();
            const double eps =
                dt > 0.0 ? static_cast<double>(metrics.total_events - last_events) / dt : 0.0;
            last_events = metrics.total_events;
            last_sample = now;

            pulsedb::query::RuntimeStatusSnapshot snap;
            snap.uptime_sec = std::chrono::duration<double>(now - start_time).count();
            snap.workers = pool.worker_count();
            snap.queue_depth = queue.size();
            snap.events_per_sec = eps;
            snap.active_sessions = engine.active_sessions();
            snap.total_events = metrics.total_events;
            snap.error_count = metrics.error_count;
            snap.error_rate = metrics.total_events > 0
                                  ? static_cast<double>(metrics.error_count) /
                                        static_cast<double>(metrics.total_events)
                                  : 0.0;
            snap.cpu_percent = sys.cpu_percent();
            snap.memory_mb = static_cast<double>(pulsedb::query::SystemStats::memory_bytes()) /
                             (1024.0 * 1024.0);
            runtime_status.set(snap);

            report(metrics, pool.stats(), queue.size(), engine.minute_count());
        }
    });

    // Periodically snapshot metrics to disk so they survive a restart.
    const bool periodic_snapshots = persistence_on && cfg.snapshot_interval_sec > 0;
    std::thread snapshotter([&] {
        if (!periodic_snapshots) {
            return;
        }
        const int ticks_per_save = cfg.snapshot_interval_sec * 5;  // 200ms ticks
        int ticks = 0;
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (++ticks % ticks_per_save == 0) {
                std::string error;
                if (!pulsedb::persistence::save(engine, cfg.snapshot_path, &error)) {
                    // Previously the return value was discarded, so a full disk
                    // meant snapshots silently stopped happening.
                    std::clog << "[server] WARNING: snapshot failed: " << error << '\n';
                }
            }
        }
    });

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::clog << "[server] shutting down...\n";
    collector.stop();  // stop accepting ingest requests
    query.stop();      // stop serving the metrics API
    pool.stop();       // close queue, drain remaining batches, join workers

    server_thread.join();
    query_thread.join();
    reporter.join();
    snapshotter.join();

    // Final snapshot so the very latest state is persisted on a clean exit.
    // The error is reported here for the same reason the periodic path reports
    // it: discarding it meant a failed final save was indistinguishable from a
    // successful one, and it is the save whose loss costs the most.
    if (persistence_on) {
        std::string error;
        if (pulsedb::persistence::save(engine, cfg.snapshot_path, &error)) {
            std::clog << "[server] saved metrics snapshot to " << cfg.snapshot_path << '\n';
        } else {
            std::cerr << "[server] ERROR: final snapshot failed: " << error << '\n';
        }
    }

    std::clog << "[server] final metrics (" << engine.minute_count()
              << " minute buckets):\n";
    report(engine.total(), pool.stats(), queue.size(), engine.minute_count());
    return 0;
}
