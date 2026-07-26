#include "pulsedb/collector/collector.hpp"

#include <httplib.h>

#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace pulsedb::collector {
namespace {

int to_http_status(IngestStatus status) {
    switch (status) {
        case IngestStatus::kAccepted:         return 202;
        case IngestStatus::kInvalidJson:      return 400;
        case IngestStatus::kInvalidSchema:    return 422;
        case IngestStatus::kPayloadTooLarge:  return 413;
    }
    return 500;
}

void respond_json(httplib::Response& res, int status, const std::string& state,
                  const std::string& message) {
    nlohmann::json body;
    body["status"] = state;
    body["message"] = message;
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

}  // namespace

struct Collector::Impl {
    Impl(CollectorConfig cfg, EventQueue& q)
        : config(std::move(cfg)), queue(q), handler(config.ingest) {
        setup_routes();
    }

    void setup_routes() {
        // Refuse oversized bodies at the transport layer: httplib checks this
        // while reading and answers 413 itself, so an attacker cannot make us
        // buffer an arbitrarily large body into memory before any of our own
        // validation runs. Without it httplib defaults to SIZE_MAX.
        server.set_payload_max_length(config.ingest.max_body_bytes);

        server.Post(config.ingest_path,
                    [this](const httplib::Request& req, httplib::Response& res) {
                        handle_ingest(req, res);
                    });

        server.Get("/health",
                   [](const httplib::Request&, httplib::Response& res) {
                       nlohmann::json body;
                       body["status"] = "ok";
                       res.status = 200;
                       res.set_content(body.dump(), "application/json");
                   });
    }

    void handle_ingest(const httplib::Request& req, httplib::Response& res) {
        // Non-const so the accepted batch can be moved (not copied) out below.
        IngestResult result = handler.handle(req.body, core::Clock::now());
        if (result.status != IngestStatus::kAccepted) {
            respond_json(res, to_http_status(result.status), "error", result.message);
            return;
        }

        core::EventBatch batch = std::move(*result.batch);
        const std::size_t count = batch.size();

        // try_push (not push): never block the acceptor thread. A full
        // queue is surfaced as 503 so clients retry with backoff.
        if (!queue.try_push(std::move(batch))) {
            respond_json(res, 503, "unavailable",
                         "ingestion queue is full; retry after backoff");
            return;
        }

        nlohmann::json body;
        body["status"] = "accepted";
        body["accepted"] = count;
        body["queue_depth"] = queue.size();
        res.status = 202;
        res.set_content(body.dump(), "application/json");
    }

    CollectorConfig config;
    EventQueue& queue;
    IngestHandler handler;
    httplib::Server server;
};

Collector::Collector(CollectorConfig config, EventQueue& queue)
    : impl_(std::make_unique<Impl>(std::move(config), queue)) {}

Collector::~Collector() {
    if (impl_) {
        impl_->server.stop();
    }
}

bool Collector::listen() {
    return impl_->server.listen(impl_->config.host, impl_->config.port);
}

int Collector::bind_to_any_port() {
    return impl_->server.bind_to_any_port(impl_->config.host);
}

void Collector::listen_after_bind() { impl_->server.listen_after_bind(); }

void Collector::stop() { impl_->server.stop(); }

bool Collector::is_running() const { return impl_->server.is_running(); }

}  // namespace pulsedb::collector
