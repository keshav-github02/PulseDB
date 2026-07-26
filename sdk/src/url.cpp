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

    int port = default_port;
    std::string_view host_sv = s;

    if (const auto colon = s.rfind(':'); colon != std::string_view::npos) {
        host_sv = s.substr(0, colon);
        const std::string_view port_sv = s.substr(colon + 1);
        if (port_sv.empty()) {
            return std::nullopt;
        }
        int value = 0;
        for (const char c : port_sv) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
            if (value > 65535) {
                return std::nullopt;
            }
        }
        if (value == 0) {
            return std::nullopt;
        }
        port = value;
    }

    if (host_sv.empty()) {
        return std::nullopt;
    }
    return HostPort{std::string(host_sv), port, tls};
}

}  // namespace pulsedb::sdk
