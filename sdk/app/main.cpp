// PulseDB SDK simulator entry point.
//
// Generates realistic playback sessions and POSTs them to a running
// collector, batching events and retrying transient failures with
// exponential backoff. Useful as a load/traffic generator for the rest of
// the pipeline.
//
// Usage:
//   pulsedb_sdk [--url URL] [--sessions N] [--batch-size N]
//               [--seed N] [--delay-ms N]

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pulsedb/sdk/http_event_sink.hpp"
#include "pulsedb/sdk/retrying_sender.hpp"
#include "pulsedb/sdk/session_generator.hpp"
#include "pulsedb/sdk/simulator.hpp"
#include "pulsedb/sdk/spool_store.hpp"
#include "pulsedb/sdk/spooling_sender.hpp"
#include "pulsedb/sdk/url.hpp"

namespace {

std::atomic<bool> g_stop{false};
void handle_signal(int /*signum*/) { g_stop.store(true); }

std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Args {
    std::string url = "http://127.0.0.1:8080";
    std::string path = "/v1/events";
    std::size_t sessions = 20;
    std::size_t batch_size = 50;
    std::uint64_t seed = 0;   // 0 => derive a random seed
    long delay_ms = 0;
    std::string spool_dir = "spool";  // offline batches persisted here
    bool live = false;                // stream continuously with open sessions
    std::size_t concurrency = 25;     // live mode: target concurrent sessions
    long duration_sec = 0;            // live mode: 0 => run until Ctrl+C
    bool ok = true;
};

bool parse_ull(std::string_view sv, unsigned long long& out) {
    const char* begin = sv.data();
    const char* end = begin + sv.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const bool has_value = (i + 1 < argc);
        const std::string_view value = has_value ? argv[i + 1] : std::string_view{};

        auto take_ull = [&](std::size_t& dst) {
            unsigned long long v = 0;
            if (has_value && parse_ull(value, v)) {
                dst = static_cast<std::size_t>(v);
                ++i;
            } else {
                std::cerr << "invalid value for " << flag << '\n';
                args.ok = false;
            }
        };

        if (flag == "--url" && has_value) {
            args.url = value;
            ++i;
        } else if (flag == "--sessions") {
            take_ull(args.sessions);
        } else if (flag == "--batch-size") {
            take_ull(args.batch_size);
        } else if (flag == "--seed" && has_value) {
            unsigned long long v = 0;
            if (parse_ull(value, v)) {
                args.seed = v;
                ++i;
            } else {
                std::cerr << "invalid value for --seed\n";
                args.ok = false;
            }
        } else if (flag == "--delay-ms" && has_value) {
            args.delay_ms = std::atol(std::string(value).c_str());
            ++i;
        } else if (flag == "--spool-dir" && has_value) {
            args.spool_dir = value;
            ++i;
        } else if (flag == "--live") {
            args.live = true;
        } else if (flag == "--concurrency") {
            take_ull(args.concurrency);
        } else if (flag == "--duration-sec" && has_value) {
            args.duration_sec = std::atol(std::string(value).c_str());
            ++i;
        } else if (flag == "--help" || flag == "-h") {
            args.ok = false;
        } else {
            std::cerr << "unknown or incomplete argument: " << flag << '\n';
            args.ok = false;
        }
    }
    return args;
}

