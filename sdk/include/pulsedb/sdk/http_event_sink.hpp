#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "pulsedb/sdk/event_sink.hpp"

namespace pulsedb::sdk {

/// An EventSink that POSTs batches to a PulseDB collector over **plaintext**
/// HTTP.
///
/// There is no TLS: this build of cpp-httplib is compiled without OpenSSL, so
/// there is no port or argument that makes this sink encrypt. Callers resolving
/// a user-supplied URL must reject an https:// scheme rather than routing it
/// here (see HostPort::tls); terminate TLS at a proxy if it is required.
///
/// A 202 response is treated as success; any other status, or a transport
/// error, is reported as a failure (with the status preserved so callers
/// can decide whether to retry). The httplib client is hidden behind a
/// PImpl so this header carries no networking dependency.
class HttpEventSink : public EventSink {
public:
    /// @param host   Collector host (e.g. "127.0.0.1").
    /// @param port   Collector port (e.g. 8080).
    /// @param path   Ingest route (e.g. "/v1/events").
    HttpEventSink(const std::string& host, int port, std::string path);
    ~HttpEventSink() override;

    HttpEventSink(const HttpEventSink&) = delete;
    HttpEventSink& operator=(const HttpEventSink&) = delete;

    [[nodiscard]] SendResult send(const nlohmann::json& events) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulsedb::sdk
