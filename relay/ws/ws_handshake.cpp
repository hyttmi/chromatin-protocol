#include "relay/ws/ws_handshake.h"

#include <openssl/evp.h>
#include <openssl/buffer.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace chromatindb::relay::ws {

namespace {

/// RFC 6455 magic GUID for Sec-WebSocket-Accept computation.
constexpr const char* WS_MAGIC_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// Convert a string to lowercase in-place.
std::string to_lower(std::string_view sv) {
    std::string result(sv);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

/// Check if a header value contains a substring (case-insensitive).
bool contains_ci(std::string_view haystack, std::string_view needle) {
    auto lower_haystack = to_lower(haystack);
    auto lower_needle = to_lower(needle);
    return lower_haystack.find(lower_needle) != std::string::npos;
}

/// Trim leading and trailing whitespace.
std::string_view trim(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

std::string compute_accept_key(std::string_view client_key) {
    // Concatenate client key + magic GUID
    std::string input(client_key);
    input += WS_MAGIC_GUID;

    // SHA-1 hash using OpenSSL EVP API
    unsigned char sha1_hash[20];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, sha1_hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    // Base64 encode using OpenSSL
    // EVP_EncodeBlock writes ceil(n/3)*4 bytes + NUL terminator
    char base64_buf[((20 + 2) / 3) * 4 + 1];
    int base64_len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(base64_buf),
        sha1_hash, static_cast<int>(hash_len));

    return std::string(base64_buf, static_cast<size_t>(base64_len));
}

ParseResult parse_upgrade_request(std::string_view raw_request) {
    // Find end of first line
    auto line_end = raw_request.find("\r\n");
    if (line_end == std::string_view::npos) {
        return {false, std::nullopt, "incomplete request line"};
    }

    std::string_view request_line = raw_request.substr(0, line_end);

    // Parse request line: "GET /path HTTP/1.1"
    // Must start with GET
    if (request_line.substr(0, 4) != "GET ") {
        return {false, std::nullopt, ""};  // Not a GET request -> not an upgrade
    }

    // Extract path
    auto path_start = 4;  // After "GET "
    auto path_end = request_line.find(' ', path_start);
    if (path_end == std::string_view::npos) {
        return {false, std::nullopt, "malformed request line"};
    }
    std::string path(request_line.substr(path_start, path_end - path_start));

    // Verify HTTP/1.1
    auto version = request_line.substr(path_end + 1);
    if (version != "HTTP/1.1") {
        return {false, std::nullopt, "not HTTP/1.1"};
    }

    // Parse headers
    std::string upgrade_value;
    std::string connection_value;
    std::string ws_key;
    std::string ws_version;

    size_t pos = line_end + 2;  // Skip first \r\n
    while (pos < raw_request.size()) {
        auto next_end = raw_request.find("\r\n", pos);
        if (next_end == std::string_view::npos) break;

        if (next_end == pos) break;  // Empty line = end of headers

        std::string_view header_line = raw_request.substr(pos, next_end - pos);

        // Split on first ": "
        auto colon = header_line.find(':');
        if (colon != std::string_view::npos) {
            auto name = to_lower(trim(header_line.substr(0, colon)));
            auto value = trim(header_line.substr(colon + 1));

            if (name == "upgrade") {
                upgrade_value = std::string(value);
            } else if (name == "connection") {
                connection_value = std::string(value);
            } else if (name == "sec-websocket-key") {
                ws_key = std::string(value);
            } else if (name == "sec-websocket-version") {
                ws_version = std::string(trim(value));
            }
        }

        pos = next_end + 2;
    }

    // Check if this is an upgrade request at all
    bool has_upgrade = contains_ci(upgrade_value, "websocket");
    bool has_connection = contains_ci(connection_value, "upgrade");

    if (!has_upgrade || !has_connection) {
        return {false, std::nullopt, ""};
    }

    // It IS an upgrade request -- now validate the required fields
    if (ws_key.empty()) {
        return {true, std::nullopt, "missing Sec-WebSocket-Key"};
    }

    if (ws_version != "13") {
        return {true, std::nullopt, "unsupported Sec-WebSocket-Version (expected 13)"};
    }

    return {true, UpgradeRequest{std::move(ws_key), std::move(path)}, ""};
}

std::string build_upgrade_response(const std::string& accept_key) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept_key + "\r\n"
           "\r\n";
}

std::string build_426_response() {
    return "HTTP/1.1 426 Upgrade Required\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Content-Length: 0\r\n"
           "\r\n";
}

} // namespace chromatindb::relay::ws
