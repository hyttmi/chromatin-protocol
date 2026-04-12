// relay_test_helpers.h -- shared test infrastructure for relay E2E tests
//
// Header-only helpers used by both relay_smoke_test and relay_feature_test.
// All functions are inline to avoid ODR violations across translation units.

#pragma once

#include "relay/identity/relay_identity.h"
#include "relay/util/base64.h"
#include "relay/util/hex.h"
#include "relay/ws/ws_frame.h"
#include "relay/ws/ws_handshake.h"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <oqs/sha3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace relay_test {

namespace ws = chromatindb::relay::ws;
namespace identity = chromatindb::relay::identity;
namespace util = chromatindb::relay::util;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// TCP helpers (blocking)
// ---------------------------------------------------------------------------

inline bool send_all(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, p + sent, len - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline std::string recv_until(int fd, const std::string& delim, size_t max_bytes = 8192) {
    std::string buf;
    buf.reserve(max_bytes);
    while (buf.size() < max_bytes) {
        char c;
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) break;
        buf += c;
        if (buf.size() >= delim.size() &&
            buf.compare(buf.size() - delim.size(), delim.size(), delim) == 0) {
            break;
        }
    }
    return buf;
}

inline bool recv_all(int fd, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::read(fd, buf + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// WebSocket frame send/receive helpers (client-side, masked)
// ---------------------------------------------------------------------------

inline bool ws_send_text(int fd, const std::string& text) {
    // Client frames MUST be masked (RFC 6455 Section 5.1)
    std::vector<uint8_t> payload(text.begin(), text.end());

    // Generate random mask key
    uint8_t mask[4];
    RAND_bytes(mask, 4);

    // Build frame header
    std::vector<uint8_t> frame;

    // Byte 0: FIN + OPCODE_TEXT
    frame.push_back(0x81);  // FIN=1, opcode=0x01 (text)

    // Byte 1+: masked payload length
    if (payload.size() <= 125) {
        frame.push_back(static_cast<uint8_t>(0x80 | payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    // Mask key
    frame.insert(frame.end(), mask, mask + 4);

    // Masked payload
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(payload[i] ^ mask[i % 4]);
    }

    return send_all(fd, frame.data(), frame.size());
}

struct WsFrame {
    uint8_t opcode;
    std::string payload;
    uint16_t close_code = 0;  // Only set for OPCODE_CLOSE frames
};

inline std::optional<WsFrame> ws_recv_frame(int fd) {
    uint8_t hdr[2];
    if (!recv_all(fd, hdr, 2)) return std::nullopt;

    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (!recv_all(fd, ext, 2)) return std::nullopt;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (!recv_all(fd, ext, 8)) return std::nullopt;
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask_key[4] = {};
    if (masked) {
        if (!recv_all(fd, mask_key, 4)) return std::nullopt;
    }

    std::vector<uint8_t> payload(static_cast<size_t>(payload_len));
    if (payload_len > 0) {
        if (!recv_all(fd, payload.data(), static_cast<size_t>(payload_len)))
            return std::nullopt;
    }

    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask_key[i % 4];
    }

    // Handle PING: respond with PONG and recurse
    if (opcode == ws::OPCODE_PING) {
        std::vector<uint8_t> pong_frame;
        pong_frame.push_back(0x80 | ws::OPCODE_PONG);
        uint8_t pong_mask[4];
        RAND_bytes(pong_mask, 4);
        if (payload.size() <= 125) {
            pong_frame.push_back(static_cast<uint8_t>(0x80 | payload.size()));
        }
        pong_frame.insert(pong_frame.end(), pong_mask, pong_mask + 4);
        for (size_t i = 0; i < payload.size(); ++i)
            pong_frame.push_back(payload[i] ^ pong_mask[i % 4]);
        send_all(fd, pong_frame.data(), pong_frame.size());
        return ws_recv_frame(fd);
    }

    if (opcode == ws::OPCODE_CLOSE) return std::nullopt;

    return WsFrame{opcode, std::string(payload.begin(), payload.end())};
}

// ws_recv_frame_raw -- like ws_recv_frame but does NOT swallow close frames.
// On OPCODE_CLOSE: parses the close code from the first 2 bytes of the payload
// (big-endian uint16) and returns the frame with close_code set.
// Required for SIGTERM test (E2E-05) which must verify close code 1001.
inline std::optional<WsFrame> ws_recv_frame_raw(int fd) {
    uint8_t hdr[2];
    if (!recv_all(fd, hdr, 2)) return std::nullopt;

    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (!recv_all(fd, ext, 2)) return std::nullopt;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (!recv_all(fd, ext, 8)) return std::nullopt;
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask_key[4] = {};
    if (masked) {
        if (!recv_all(fd, mask_key, 4)) return std::nullopt;
    }

    std::vector<uint8_t> payload(static_cast<size_t>(payload_len));
    if (payload_len > 0) {
        if (!recv_all(fd, payload.data(), static_cast<size_t>(payload_len)))
            return std::nullopt;
    }

    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask_key[i % 4];
    }

    // Handle PING: respond with PONG and recurse
    if (opcode == ws::OPCODE_PING) {
        std::vector<uint8_t> pong_frame;
        pong_frame.push_back(0x80 | ws::OPCODE_PONG);
        uint8_t pong_mask[4];
        RAND_bytes(pong_mask, 4);
        if (payload.size() <= 125) {
            pong_frame.push_back(static_cast<uint8_t>(0x80 | payload.size()));
        }
        pong_frame.insert(pong_frame.end(), pong_mask, pong_mask + 4);
        for (size_t i = 0; i < payload.size(); ++i)
            pong_frame.push_back(payload[i] ^ pong_mask[i % 4]);
        send_all(fd, pong_frame.data(), pong_frame.size());
        return ws_recv_frame_raw(fd);
    }

    // For close frames: parse close code and return frame (do NOT swallow)
    if (opcode == ws::OPCODE_CLOSE) {
        uint16_t close_code = 0;
        if (payload.size() >= 2) {
            close_code = (static_cast<uint16_t>(payload[0]) << 8) |
                         static_cast<uint16_t>(payload[1]);
        }
        return WsFrame{opcode, std::string(payload.begin(), payload.end()), close_code};
    }

    return WsFrame{opcode, std::string(payload.begin(), payload.end())};
}

inline std::optional<std::string> ws_recv_text(int fd) {
    auto frame = ws_recv_frame(fd);
    if (!frame) return std::nullopt;
    return frame->payload;
}

// ---------------------------------------------------------------------------
// Blob signing helpers (ML-DSA-87 over SHA3-256 digest)
// ---------------------------------------------------------------------------

inline std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl, uint64_t timestamp)
{
    std::array<uint8_t, 32> digest{};
    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);
    OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), namespace_id.size());
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());
    // Big-endian for ttl and timestamp
    uint8_t ttl_be[4];
    for (int i = 0; i < 4; ++i) ttl_be[i] = static_cast<uint8_t>(ttl >> ((3 - i) * 8));
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_be, 4);
    uint8_t ts_be[8];
    for (int i = 0; i < 8; ++i) ts_be[i] = static_cast<uint8_t>(timestamp >> ((7 - i) * 8));
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ts_be, 8);
    OQS_SHA3_sha3_256_inc_finalize(digest.data(), &ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&ctx);
    return digest;
}

