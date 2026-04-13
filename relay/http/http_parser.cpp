#include "relay/http/http_parser.h"

#include <algorithm>
#include <charconv>

namespace chromatindb::relay::http {

bool parse_request_line(std::string_view line, HttpRequest& req) {
    // Format: "METHOD /path?query HTTP/1.x"
    // Find first space -> method
    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0) {
        return false;  // No method or empty method
    }

    req.method = std::string(line.substr(0, sp1));

    // Find last space -> version must follow
    auto sp2 = line.rfind(' ');
    if (sp2 == sp1) {
        return false;  // No HTTP version (only one space)
    }

    auto version_sv = line.substr(sp2 + 1);
    if (version_sv.substr(0, 5) != "HTTP/") {
        return false;  // Not a valid HTTP version
    }
    req.version = std::string(version_sv);

    // URI is between sp1 and sp2
    auto uri = line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Split URI into path and query at '?'
    auto qmark = uri.find('?');
    if (qmark != std::string_view::npos) {
        req.path = std::string(uri.substr(0, qmark));
        req.query = std::string(uri.substr(qmark + 1));
    } else {
        req.path = std::string(uri);
        req.query.clear();
    }

    return true;
}

bool parse_header_line(std::string_view line, HttpRequest& req) {
    // Format: "Key: Value"
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }

    // Key: lowercase
    std::string key(line.substr(0, colon));
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Value: trim leading/trailing whitespace
    auto val_start = colon + 1;
    // Skip leading whitespace
    while (val_start < line.size() && line[val_start] == ' ') {
        ++val_start;
    }
    auto val_sv = line.substr(val_start);
    // Trim trailing whitespace
    while (!val_sv.empty() && val_sv.back() == ' ') {
        val_sv.remove_suffix(1);
    }
    std::string value(val_sv);

    req.headers[key] = value;

    // Special handling: Content-Length
    if (key == "content-length") {
        size_t len = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), len);
        if (ec == std::errc()) {
            req.content_length = len;
        }
    }

    return true;
}

bool finalize_headers(HttpRequest& req) {
    // Default keep_alive based on HTTP version
    if (req.version == "HTTP/1.0") {
        req.keep_alive = false;
    } else {
        req.keep_alive = true;  // HTTP/1.1 default
    }

    // Connection header overrides
    auto it = req.headers.find("connection");
    if (it != req.headers.end()) {
        // Lowercase the connection value for comparison
        std::string conn_val = it->second;
        std::transform(conn_val.begin(), conn_val.end(), conn_val.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (conn_val == "close") {
            req.keep_alive = false;
        } else if (conn_val == "keep-alive") {
            req.keep_alive = true;
        }
    }

    return true;
}

} // namespace chromatindb::relay::http
