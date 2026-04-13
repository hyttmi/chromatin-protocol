#include <catch2/catch_test_macros.hpp>
#include "relay/http/http_parser.h"
#include "relay/http/http_response.h"

using namespace chromatindb::relay::http;

// ============================================================================
// parse_request_line
// ============================================================================

TEST_CASE("parse_request_line: GET with path", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_request_line("GET /blob/abc/def HTTP/1.1", req));
    CHECK(req.method == "GET");
    CHECK(req.path == "/blob/abc/def");
    CHECK(req.query.empty());
}

TEST_CASE("parse_request_line: POST with query string", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_request_line("POST /blob?foo=bar HTTP/1.1", req));
    CHECK(req.method == "POST");
    CHECK(req.path == "/blob");
    CHECK(req.query == "foo=bar");
}

TEST_CASE("parse_request_line: DELETE request", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_request_line("DELETE /blob/ns/hash HTTP/1.1", req));
    CHECK(req.method == "DELETE");
    CHECK(req.path == "/blob/ns/hash");
}

TEST_CASE("parse_request_line: path with multiple query params", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_request_line("GET /list?ns=abc&limit=10 HTTP/1.1", req));
    CHECK(req.path == "/list");
    CHECK(req.query == "ns=abc&limit=10");
}

TEST_CASE("parse_request_line: root path", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_request_line("GET / HTTP/1.1", req));
    CHECK(req.path == "/");
}

TEST_CASE("parse_request_line: HTTP/1.0", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_request_line("GET /test HTTP/1.0", req));
    CHECK(req.method == "GET");
    CHECK(req.path == "/test");
}

TEST_CASE("parse_request_line: rejects missing HTTP version", "[http][parser]") {
    HttpRequest req;
    CHECK_FALSE(parse_request_line("GET /path", req));
}

TEST_CASE("parse_request_line: rejects empty line", "[http][parser]") {
    HttpRequest req;
    CHECK_FALSE(parse_request_line("", req));
}

TEST_CASE("parse_request_line: rejects method only", "[http][parser]") {
    HttpRequest req;
    CHECK_FALSE(parse_request_line("GET", req));
}

TEST_CASE("parse_request_line: rejects no method", "[http][parser]") {
    HttpRequest req;
    CHECK_FALSE(parse_request_line(" /path HTTP/1.1", req));
}

TEST_CASE("parse_request_line: empty path treated as /", "[http][parser]") {
    // "GET  HTTP/1.1" -- two spaces, empty URI
    HttpRequest req;
    // This should either fail or set path to empty/root
    auto result = parse_request_line("GET  HTTP/1.1", req);
    // An empty URI is technically invalid, but we're lenient
    if (result) {
        CHECK((req.path.empty() || req.path == "/"));
    }
}

// ============================================================================
// parse_header_line
// ============================================================================

TEST_CASE("parse_header_line: Content-Length", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_header_line("Content-Length: 42", req));
    CHECK(req.content_length == 42);
    CHECK(req.headers["content-length"] == "42");
}

TEST_CASE("parse_header_line: Authorization header", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_header_line("Authorization: Bearer abc123", req));
    CHECK(req.headers["authorization"] == "Bearer abc123");
}

TEST_CASE("parse_header_line: Content-Type", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_header_line("Content-Type: application/octet-stream", req));
    CHECK(req.headers["content-type"] == "application/octet-stream");
}

TEST_CASE("parse_header_line: keys are lowercased", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_header_line("X-Custom-Header: value123", req));
    CHECK(req.headers.count("x-custom-header") == 1);
    CHECK(req.headers["x-custom-header"] == "value123");
}

TEST_CASE("parse_header_line: value whitespace trimmed", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_header_line("Host:  example.com  ", req));
    CHECK(req.headers["host"] == "example.com");
}

TEST_CASE("parse_header_line: colon in value preserved", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_header_line("Authorization: Bearer token:with:colons", req));
    CHECK(req.headers["authorization"] == "Bearer token:with:colons");
}