inline json make_data_message(const identity::RelayIdentity& id,
                               uint32_t request_id,
                               const std::vector<uint8_t>& test_data,
                               uint32_t ttl, uint64_t timestamp)
{
    auto ns = id.public_key_hash();
    auto pk = id.public_key();
    auto digest = build_signing_input(ns, test_data, ttl, timestamp);
    auto signature = id.sign(digest);
    return {
        {"type", "data"},
        {"request_id", request_id},
        {"namespace", util::to_hex(ns)},
        {"pubkey", util::to_hex(pk)},
        {"data", util::base64_encode(test_data)},
        {"ttl", ttl},
        {"timestamp", std::to_string(timestamp)},
        {"signature", util::base64_encode(
            std::span<const uint8_t>(signature.data(), signature.size()))}
    };
}

// ---------------------------------------------------------------------------
// Test result tracking
// ---------------------------------------------------------------------------

struct TestResult {
    std::string name;
    bool passed;
    std::string detail;
};

inline std::vector<TestResult> results;

inline void record(const std::string& name, bool passed, const std::string& detail = "") {
    results.push_back({name, passed, detail});
    std::cout << (passed ? "  PASS: " : "  FAIL: ") << name;
    if (!detail.empty()) std::cout << " -- " << detail;
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Connect + authenticate helper
// ---------------------------------------------------------------------------

// Returns an authenticated WebSocket fd, or -1 on failure.
// Performs: TCP connect, SO_RCVTIMEO(5s), WebSocket upgrade, ML-DSA-87 auth.
inline int connect_and_auth(const std::string& host, int port,
                            const identity::RelayIdentity& id) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    // Set socket read timeout to prevent hanging
    struct timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // --- WebSocket upgrade handshake ---
    uint8_t raw_key[16];
    RAND_bytes(raw_key, 16);
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ws_key;
    for (int i = 0; i < 16; i += 3) {
        int n = (raw_key[i] << 16);
        if (i + 1 < 16) n |= (raw_key[i + 1] << 8);
        if (i + 2 < 16) n |= raw_key[i + 2];
        ws_key += b64[(n >> 18) & 0x3F];
        ws_key += b64[(n >> 12) & 0x3F];
        ws_key += (i + 1 < 16) ? b64[(n >> 6) & 0x3F] : '=';
        ws_key += (i + 2 < 16) ? b64[n & 0x3F] : '=';
    }

    std::string upgrade_req =
        "GET / HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + ws_key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    if (!send_all(fd, upgrade_req.data(), upgrade_req.size())) {
        ::close(fd);
        return -1;
    }

    auto response = recv_until(fd, "\r\n\r\n");
    if (response.find("101 Switching Protocols") == std::string::npos) {
        ::close(fd);
        return -1;
    }

    // Validate Sec-WebSocket-Accept
    auto expected_accept = ws::compute_accept_key(ws_key);
    if (response.find(expected_accept) == std::string::npos) {
        ::close(fd);
        return -1;
    }

    // --- Auth challenge-response ---
    auto challenge_text = ws_recv_text(fd);
    if (!challenge_text) {
        ::close(fd);
        return -1;
    }

    json challenge_json;
    try {
        challenge_json = json::parse(*challenge_text);
    } catch (...) {
        ::close(fd);
        return -1;
    }

    if (!challenge_json.contains("type") || challenge_json["type"] != "challenge" ||
        !challenge_json.contains("nonce") || !challenge_json["nonce"].is_string()) {
        ::close(fd);
        return -1;
    }

    auto nonce_hex = challenge_json["nonce"].get<std::string>();
    auto nonce_bytes = util::from_hex(nonce_hex);
    if (!nonce_bytes || nonce_bytes->size() != 32) {
        ::close(fd);
        return -1;
    }

    auto signature = id.sign(*nonce_bytes);
    auto pubkey = id.public_key();

    json auth_response = {
        {"type", "challenge_response"},
        {"pubkey", util::to_hex(pubkey)},
        {"signature", util::to_hex(std::span<const uint8_t>(signature.data(), signature.size()))}
    };
    if (!ws_send_text(fd, auth_response.dump())) {
        ::close(fd);
        return -1;
    }

    auto auth_result_text = ws_recv_text(fd);
    if (!auth_result_text) {
        ::close(fd);
        return -1;
    }

    json auth_result;
    try {
        auth_result = json::parse(*auth_result_text);
    } catch (...) {
        ::close(fd);
        return -1;
    }

    if (!auth_result.contains("type") || auth_result["type"] != "auth_ok") {
        ::close(fd);
        return -1;
    }

    return fd;
}

// ---------------------------------------------------------------------------
// Config file rewrite helper (for SIGHUP tests)
// ---------------------------------------------------------------------------

inline void rewrite_config(const std::string& path,
                           const std::string& key, const json& value) {
    std::ifstream in(path);
    auto j = json::parse(in);
    in.close();
    j[key] = value;
    std::ofstream out(path);
    out << j.dump(2);
    out.close();
}

} // namespace relay_test
