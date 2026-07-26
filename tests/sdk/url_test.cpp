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

}  // namespace
