#include "pulsedb/sdk/http_event_sink.hpp"

#include <httplib.h>

#include <string>
#include <utility>

namespace pulsedb::sdk {

struct HttpEventSink::Impl {
    Impl(const std::string& host, int port, std::string p)
        : client(host, port), path(std::move(p)) {
        client.set_keep_alive(true);
        client.set_connection_timeout(std::chrono::seconds(2));
        client.set_read_timeout(std::chrono::seconds(3));
        client.set_write_timeout(std::chrono::seconds(3));
    }

    httplib::Client client;
    std::string path;
};

HttpEventSink::HttpEventSink(const std::string& host, int port, std::string path)
    : impl_(std::make_unique<Impl>(host, port, std::move(path))) {}

HttpEventSink::~HttpEventSink() = default;

SendResult HttpEventSink::send(const nlohmann::json& events) {
    const std::string body = events.dump();
    const auto res = impl_->client.Post(impl_->path, body, "application/json");
    if (!res) {
        return SendResult::failure(
            0, "transport error (" + std::to_string(static_cast<int>(res.error())) + ")");
    }
    if (res->status == 202) {
        return SendResult::success(res->status);
    }
    return SendResult::failure(res->status,
                               "collector returned status " + std::to_string(res->status));
}

}  // namespace pulsedb::sdk
