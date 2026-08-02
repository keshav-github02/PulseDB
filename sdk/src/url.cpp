#include "pulsedb/sdk/url.hpp"

namespace pulsedb::sdk {

std::optional<HostPort> parse_http_url(std::string_view url) {
    std::string_view s = url;
    int default_port = 80;
    bool tls = false;

    if (s.starts_with("http://")) {
        s.remove_prefix(7);
    } else if (s.starts_with("https://")) {
        s.remove_prefix(8);
        default_port = 443;
        tls = true;
    }

    // Drop any path/query component.
    if (const auto slash = s.find('/'); slash != std::string_view::npos) {
        s = s.substr(0, slash);
    }
    if (s.empty()) {
        return std::nullopt;
    }

    const auto parse_port = [](std::string_view text, int& out) {
        if (text.empty()) {
            return false;
        }
        int value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
            if (value > 65535) {
                return false;
            }
        }
        if (value == 0) {
            return false;
        }
        out = value;
        return true;
    };

    int port = default_port;
    std::string_view host_sv = s;

    if (s.front() == '[') {
        // Bracketed IPv6 literal. The brackets exist precisely so the address's
        // own colons are not mistaken for a port separator, so the closing one
        // has to be found before looking for a port -- rfind(':') alone split
        // "[::1]" into a host of "[:" and a port of "1]".
        const auto close = s.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host_sv = s.substr(1, close - 1);  // brackets are wire syntax, not the host
        if (const std::string_view rest = s.substr(close + 1); !rest.empty()) {
            if (rest.front() != ':' || !parse_port(rest.substr(1), port)) {
                return std::nullopt;
            }
        }
    } else {
        if (const auto colon = s.rfind(':'); colon != std::string_view::npos) {
            host_sv = s.substr(0, colon);
            if (!parse_port(s.substr(colon + 1), port)) {
                return std::nullopt;
            }
        }
        // Only the *last* colon was treated as the port separator, so anything
        // structural left in the host means this was never a host:port pair.
        // These used to be accepted and handed to the HTTP client, which then
        // failed at connect time with an opaque transport error -- so a typo'd
        // --url looked like the collector was down. Rejecting here reaches the
        // caller's "could not parse collector URL" message instead.
        //   "host:8080:9090" -> host "host:8080"
        //   "user@host:8080" -> host "user@host"  (userinfo is not supported)
        if (host_sv.find_first_of(":@") != std::string_view::npos) {
            return std::nullopt;
        }
    }

    if (host_sv.empty()) {
        return std::nullopt;
    }
    // No legal host contains a space or a control character; one that does is a
    // mangled argument, not a name worth a DNS lookup.
    for (const char c : host_sv) {
        if (static_cast<unsigned char>(c) <= ' ' || c == 0x7F) {
            return std::nullopt;
        }
    }
    return HostPort{std::string(host_sv), port, tls};
}

}  // namespace pulsedb::sdk
