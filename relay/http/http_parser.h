#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace chromatindb::relay::http {

struct HttpRequest {
    std::string method;       // "GET", "POST", "DELETE"
    std::string path;         // "/blob/abc/def" (no query string)
    std::string query;        // "foo=bar" (after '?', empty if none)
    std::unordered_map<std::string, std::string> headers;  // lowercase keys
    size_t content_length = 0;
    bool keep_alive = true;   // HTTP/1.1 default

    // HTTP version stored for finalize_headers logic
    std::string version;      // "HTTP/1.1" or "HTTP/1.0"
};

/// Parse "METHOD /path?query HTTP/1.1" into req.
/// Returns false on malformed input.
bool parse_request_line(std::string_view line, HttpRequest& req);

/// Parse "Key: Value" header line into req.
/// Key is lowercased. Handles Content-Length and Connection specially.
/// Returns false if no colon found.
bool parse_header_line(std::string_view line, HttpRequest& req);

/// Set keep_alive based on Connection header + HTTP version.
/// Call after all headers parsed.
bool finalize_headers(HttpRequest& req);

} // namespace chromatindb::relay::http
