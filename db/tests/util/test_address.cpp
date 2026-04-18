#include "db/util/address.h"

#include <catch2/catch_test_macros.hpp>

using chromatindb::util::is_valid_host_port;

TEST_CASE("is_valid_host_port accepts well-formed addresses", "[address]") {
    REQUIRE(is_valid_host_port("192.168.1.73:4200"));
    REQUIRE(is_valid_host_port("localhost:4200"));
    REQUIRE(is_valid_host_port("example.com:80"));
    REQUIRE(is_valid_host_port("10.0.0.1:1"));
    REQUIRE(is_valid_host_port("10.0.0.1:65535"));
}

TEST_CASE("is_valid_host_port rejects missing colon", "[address]") {
    REQUIRE_FALSE(is_valid_host_port(""));
    REQUIRE_FALSE(is_valid_host_port("192.168.1.73"));
    REQUIRE_FALSE(is_valid_host_port(
        "591782456857e3b01c8f0c1e6c6ce5f844c957e4b2cec27ed4ba945181ea7193"));
}

TEST_CASE("is_valid_host_port rejects empty host or port", "[address]") {
    REQUIRE_FALSE(is_valid_host_port(":4200"));
    REQUIRE_FALSE(is_valid_host_port("192.168.1.73:"));
    REQUIRE_FALSE(is_valid_host_port(":"));
}

TEST_CASE("is_valid_host_port rejects non-numeric port", "[address]") {
    REQUIRE_FALSE(is_valid_host_port("host:abc"));
    REQUIRE_FALSE(is_valid_host_port("host:4200a"));
    REQUIRE_FALSE(is_valid_host_port("host: 4200"));
}

TEST_CASE("is_valid_host_port rejects out-of-range port", "[address]") {
    REQUIRE_FALSE(is_valid_host_port("host:0"));
    REQUIRE_FALSE(is_valid_host_port("host:65536"));
    REQUIRE_FALSE(is_valid_host_port("host:99999"));
}

TEST_CASE("is_valid_host_port rejects whitespace in host", "[address]") {
    REQUIRE_FALSE(is_valid_host_port("bad host:4200"));
    REQUIRE_FALSE(is_valid_host_port("host\t:4200"));
}