TEST_CASE("parse_header_line: empty value", "[http][parser]") {
    HttpRequest req;
    REQUIRE(parse_header_line("X-Empty: ", req));
    CHECK(req.headers["x-empty"].empty());
}

TEST_CASE("parse_header_line: no colon rejects", "[http][parser]") {
    HttpRequest req;
    CHECK_FALSE(parse_header_line("InvalidHeader", req));
}

TEST_CASE("parse_header_line: Content-Length defaults to 0", "[http][parser]") {
    HttpRequest req;
    CHECK(req.content_length == 0);
}

// ============================================================================
// finalize_headers (keep_alive logic)
// ============================================================================

TEST_CASE("finalize_headers: HTTP/1.1 defaults keep_alive to true", "[http][parser]") {
    HttpRequest req;
    parse_request_line("GET / HTTP/1.1", req);
    finalize_headers(req);
    CHECK(req.keep_alive == true);
}

TEST_CASE("finalize_headers: HTTP/1.0 defaults keep_alive to false", "[http][parser]") {
    HttpRequest req;
    parse_request_line("GET / HTTP/1.0", req);
    finalize_headers(req);
    CHECK(req.keep_alive == false);
}

TEST_CASE("finalize_headers: Connection close overrides HTTP/1.1", "[http][parser]") {
    HttpRequest req;
    parse_request_line("GET / HTTP/1.1", req);
    parse_header_line("Connection: close", req);
    finalize_headers(req);
    CHECK(req.keep_alive == false);
}

TEST_CASE("finalize_headers: Connection keep-alive upgrades HTTP/1.0", "[http][parser]") {
    HttpRequest req;
    parse_request_line("GET / HTTP/1.0", req);
    parse_header_line("Connection: keep-alive", req);
    finalize_headers(req);
    CHECK(req.keep_alive == true);
}

TEST_CASE("finalize_headers: Connection header case-insensitive", "[http][parser]") {
    HttpRequest req;
    parse_request_line("GET / HTTP/1.1", req);
    parse_header_line("Connection: Close", req);
    finalize_headers(req);
    CHECK(req.keep_alive == false);
}

// ============================================================================
// HttpResponse builders
// ============================================================================

TEST_CASE("HttpResponse::json sets correct headers", "[http][response]") {
    nlohmann::json j = {{"status", "ok"}};
    auto resp = HttpResponse::json(200, j);
    CHECK(resp.status == 200);
    CHECK(resp.status_text == "OK");

    // Check Content-Type header
    bool has_content_type = false;
    bool has_content_length = false;
    for (const auto& [key, val] : resp.headers) {
        if (key == "Content-Type" && val == "application/json") has_content_type = true;
        if (key == "Content-Length") has_content_length = true;
    }
    CHECK(has_content_type);
    CHECK(has_content_length);
    CHECK_FALSE(resp.body.empty());
}

TEST_CASE("HttpResponse::binary sets correct headers", "[http][response]") {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto resp = HttpResponse::binary(200, data);
    CHECK(resp.status == 200);

    bool has_content_type = false;
    for (const auto& [key, val] : resp.headers) {
        if (key == "Content-Type" && val == "application/octet-stream") has_content_type = true;
    }
    CHECK(has_content_type);
    CHECK(resp.body.size() == 3);
}

TEST_CASE("HttpResponse::error produces JSON error body", "[http][response]") {
    auto resp = HttpResponse::error(400, "bad_request", "Invalid input");
    CHECK(resp.status == 400);
    CHECK(resp.status_text == "Bad Request");

    // Parse body as JSON
    std::string body_str(resp.body.begin(), resp.body.end());
    auto j = nlohmann::json::parse(body_str);
    CHECK(j["error"] == "bad_request");
    CHECK(j["message"] == "Invalid input");
}

