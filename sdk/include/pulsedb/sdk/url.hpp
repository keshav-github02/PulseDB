#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pulsedb::sdk {

/// A host, TCP port, and requested scheme parsed from a URL.
struct HostPort {
    std::string host;
    int port;
    /// True when the URL asked for https.
    ///
    /// Callers MUST check this. HttpEventSink speaks plaintext HTTP only (this
    /// build of cpp-httplib is compiled without OpenSSL), so ignoring the flag
    /// means an "https://..." URL sends telemetry in the clear to port 443 --
    /// worse than refusing TLS outright, because the operator asked for
    /// encryption and was silently given none.
    bool tls = false;
};

/// Parse an HTTP(S) URL into host, port and scheme.
///
/// Accepts forms like "http://host:port/path", "host:port" and "host".
/// The scheme and path are optional; a missing port defaults to 80 for
/// http (or no scheme) and 443 for https. Returns std::nullopt when the
/// host is empty or the port is not a valid 1..65535 integer.
///
/// Parsing an https URL succeeds -- deciding what to do about a scheme nothing
/// can honour is the caller's policy, not the parser's job. See HostPort::tls.
std::optional<HostPort> parse_http_url(std::string_view url);

}  // namespace pulsedb::sdk
