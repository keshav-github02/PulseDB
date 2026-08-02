#include "pulsedb/sdk/url.hpp"

#include <gtest/gtest.h>

namespace {

using pulsedb::sdk::parse_http_url;

TEST(ParseHttpUrlTest, HostAndPortWithScheme) {
    const auto hp = parse_http_url("http://127.0.0.1:8080");
    ASSERT_TRUE(hp.has_value());
    EXPECT_EQ(hp->host, "127.0.0.1");
    EXPECT_EQ(hp->port, 8080);
}

TEST(ParseHttpUrlTest, HostAndPortWithoutScheme) {
    const auto hp = parse_http_url("localhost:9090");
    ASSERT_TRUE(hp.has_value());
    EXPECT_EQ(hp->host, "localhost");
    EXPECT_EQ(hp->port, 9090);
}

TEST(ParseHttpUrlTest, DefaultsToPort80ForHttp) {
    const auto hp = parse_http_url("http://example.com");
    ASSERT_TRUE(hp.has_value());
    EXPECT_EQ(hp->host, "example.com");
    EXPECT_EQ(hp->port, 80);
}

TEST(ParseHttpUrlTest, DefaultsToPort443ForHttps) {
    const auto hp = parse_http_url("https://example.com");
    ASSERT_TRUE(hp.has_value());
    EXPECT_EQ(hp->port, 443);
}

// Regression (CB-7): the https scheme used to be parsed and then forgotten, so
// an "https://" URL routed to the plaintext HttpEventSink and shipped telemetry
// in the clear to port 443 -- an operator who asked for encryption silently got
// none. The parser now reports the scheme so callers can refuse.
TEST(ParseHttpUrlTest, ReportsWhetherTheUrlAskedForTls) {
    const auto secure = parse_http_url("https://example.com:8443/v1/events");
    ASSERT_TRUE(secure.has_value());
    EXPECT_TRUE(secure->tls);
    EXPECT_EQ(secure->host, "example.com");
    EXPECT_EQ(secure->port, 8443);

    for (const char* plaintext : {"http://example.com", "example.com:8080", "example.com"}) {
        const auto hp = parse_http_url(plaintext);
        ASSERT_TRUE(hp.has_value()) << plaintext;
        EXPECT_FALSE(hp->tls) << plaintext;
    }
}

TEST(ParseHttpUrlTest, IgnoresPath) {
    const auto hp = parse_http_url("http://host:8080/v1/events");
    ASSERT_TRUE(hp.has_value());
    EXPECT_EQ(hp->host, "host");
    EXPECT_EQ(hp->port, 8080);
}

TEST(ParseHttpUrlTest, RejectsEmpty) {
    EXPECT_FALSE(parse_http_url("").has_value());
    EXPECT_FALSE(parse_http_url("http://").has_value());
}

TEST(ParseHttpUrlTest, RejectsNonNumericOrOutOfRangePort) {
    EXPECT_FALSE(parse_http_url("http://host:abc").has_value());
    EXPECT_FALSE(parse_http_url("http://host:99999").has_value());
    EXPECT_FALSE(parse_http_url("http://host:0").has_value());
    EXPECT_FALSE(parse_http_url("http://host:").has_value());
}

// Only the last colon was treated as the port separator, so whatever preceded it
// became the host unexamined. These were accepted and handed to the HTTP client,
// which failed at connect time with an opaque transport error -- so a mistyped
// --url was indistinguishable from a collector that was down.
TEST(ParseHttpUrlTest, RejectsAHostThatIsStillStructured) {
    EXPECT_FALSE(parse_http_url("http://host:8080:9090").has_value())
        << "two ports is not a host named \"host:8080\"";
    EXPECT_FALSE(parse_http_url("http://user@host:8080").has_value())
        << "userinfo is unsupported, so it must not become part of the hostname";
    EXPECT_FALSE(parse_http_url("http://ho st:80").has_value())
        << "a space means a mangled argument";
    EXPECT_FALSE(parse_http_url("http://host\x01:80").has_value())
        << "control characters are not hostnames";
}

TEST(ParseHttpUrlTest, HandlesBracketedIpv6Literals) {
    // The brackets exist so the address's own colons are not read as a port
    // separator; rfind(':') alone split "[::1]" into host "[:" and port "1]".
    const auto with_port = parse_http_url("http://[::1]:8080");
    ASSERT_TRUE(with_port.has_value());
    EXPECT_EQ(with_port->host, "::1") << "brackets are wire syntax, not the host";
    EXPECT_EQ(with_port->port, 8080);

    const auto default_port = parse_http_url("http://[::1]");
    ASSERT_TRUE(default_port.has_value()) << "a bracketed literal need not carry a port";
    EXPECT_EQ(default_port->host, "::1");
    EXPECT_EQ(default_port->port, 80);

    const auto full = parse_http_url("http://[2001:db8::1]:9000/v1/events");
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->host, "2001:db8::1");
    EXPECT_EQ(full->port, 9000);

    EXPECT_FALSE(parse_http_url("http://[::1").has_value()) << "unclosed bracket";
    EXPECT_FALSE(parse_http_url("http://[]:80").has_value()) << "empty literal";
    EXPECT_FALSE(parse_http_url("http://[::1]8080").has_value())
        << "trailing text that is not \":port\"";
}

}  // namespace
