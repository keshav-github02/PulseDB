#include "pulsedb/query/query_server.hpp"

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "pulsedb/query/metrics_api.hpp"
#include "pulsedb/storage/minute_key.hpp"

namespace pulsedb::query {
namespace {

void write_json(httplib::Response& res, const nlohmann::json& body, int status = 200) {
    res.status = status;
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(body.dump(), "application/json");
}

std::optional<std::int64_t> parse_i64(const std::string& text) {
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

struct QueryServer::Impl {
    Impl(Config cfg, const aggregation::AggregationEngine& engine, const RuntimeStatus* rt)
        : config(std::move(cfg)), api(engine), runtime(rt) {
        setup_routes();
    }

    void setup_routes() {
        // An index at the root, because nothing was served there and a bare
        // http://host:<query_port>/ answered 404. That is technically correct --
        // this port carries a JSON API, not a site -- but it is also the first
        // thing anyone tries, and a 404 reads as "the server is broken" rather
        // than "you want a path". Listing the routes turns the dead end into a
        // pointer, and costs nothing to keep accurate.
        server.Get("/", [this](const httplib::Request&, httplib::Response& res) {
            write_json(
                res,
                {{"service", "pulsedb-metrics-api"},
                 {"description", "Read-only metrics API. Event ingestion is a separate "
                                 "port -- POST /v1/events there, not here."},
                 {"endpoints",
                  {{"/metrics", "platform-wide totals plus the number of minute buckets"},
                   {"/metrics/live?minutes=N",
                    "the N most recent per-minute buckets; N is clamped to "
                    "max_response_minutes"},
                   {"/metrics/range?from=<epoch_ms>&to=<epoch_ms>",
                    "buckets within a window; a window wider than max_response_minutes "
                    "is refused with 400 rather than truncated"},
                   {"/metrics/player", "per-player metric breakdown"},
                   {"/metrics/device", "per-device metric breakdown"},
                   {"/status",
                    "live operational metrics: events/sec, queue depth, active sessions, "
                    "error rate, CPU, memory, uptime"},
                   {"/health", "liveness probe"}}},
                 {"max_response_minutes", config.max_response_minutes},
                 {"dashboard",
                  "a separate Vite app: run `npm run dev` in dashboard/ and open the "
                  "URL it prints (it polls this API)"}});
        });

        server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            write_json(res, {{"status", "ok"}});
        });

        server.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            if (runtime == nullptr) {
                write_json(res, {{"error", "runtime status unavailable"}}, 503);
                return;
            }
            const RuntimeStatusSnapshot s = runtime->get();
            nlohmann::json body;
            body["uptime_sec"] = s.uptime_sec;
            body["workers"] = s.workers;
            body["queue_depth"] = s.queue_depth;
            body["events_per_sec"] = s.events_per_sec;
            body["active_sessions"] = s.active_sessions;
            body["total_events"] = s.total_events;
            body["error_count"] = s.error_count;
            body["error_rate"] = s.error_rate;
            body["cpu_percent"] = s.cpu_percent;
            body["memory_mb"] = s.memory_mb;
            write_json(res, body);
        });

        server.Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
            write_json(res, api.overall());
        });

        server.Get("/metrics/live", [this](const httplib::Request& req, httplib::Response& res) {
            std::size_t minutes = 15;
            if (req.has_param("minutes")) {
                if (const auto n = parse_i64(req.get_param_value("minutes")); n && *n > 0) {
                    minutes = static_cast<std::size_t>(*n);
                }
            }
            // Clamp rather than reject: `minutes` is a "give me the most recent N"
            // hint, so silently capping it still answers the question asked, just
            // truncated. An uncapped value let one request serialise the whole
            // store (see Config::max_response_minutes).
            write_json(res, api.live(std::min(minutes, config.max_response_minutes)));
        });

        server.Get("/metrics/range", [this](const httplib::Request& req, httplib::Response& res) {
            // Written as plain ifs rather than `has_param(...) ? parse(...) :
            // std::nullopt`: the ternary's mixed operand types defeated GCC's
            // flow analysis, so it warned that *from / *to below may be
            // uninitialised (-Wmaybe-uninitialized) despite the guard.
            std::optional<std::int64_t> from;
            std::optional<std::int64_t> to;
            if (req.has_param("from")) {
                from = parse_i64(req.get_param_value("from"));
            }
            if (req.has_param("to")) {
                to = parse_i64(req.get_param_value("to"));
            }
            if (!from || !to) {
                write_json(res,
                           {{"error", "query params 'from' and 'to' (epoch ms) are required"}},
                           400);
                return;
            }
            // Same hazard as on the ingest path: these reach
            // MinuteKey::from_timestamp_ms, which silently wraps rather than
            // failing on out-of-domain input and would echo a malformed ISO
            // string with a negative year back to the caller.
            if (!storage::is_valid_timestamp_ms(*from) ||
                !storage::is_valid_timestamp_ms(*to)) {
                write_json(res,
                           {{"error", "'from' and 'to' must be epoch ms within [" +
                                          std::to_string(storage::kMinTimestampMs) + ", " +
                                          std::to_string(storage::kMaxTimestampMs) + "]"}},
                           400);
                return;
            }
            if (*from > *to) {
                write_json(res, {{"error", "'from' must not be greater than 'to'"}}, 400);
                return;
            }
            // A window is rejected rather than clamped, unlike /metrics/live: there
            // is no truncation of [from, to] that is still the answer to the
            // question, and quietly returning a slice of the requested range would
            // read as "these are all the buckets in that window". Paging is the
            // caller's decision, so tell them the limit and let them make it.
            //
            // Computed in minutes on int64 (both bounds are already known to be
            // inside the representable domain, so the subtraction cannot overflow).
            const std::int64_t span_minutes = (*to - *from) / 60'000 + 1;
            if (span_minutes > static_cast<std::int64_t>(config.max_response_minutes)) {
                write_json(res,
                           {{"error", "requested window spans " +
                                          std::to_string(span_minutes) +
                                          " minutes, over the " +
                                          std::to_string(config.max_response_minutes) +
                                          "-minute limit; request a narrower range"}},
                           400);
                return;
            }
            write_json(res, api.range(*from, *to));
        });

        server.Get("/metrics/player", [this](const httplib::Request&, httplib::Response& res) {
            write_json(res, api.by_player());
        });

        server.Get("/metrics/device", [this](const httplib::Request&, httplib::Response& res) {
            write_json(res, api.by_device());
        });

        // CORS preflight for any path.
        server.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.status = 204;
        });
    }

    Config config;
    MetricsApi api;
    const RuntimeStatus* runtime;
    httplib::Server server;
};

QueryServer::QueryServer(Config config, const aggregation::AggregationEngine& engine,
                         const RuntimeStatus* runtime)
    : impl_(std::make_unique<Impl>(std::move(config), engine, runtime)) {}

QueryServer::~QueryServer() {
    if (impl_) {
        impl_->server.stop();
    }
}

bool QueryServer::listen() {
    return impl_->server.listen(impl_->config.host, impl_->config.port);
}

int QueryServer::bind_to_any_port() {
    return impl_->server.bind_to_any_port(impl_->config.host);
}

void QueryServer::listen_after_bind() { impl_->server.listen_after_bind(); }

void QueryServer::stop() { impl_->server.stop(); }

bool QueryServer::is_running() const { return impl_->server.is_running(); }

}  // namespace pulsedb::query