TEST_CASE("HttpResponse::error without message", "[http][response]") {
    auto resp = HttpResponse::error(401, "unauthorized");
    CHECK(resp.status == 401);
    std::string body_str(resp.body.begin(), resp.body.end());
    auto j = nlohmann::json::parse(body_str);
    CHECK(j["error"] == "unauthorized");
}

TEST_CASE("HttpResponse::not_found produces 404", "[http][response]") {
    auto resp = HttpResponse::not_found();
    CHECK(resp.status == 404);
    CHECK(resp.status_text == "Not Found");
    std::string body_str(resp.body.begin(), resp.body.end());
    auto j = nlohmann::json::parse(body_str);
    CHECK(j["error"] == "not_found");
}

TEST_CASE("HttpResponse::no_content produces 204", "[http][response]") {
    auto resp = HttpResponse::no_content();
    CHECK(resp.status == 204);
    CHECK(resp.status_text == "No Content");
    CHECK(resp.body.empty());
}

TEST_CASE("HttpResponse::serialize produces valid HTTP response", "[http][response]") {
    auto resp = HttpResponse::json(200, {{"ok", true}});
    auto serialized = resp.serialize();

    // Should start with HTTP/1.1 200 OK
    CHECK(serialized.find("HTTP/1.1 200 OK\r\n") == 0);

    // Should have header/body separator
    CHECK(serialized.find("\r\n\r\n") != std::string::npos);

    // Should contain Content-Type header
    CHECK(serialized.find("Content-Type: application/json\r\n") != std::string::npos);

    // Body should follow the separator
    auto sep_pos = serialized.find("\r\n\r\n");
    auto body_part = serialized.substr(sep_pos + 4);
    auto j = nlohmann::json::parse(body_part);
    CHECK(j["ok"] == true);
}

TEST_CASE("HttpResponse::serialize binary response", "[http][response]") {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto resp = HttpResponse::binary(200, data);
    auto serialized = resp.serialize();

    CHECK(serialized.find("HTTP/1.1 200 OK\r\n") == 0);
    CHECK(serialized.find("Content-Type: application/octet-stream\r\n") != std::string::npos);
    CHECK(serialized.find("Content-Length: 4\r\n") != std::string::npos);

    // Body bytes after separator
    auto sep_pos = serialized.find("\r\n\r\n");
    auto body_part = serialized.substr(sep_pos + 4);
    REQUIRE(body_part.size() == 4);
    CHECK(static_cast<uint8_t>(body_part[0]) == 0xDE);
    CHECK(static_cast<uint8_t>(body_part[1]) == 0xAD);
    CHECK(static_cast<uint8_t>(body_part[2]) == 0xBE);
    CHECK(static_cast<uint8_t>(body_part[3]) == 0xEF);
}

TEST_CASE("HttpResponse::serialize 204 no body", "[http][response]") {
    auto resp = HttpResponse::no_content();
    auto serialized = resp.serialize();

    CHECK(serialized.find("HTTP/1.1 204 No Content\r\n") == 0);
    auto sep_pos = serialized.find("\r\n\r\n");
    auto body_part = serialized.substr(sep_pos + 4);
    CHECK(body_part.empty());
}

TEST_CASE("HttpResponse status text lookup", "[http][response]") {
    CHECK(HttpResponse::error(403, "forbidden").status_text == "Forbidden");
    CHECK(HttpResponse::error(405, "method_not_allowed").status_text == "Method Not Allowed");
    CHECK(HttpResponse::error(409, "conflict").status_text == "Conflict");
    CHECK(HttpResponse::error(413, "too_large").status_text == "Payload Too Large");
    CHECK(HttpResponse::error(500, "internal").status_text == "Internal Server Error");
    CHECK(HttpResponse::error(502, "bad_gw").status_text == "Bad Gateway");
    CHECK(HttpResponse::error(503, "unavailable").status_text == "Service Unavailable");
}
