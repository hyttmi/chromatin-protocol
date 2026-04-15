#include <catch2/catch_test_macros.hpp>

#include "relay/http/http_response.h"

using namespace chromatindb::relay::http;

TEST_CASE("http_response: serialize_header produces headers only", "[http_response]") {
    auto resp = HttpResponse::json(200, {{"ok", true}});

    auto header = resp.serialize_header();

    // Starts with status line
    REQUIRE(header.substr(0, 15) == "HTTP/1.1 200 OK");

    // Contains Content-Type header
    REQUIRE(header.find("Content-Type: application/json\r\n") != std::string::npos);

    // Contains Content-Length header
    REQUIRE(header.find("Content-Length:") != std::string::npos);

    // Ends with double CRLF (header-body separator)
    REQUIRE(header.size() >= 4);
    REQUIRE(header.substr(header.size() - 4) == "\r\n\r\n");

    // Does NOT contain the JSON body
    auto body_str = std::string(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
    REQUIRE(body_str.find("ok") != std::string::npos);  // Body has content
    // The header string should not contain the body content after the double CRLF
    auto after_headers = header.find("\r\n\r\n");
    REQUIRE(after_headers != std::string::npos);
    // Everything after \r\n\r\n should be empty (nothing follows the header separator)
    REQUIRE(after_headers + 4 == header.size());
}

TEST_CASE("http_response: serialize_header matches serialize prefix", "[http_response]") {
    auto resp = HttpResponse::json(200, {{"key", "value"}});

    auto header = resp.serialize_header();
    auto full = resp.serialize();

    // The full serialized response should start with the header
    REQUIRE(full.substr(0, header.size()) == header);

    // The remainder after the header should be the body
    auto remainder = full.substr(header.size());
    auto body_str = std::string(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
    REQUIRE(remainder == body_str);
}

TEST_CASE("http_response: scatter-gather produces same output as serialize", "[http_response]") {
    auto resp = HttpResponse::json(201, {{"id", 42}, {"name", "test"}});

    auto header = resp.serialize_header();
    auto full_serialize = resp.serialize();

    // Manually concatenate header + body (simulating scatter-gather)
    std::string scatter_gather = header;
    scatter_gather.append(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());

    REQUIRE(scatter_gather == full_serialize);
}

TEST_CASE("http_response: binary scatter-gather produces same output", "[http_response]") {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    auto resp = HttpResponse::binary(200, data);

    auto header = resp.serialize_header();
    auto full_serialize = resp.serialize();

    // Scatter-gather reconstruction
    std::string scatter_gather = header;
    scatter_gather.append(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());

    REQUIRE(scatter_gather == full_serialize);
    REQUIRE(header.find("Content-Type: application/octet-stream\r\n") != std::string::npos);
}

TEST_CASE("http_response: no_content serialize_header has no body", "[http_response]") {
    auto resp = HttpResponse::no_content();

    auto header = resp.serialize_header();

    // Status line should be 204
    REQUIRE(header.find("HTTP/1.1 204 No Content\r\n") != std::string::npos);

    // Ends with double CRLF
    REQUIRE(header.substr(header.size() - 4) == "\r\n\r\n");

    // Body should be empty
    REQUIRE(resp.body.empty());

    // Full serialize should be identical to header-only (no body to append)
    REQUIRE(resp.serialize() == header);
}

TEST_CASE("http_response: error response serialize_header correct status", "[http_response]") {
    auto resp = HttpResponse::error(404, "not_found", "resource not found");

    auto header = resp.serialize_header();

    REQUIRE(header.find("HTTP/1.1 404 Not Found\r\n") != std::string::npos);
    REQUIRE(header.find("Content-Type: application/json\r\n") != std::string::npos);

    // Body should contain the error JSON
    auto body_str = std::string(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
    auto j = nlohmann::json::parse(body_str);
    REQUIRE(j["error"] == "not_found");
    REQUIRE(j["message"] == "resource not found");
}

TEST_CASE("http_response: status_text_for_public returns correct texts", "[http_response]") {
    REQUIRE(std::string(HttpResponse::status_text_for_public(200)) == "OK");
    REQUIRE(std::string(HttpResponse::status_text_for_public(201)) == "Created");
    REQUIRE(std::string(HttpResponse::status_text_for_public(204)) == "No Content");
    REQUIRE(std::string(HttpResponse::status_text_for_public(400)) == "Bad Request");
    REQUIRE(std::string(HttpResponse::status_text_for_public(413)) == "Payload Too Large");
    REQUIRE(std::string(HttpResponse::status_text_for_public(500)) == "Internal Server Error");
    REQUIRE(std::string(HttpResponse::status_text_for_public(999)) == "Unknown");
}
