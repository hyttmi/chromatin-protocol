#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace chromatindb::relay::ws {

/// Parsed HTTP upgrade request data.
struct UpgradeRequest {
    std::string websocket_key;  // Sec-WebSocket-Key value
    std::string path;           // Request path (e.g., "/")
};

/// Result of parsing an HTTP request for WebSocket upgrade.
struct ParseResult {
    bool is_upgrade;                           // True if this is an upgrade attempt
    std::optional<UpgradeRequest> request;     // Present only if is_upgrade && valid
    std::string error;                         // Reason if is_upgrade but invalid
};

/// Parse raw HTTP upgrade request bytes.
/// Validates: GET method, HTTP/1.1, Upgrade: websocket (case-insensitive),
/// Connection: Upgrade (case-insensitive), Sec-WebSocket-Version: 13,
/// Sec-WebSocket-Key present.
/// Per D-11: silently ignores Sec-WebSocket-Extensions.
/// Per D-12: sets is_upgrade=false if not an upgrade request (caller sends 426).
ParseResult parse_upgrade_request(std::string_view raw_request);

/// Compute the Sec-WebSocket-Accept value per RFC 6455 Section 4.2.2.
/// SHA-1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") then Base64 encode.
std::string compute_accept_key(std::string_view client_key);

/// Build the HTTP 101 Switching Protocols response.
std::string build_upgrade_response(const std::string& accept_key);

/// Build the HTTP 426 Upgrade Required response.
std::string build_426_response();

} // namespace chromatindb::relay::ws
