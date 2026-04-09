#include <catch2/catch_test_macros.hpp>

#include "relay/ws/ws_handshake.h"

#include <string>

using namespace chromatindb::relay::ws;

// ============================================================================
// compute_accept_key tests
// ============================================================================

TEST_CASE("WS Handshake: compute_accept_key matches RFC 6455 example", "[ws_handshake]") {
    // RFC 6455 Section 4.2.2 example
    auto key = compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
    REQUIRE(key == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// ============================================================================
// parse_upgrade_request tests
// ============================================================================

TEST_CASE("WS Handshake: valid upgrade request with all required headers", "[ws_handshake]") {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    REQUIRE(result.is_upgrade == true);
    REQUIRE(result.request.has_value());
    REQUIRE(result.request->websocket_key == "dGhlIHNhbXBsZSBub25jZQ==");
    REQUIRE(result.request->path == "/");
}

TEST_CASE("WS Handshake: missing Upgrade header fails", "[ws_handshake]") {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    // Missing Upgrade header means it's not an upgrade request
    REQUIRE(result.is_upgrade == false);
}

TEST_CASE("WS Handshake: missing Sec-WebSocket-Key fails", "[ws_handshake]") {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    REQUIRE(result.is_upgrade == true);
    REQUIRE_FALSE(result.request.has_value());
    REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("WS Handshake: missing Sec-WebSocket-Version fails", "[ws_handshake]") {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    REQUIRE(result.is_upgrade == true);
    REQUIRE_FALSE(result.request.has_value());
    REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("WS Handshake: non-root path is accepted", "[ws_handshake]") {
    std::string req =
        "GET /other-path HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    REQUIRE(result.is_upgrade == true);
    REQUIRE(result.request.has_value());
    REQUIRE(result.request->path == "/other-path");
}

TEST_CASE("WS Handshake: POST method is not an upgrade", "[ws_handshake]") {
    std::string req =
        "POST / HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    REQUIRE(result.is_upgrade == false);
}

TEST_CASE("WS Handshake: case-insensitive header matching", "[ws_handshake]") {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "upgrade: WebSocket\r\n"
        "connection: Upgrade\r\n"
        "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "sec-websocket-version: 13\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    REQUIRE(result.is_upgrade == true);
    REQUIRE(result.request.has_value());
    REQUIRE(result.request->websocket_key == "dGhlIHNhbXBsZSBub25jZQ==");
}

// ============================================================================
// Response builder tests
// ============================================================================

TEST_CASE("WS Handshake: build_upgrade_response contains 101 Switching Protocols", "[ws_handshake]") {
    auto resp = build_upgrade_response("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    REQUIRE(resp.find("HTTP/1.1 101 Switching Protocols") != std::string::npos);
    REQUIRE(resp.find("Upgrade: websocket") != std::string::npos);
    REQUIRE(resp.find("Connection: Upgrade") != std::string::npos);
    REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    REQUIRE(resp.find("\r\n\r\n") != std::string::npos);
}

TEST_CASE("WS Handshake: build_426_response contains 426 Upgrade Required", "[ws_handshake]") {
    auto resp = build_426_response();
    REQUIRE(resp.find("HTTP/1.1 426 Upgrade Required") != std::string::npos);
    REQUIRE(resp.find("Upgrade: websocket") != std::string::npos);
}

TEST_CASE("WS Handshake: missing Connection header is not an upgrade", "[ws_handshake]") {
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost:4201\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    auto result = parse_upgrade_request(req);
    // Connection: Upgrade is required for a valid upgrade
    REQUIRE(result.is_upgrade == false);
}