// Continuous "live" traffic: keep ~concurrency sessions open at once, emitting
// each session's events one per tick (timestamped now) and replacing a session
// when it finishes. Unlike batch mode -- which ships whole sessions at once --
// this keeps sessions genuinely in-flight, so the server's active-sessions
// gauge, events/sec and charts stay alive.
int run_live(const Args& args, const pulsedb::sdk::HostPort& hp, std::uint64_t seed) {
    using namespace std::chrono;

    pulsedb::sdk::HttpEventSink sink(hp.host, hp.port, args.path);
    pulsedb::sdk::RetryingSender sender(sink, pulsedb::sdk::BackoffPolicy{});
    pulsedb::sdk::SpoolStore spool(args.spool_dir);
    pulsedb::sdk::SpoolingSender spooling(sender, spool);
    pulsedb::sdk::SessionGenerator generator(seed);
    if (const auto replayed = spooling.replay();
        replayed.replayed > 0 || replayed.discarded > 0) {
        std::cout << "[sdk] replayed " << replayed.replayed << " spooled batch(es), discarded "
                  << replayed.discarded << '\n';
    }

    const std::size_t concurrency = args.concurrency == 0 ? 1 : args.concurrency;
    constexpr auto tick = milliseconds(150);

    struct LiveSession {
        std::vector<nlohmann::json> events;
        std::size_t idx = 0;
    };
    std::vector<LiveSession> live(concurrency);
    for (auto& s : live) {
        s.events = generator.generate_session(now_epoch_ms());
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "[sdk] LIVE mode: ~" << concurrency << " concurrent sessions -> "
              << hp.host << ':' << hp.port << args.path << "  (Ctrl+C to stop)\n";

    const auto start = steady_clock::now();
    const auto deadline = args.duration_sec > 0
                              ? start + seconds(args.duration_sec)
                              : steady_clock::time_point::max();

    std::uint64_t events_sent = 0;
    std::uint64_t batches_sent = 0;
    std::uint64_t batches_failed = 0;
    int report_ticks = 0;

    while (!g_stop.load() && steady_clock::now() < deadline) {
        nlohmann::json batch = nlohmann::json::array();
        for (auto& s : live) {
            if (s.idx >= s.events.size()) {  // session finished -> start a new one
                s.events = generator.generate_session(now_epoch_ms());
                s.idx = 0;
            }
            nlohmann::json event = s.events[s.idx++];
            event["timestamp"] = now_epoch_ms();  // emit at real time (current minute)
            batch.push_back(std::move(event));
        }

        if (spooling.send(batch).ok) {
            events_sent += batch.size();
            ++batches_sent;
        } else {
            ++batches_failed;
        }

        if (++report_ticks % 20 == 0) {  // ~every 3s
            std::cout << "[sdk] live: events_sent=" << events_sent
                      << " batches=" << batches_sent
                      << (batches_failed ? " (failed=" + std::to_string(batches_failed) +
                                               ", spooling to disk)"
                                         : "")
                      << '\n';
        }
        std::this_thread::sleep_for(tick);
    }

    std::cout << "[sdk] live stopped: events_sent=" << events_sent
              << " batches_sent=" << batches_sent
              << " batches_failed=" << batches_failed
              << " spooled=" << spooling.spooled_count() << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    if (!args.ok) {
        std::cerr << "usage: pulsedb_sdk [--url URL] [--seed N] [--spool-dir DIR]\n"
                     "  batch mode (default): [--sessions N] [--batch-size N] [--delay-ms N]\n"
                     "  live mode:            --live [--concurrency N] [--duration-sec N]\n";
        return 2;
    }

    const auto host_port = pulsedb::sdk::parse_http_url(args.url);
    if (!host_port) {
        std::cerr << "error: could not parse collector URL: " << args.url << '\n';
        return 2;
    }
    // Refuse rather than downgrade. This build has no TLS support, so honouring
    // an https:// URL would mean sending telemetry unencrypted to port 443 while
    // the operator believed it was protected. Failing loudly is the only safe
    // response to a security guarantee we cannot provide.
    if (host_port->tls) {
        std::cerr << "error: https:// is not supported -- this build has no TLS "
                     "support, and sending telemetry in the clear to a URL that "
                     "asked for encryption would be worse than refusing.\n"
                     "       Use http://, or terminate TLS at a proxy in front "
                     "of the collector.\n";
        return 2;
    }

    std::uint64_t seed = args.seed;
    if (seed == 0) {
        std::random_device rd;
        seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }

    if (args.live) {
        return run_live(args, *host_port, seed);
    }

    // Delivery pipeline: HTTP -> retry-with-backoff -> spool-to-disk-on-failure.
    pulsedb::sdk::HttpEventSink sink(host_port->host, host_port->port, args.path);
    pulsedb::sdk::RetryingSender sender(sink, pulsedb::sdk::BackoffPolicy{});
    pulsedb::sdk::SpoolStore spool(args.spool_dir);
    pulsedb::sdk::SpoolingSender spooling(sender, spool);
    pulsedb::sdk::SessionGenerator generator(seed);

    pulsedb::sdk::SimulatorConfig config;
    config.sessions = args.sessions;
    config.batch_size = args.batch_size;
    config.session_delay = std::chrono::milliseconds(args.delay_ms);

    // Replay anything spooled during a previous offline run before sending new
    // traffic, so recovery happens as soon as the collector is reachable.
    if (const auto replayed = spooling.replay();
        replayed.replayed > 0 || replayed.failed > 0 || replayed.discarded > 0) {
        std::cout << "[sdk] replayed " << replayed.replayed << " spooled batch(es)";
        if (replayed.discarded > 0) {
            std::cout << ", discarded " << replayed.discarded
                      << " permanently rejected/corrupt";
        }
        if (replayed.failed > 0) {
            std::cout << " (collector still unreachable)";
        }
        std::cout << '\n';
    }

    std::cout << "[sdk] sending " << config.sessions << " sessions to "
              << host_port->host << ':' << host_port->port << args.path
              << " (batch_size=" << config.batch_size << ", seed=" << seed << ")\n";

    pulsedb::sdk::Simulator simulator(config, spooling, std::move(generator));
    const auto stats = simulator.run();

    std::cout << "[sdk] done:\n"
              << "  sessions generated : " << stats.sessions_generated << '\n'
              << "  events generated   : " << stats.events_generated << '\n'
              << "  batches sent        : " << stats.batches_sent << '\n'
              << "  batches failed      : " << stats.batches_failed << '\n'
              << "  events delivered    : " << stats.events_sent << '\n'
              << "  retries             : " << sender.total_retries() << '\n'
              << "  batches spooled     : " << spooling.spooled_count()
              << " (in " << spool.dir().string() << ", cap " << spool.max_batches() << ")\n"
              << "  batches discarded   : " << spooling.discarded_count()
              << " (permanently rejected or corrupt)\n"
              << "  spool write failures: " << spooling.spool_failure_count()
              << " (batches lost -- could not reach disk)\n"
              << "  batches evicted     : " << spool.evicted_count() << " (spool full)\n";

    return stats.batches_failed == 0 ? 0 : 1;
}
