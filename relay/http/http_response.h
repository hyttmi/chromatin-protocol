#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace chromatindb::relay::http {

struct HttpResponse {
    uint16_t status = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;  // ordered pairs
    std::vector<uint8_t> body;

    /// JSON response with auto Content-Type and Content-Length.
    static HttpResponse json(uint16_t status_code, const nlohmann::json& j) {
        HttpResponse resp;
        resp.status = status_code;
        resp.status_text = status_text_for(status_code);
        auto body_str = j.dump();
        resp.body.assign(body_str.begin(), body_str.end());
        resp.headers.emplace_back("Content-Type", "application/json");
        resp.headers.emplace_back("Content-Length", std::to_string(resp.body.size()));
        return resp;
    }

    /// Binary response with auto Content-Type and Content-Length.
    static HttpResponse binary(uint16_t status_code, std::vector<uint8_t> data) {
        HttpResponse resp;
        resp.status = status_code;
        resp.status_text = status_text_for(status_code);
        resp.body = std::move(data);
        resp.headers.emplace_back("Content-Type", "application/octet-stream");
        resp.headers.emplace_back("Content-Length", std::to_string(resp.body.size()));
        return resp;
    }

    /// JSON error response: {"error": code, "message": message}.
    static HttpResponse error(uint16_t status_code, const std::string& code,
                              const std::string& message = "") {
        nlohmann::json j;
        j["error"] = code;
        if (!message.empty()) {
            j["message"] = message;
        }
        return HttpResponse::json(status_code, j);
    }

    /// 404 Not Found with JSON body.
    static HttpResponse not_found() {
        return error(404, "not_found");
    }

    /// 204 No Content (empty body).
    static HttpResponse no_content() {
        HttpResponse resp;
        resp.status = 204;
        resp.status_text = status_text_for(204);
        return resp;
    }

    /// Serialize only the HTTP/1.1 status line + headers + trailing CRLF.
    /// Does NOT include the body. Used for scatter-gather writes where body
    /// is written separately via buffer sequence.
    std::string serialize_header() const {
        std::string result;
        result.reserve(256);
        result += "HTTP/1.1 ";
        result += std::to_string(status);
        result += ' ';
        result += status_text;
        result += "\r\n";
        for (const auto& [key, val] : headers) {
            result += key;
            result += ": ";
            result += val;
            result += "\r\n";
        }
        result += "\r\n";
        return result;
    }

    /// Serialize to full HTTP/1.1 response string.
    std::string serialize() const {
        std::string result = serialize_header();
        // Body
        result.append(reinterpret_cast<const char*>(body.data()), body.size());
        return result;
    }

    /// Public access to status text lookup (for streaming response builders).
    static const char* status_text_for_public(uint16_t code) {
        return status_text_for(code);
    }

private:
    /// Look up status text for a code.
    static const char* status_text_for(uint16_t code) {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 413: return "Payload Too Large";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default:  return "Unknown";
        }
    }
};

} // namespace chromatindb::relay::http
