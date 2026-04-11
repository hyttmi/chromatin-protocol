// relay_smoke_test -- WebSocket smoke test for chromatindb relay
//
// Connects to a running relay via WebSocket, completes ML-DSA-87
// challenge-response auth, exercises key paths (write, read, subscribe,
// compound queries), and validates JSON responses.
//
// Usage:
//   relay_smoke_test --identity /path/to/client.key
//                    [--host 127.0.0.1] [--port 4201]
//
// The identity must be an allowed client key on the relay (if ACL is active).
// This is a standalone program (not Catch2) for sanitizer runs.
//
// Sanitizer test commands (per D-10):
//
// ASAN:
//   cmake -B build-asan -DSANITIZER=asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
//   cmake --build build-asan -j$(nproc)
//   # Start relay from build-asan, then:
//   ./build-asan/tools/relay_smoke_test --identity /path/to/test.key
//
// UBSAN:
//   cmake -B build-ubsan -DSANITIZER=ubsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
//   cmake --build build-ubsan -j$(nproc)
//   # Start relay from build-ubsan, then:
//   ./build-ubsan/tools/relay_smoke_test --identity /path/to/test.key
//
// TSAN:
//   cmake -B build-tsan -DSANITIZER=tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
//   cmake --build build-tsan -j$(nproc)
//   # Start relay from build-tsan, then:
//   ./build-tsan/tools/relay_smoke_test --identity /path/to/test.key

#include "relay/identity/relay_identity.h"
#include "relay/util/base64.h"
#include "relay/util/hex.h"
#include "relay/wire/blob_codec.h"
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
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace ws = chromatindb::relay::ws;
namespace identity = chromatindb::relay::identity;
namespace util = chromatindb::relay::util;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// TCP helpers (blocking)
// ---------------------------------------------------------------------------

static bool send_all(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, p + sent, len - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static std::string recv_until(int fd, const std::string& delim, size_t max_bytes = 8192) {
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

static bool recv_all(int fd, uint8_t* buf, size_t len) {
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

static bool ws_send_text(int fd, const std::string& text) {
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
};

static std::optional<WsFrame> ws_recv_frame(int fd) {
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

static std::optional<std::string> ws_recv_text(int fd) {
    auto frame = ws_recv_frame(fd);
    if (!frame) return std::nullopt;
    return frame->payload;
}

// ---------------------------------------------------------------------------
// Blob signing helpers (ML-DSA-87 over SHA3-256 digest)
// ---------------------------------------------------------------------------

static std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl, uint64_t timestamp)
{
    std::array<uint8_t, 32> digest{};
    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);
    OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), namespace_id.size());
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());
    // LITTLE-endian for ttl and timestamp (protocol-defined, NOT big-endian)
    uint8_t ttl_le[4];
    for (int i = 0; i < 4; ++i) ttl_le[i] = static_cast<uint8_t>(ttl >> (i * 8));
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_le, 4);
    uint8_t ts_le[8];
    for (int i = 0; i < 8; ++i) ts_le[i] = static_cast<uint8_t>(timestamp >> (i * 8));
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ts_le, 8);
    OQS_SHA3_sha3_256_inc_finalize(digest.data(), &ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&ctx);
    return digest;
}

static json make_data_message(const identity::RelayIdentity& id,
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

static std::vector<TestResult> results;

static void record(const std::string& name, bool passed, const std::string& detail = "") {
    results.push_back({name, passed, detail});
    std::cout << (passed ? "  PASS: " : "  FAIL: ") << name;
    if (!detail.empty()) std::cout << " -- " << detail;
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --identity <key_path> [--host <addr>] [--port <port>]\n"
              << "\n"
              << "  --identity  Path to ML-DSA-87 client key file (required)\n"
              << "  --host      Relay host address (default: 127.0.0.1)\n"
              << "  --port      Relay port (default: 4201)\n";
}

int main(int argc, char* argv[]) {
    std::string identity_path;
    std::string host = "127.0.0.1";
    int port = 4201;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--identity" || arg == "-i") && i + 1 < argc) {
            identity_path = argv[++i];
        } else if ((arg == "--host" || arg == "-h") && i + 1 < argc) {
            host = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (identity_path.empty()) {
        std::cerr << "ERROR: --identity is required\n";
        usage(argv[0]);
        return 1;
    }

    // Load identity
    auto id = identity::RelayIdentity::load_from(identity_path);
    auto ns_hex = util::to_hex(id.public_key_hash());
    std::cout << "Client identity: " << ns_hex << "\n";

    // =====================================================================
    // Step 1: TCP connect
    // =====================================================================
    std::cout << "\n--- TCP connect ---\n";

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "ERROR: socket() failed\n";
        return 1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "ERROR: invalid host address: " << host << "\n";
        ::close(fd);
        return 1;
    }

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "ERROR: connect() to " << host << ":" << port << " failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }
    record("tcp_connect", true, host + ":" + std::to_string(port));

    // Set socket read timeout to prevent hanging on missing responses
    struct timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // =====================================================================
    // Step 2: WebSocket upgrade handshake
    // =====================================================================
    std::cout << "\n--- WebSocket upgrade ---\n";

    // Generate a random 16-byte key, base64 encode it
    uint8_t raw_key[16];
    RAND_bytes(raw_key, 16);
    // Simple base64 encode for 16 bytes (produces 24 chars)
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
        record("ws_upgrade", false, "failed to send upgrade request");
        ::close(fd);
        return 1;
    }

    auto response = recv_until(fd, "\r\n\r\n");
    bool upgrade_ok = response.find("101 Switching Protocols") != std::string::npos;

    // Validate Sec-WebSocket-Accept
    if (upgrade_ok) {
        auto expected_accept = ws::compute_accept_key(ws_key);
        if (response.find(expected_accept) == std::string::npos) {
            record("ws_upgrade", false, "Sec-WebSocket-Accept mismatch");
            ::close(fd);
            return 1;
        }
    }
    record("ws_upgrade", upgrade_ok,
           upgrade_ok ? "101 Switching Protocols" : "unexpected response");
    if (!upgrade_ok) {
        ::close(fd);
        return 1;
    }

    // =====================================================================
    // Step 3: Auth challenge-response
    // =====================================================================
    std::cout << "\n--- Authentication ---\n";

    // Receive challenge
    auto challenge_text = ws_recv_text(fd);
    if (!challenge_text) {
        record("auth_challenge_recv", false, "no frame received");
        ::close(fd);
        return 1;
    }

    json challenge_json;
    try {
        challenge_json = json::parse(*challenge_text);
    } catch (...) {
        record("auth_challenge_recv", false, "invalid JSON: " + *challenge_text);
        ::close(fd);
        return 1;
    }

    bool has_challenge_type = challenge_json.contains("type") &&
                               challenge_json["type"] == "challenge";
    bool has_nonce = challenge_json.contains("nonce") &&
                      challenge_json["nonce"].is_string();
    record("auth_challenge_recv", has_challenge_type && has_nonce,
           has_challenge_type ? "type=challenge" : "unexpected type");

    if (!has_challenge_type || !has_nonce) {
        ::close(fd);
        return 1;
    }

    // Sign challenge nonce
    auto nonce_hex = challenge_json["nonce"].get<std::string>();
    auto nonce_bytes = util::from_hex(nonce_hex);
    if (!nonce_bytes || nonce_bytes->size() != 32) {
        record("auth_sign", false, "invalid nonce hex");
        ::close(fd);
        return 1;
    }

    auto signature = id.sign(*nonce_bytes);
    auto pubkey = id.public_key();

    // Send challenge_response
    json auth_response = {
        {"type", "challenge_response"},
        {"pubkey", util::to_hex(pubkey)},
        {"signature", util::to_hex(std::span<const uint8_t>(signature.data(), signature.size()))}
    };
    if (!ws_send_text(fd, auth_response.dump())) {
        record("auth_send", false, "failed to send challenge_response");
        ::close(fd);
        return 1;
    }
    record("auth_send", true);

    // Receive auth result
    auto auth_result_text = ws_recv_text(fd);
    if (!auth_result_text) {
        record("auth_result", false, "no frame received");
        ::close(fd);
        return 1;
    }

    json auth_result;
    try {
        auth_result = json::parse(*auth_result_text);
    } catch (...) {
        record("auth_result", false, "invalid JSON");
        ::close(fd);
        return 1;
    }

    bool auth_ok = auth_result.contains("type") && auth_result["type"] == "auth_ok";
    record("auth_result", auth_ok,
           auth_ok ? "auth_ok" : auth_result.dump());
    if (!auth_ok) {
        ::close(fd);
        return 1;
    }

    // =====================================================================
    // Step 4: Exercise key paths
    // =====================================================================

    // Helper to send JSON and receive JSON response (text frames)
    auto send_recv = [&](const json& msg) -> std::optional<json> {
        if (!ws_send_text(fd, msg.dump())) return std::nullopt;
        auto frame = ws_recv_frame(fd);
        if (!frame) return std::nullopt;
        try { return json::parse(frame->payload); }
        catch (...) { return std::nullopt; }
    };

    // Helper to send JSON and receive raw WsFrame (for binary response detection)
    auto send_recv_frame = [&](const json& msg) -> std::optional<WsFrame> {
        if (!ws_send_text(fd, msg.dump())) return std::nullopt;
        return ws_recv_frame(fd);
    };

    // -----------------------------------------------------------------
    // Subscribe (relay-intercepted, no response expected)
    // -----------------------------------------------------------------
    {
        json subscribe_msg = {
            {"type", "subscribe"},
            {"request_id", 1},
            {"namespaces", json::array({ns_hex})}
        };
        if (!ws_send_text(fd, subscribe_msg.dump())) {
            record("subscribe", false, "send failed");
        } else {
            record("subscribe", true, "sent successfully");
        }
    }

    // =================================================================
    // Data write chain
    // =================================================================
    std::cout << "\n--- Data write chain ---\n";

    std::string written_blob_hash;  // captured from write_ack for subsequent tests
    std::vector<uint8_t> test_data = {'H','e','l','l','o',' ','R','e','l','a','y'};
    uint32_t test_ttl = 3600;
    uint64_t test_timestamp = static_cast<uint64_t>(std::time(nullptr));

    // data(8) -> write_ack(30)
    {
        auto data_msg = make_data_message(id, 100, test_data, test_ttl, test_timestamp);
        auto resp = send_recv(data_msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "write_ack" &&
                  resp->contains("hash") && resp->contains("seq_num");
        if (ok) {
            written_blob_hash = (*resp)["hash"].get<std::string>();
        }
        record("data_write", ok,
               ok ? ("hash=" + written_blob_hash) : (resp ? resp->dump() : "no response"));
    }

    // After writing to a subscribed namespace, we may receive a notification.
    // Collect it now so it doesn't interfere with subsequent request/response pairs.
    // The notification may arrive before or after the write_ack was consumed above,
    // but since we already consumed write_ack, the next frame should be the notification.
    std::optional<json> notification_json;
    if (!written_blob_hash.empty()) {
        auto notif_frame = ws_recv_frame(fd);
        if (notif_frame) {
            try {
                auto j = json::parse(notif_frame->payload);
                if (j.contains("type") && j["type"] == "notification") {
                    notification_json = j;
                }
            } catch (...) {}
        }
    }

    // notification(21) -- validate the captured notification
    {
        bool ok = notification_json.has_value() &&
                  notification_json->contains("type") &&
                  (*notification_json)["type"] == "notification" &&
                  notification_json->contains("namespace") &&
                  (*notification_json)["namespace"] == ns_hex &&
                  notification_json->contains("hash") &&
                  (*notification_json)["hash"] == written_blob_hash;
        record("notification", ok,
               ok ? ("ns+hash match") : (notification_json ? notification_json->dump() : "no notification"));
    }

    // read_request(31) -> read_response(32): binary WS frame
    if (!written_blob_hash.empty()) {
        json msg = {{"type", "read_request"}, {"request_id", 101},
                    {"namespace", ns_hex}, {"hash", written_blob_hash}};
        auto frame = send_recv_frame(msg);
        bool ok = false;
        std::string detail = "no response";
        if (frame) {
            try {
                auto resp = json::parse(frame->payload);
                if (resp.contains("type") && resp["type"] == "read_response" &&
                    resp.contains("status") && resp["status"] == 1 &&
                    resp.contains("data")) {
                    // Verify data matches what we wrote
                    auto decoded = util::base64_decode(resp["data"].get<std::string>());
                    if (decoded && *decoded == test_data) {
                        ok = true;
                        detail = "opcode=" + std::to_string(frame->opcode) + " data matches";
                    } else {
                        detail = "data mismatch";
                    }
                } else {
                    detail = resp.dump();
                }
            } catch (...) { detail = "parse error"; }
        }
        record("read_request", ok, detail);
    } else {
        record("read_request", false, "skipped -- no blob hash from write");
    }

    // metadata_request(47) -> metadata_response(48): found=true for written blob
    if (!written_blob_hash.empty()) {
        json msg = {{"type", "metadata_request"}, {"request_id", 102},
                    {"namespace", ns_hex}, {"hash", written_blob_hash}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "metadata_response" &&
                  resp->contains("found") && (*resp)["found"] == true;
        record("metadata_found", ok,
               ok ? "found=true" : (resp ? resp->dump() : "no response"));
    } else {
        record("metadata_found", false, "skipped -- no blob hash from write");
    }

    // exists_request(37) -> exists_response(38): exists=true for written blob
    if (!written_blob_hash.empty()) {
        json msg = {{"type", "exists_request"}, {"request_id", 103},
                    {"namespace", ns_hex}, {"hash", written_blob_hash}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "exists_response" &&
                  resp->contains("exists") && (*resp)["exists"] == true;
        record("exists_found", ok,
               ok ? "exists=true" : (resp ? resp->dump() : "no response"));
    } else {
        record("exists_found", false, "skipped -- no blob hash from write");
    }

    // batch_exists_request(49) -> batch_exists_response(50)
    if (!written_blob_hash.empty()) {
        std::string zero_hash(64, '0');
        json msg = {{"type", "batch_exists_request"}, {"request_id", 104},
                    {"namespace", ns_hex},
                    {"hashes", json::array({written_blob_hash, zero_hash})}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "batch_exists_response" &&
                  resp->contains("results") && (*resp)["results"].is_array() &&
                  (*resp)["results"].size() >= 2;
        record("batch_exists_request", ok,
               ok ? ("results=" + (*resp)["results"].dump()) : (resp ? resp->dump() : "no response"));
    } else {
        record("batch_exists_request", false, "skipped -- no blob hash from write");
    }

    // batch_read_request(53) -> batch_read_response(54): binary WS frame
    if (!written_blob_hash.empty()) {
        json msg = {{"type", "batch_read_request"}, {"request_id", 105},
                    {"namespace", ns_hex}, {"max_bytes", 1048576},
                    {"hashes", json::array({written_blob_hash})}};
        auto frame = send_recv_frame(msg);
        bool ok = false;
        std::string detail = "no response";
        if (frame) {
            try {
                auto resp = json::parse(frame->payload);
                if (resp.contains("type") && resp["type"] == "batch_read_response" &&
                    resp.contains("blobs") && resp["blobs"].is_array() &&
                    resp["blobs"].size() >= 1) {
                    ok = true;
                    detail = "opcode=" + std::to_string(frame->opcode) +
                             " blobs=" + std::to_string(resp["blobs"].size());
                } else {
                    detail = resp.dump();
                }
            } catch (...) { detail = "parse error"; }
        }
        record("batch_read_request", ok, detail);
    } else {
        record("batch_read_request", false, "skipped -- no blob hash from write");
    }

    // delete(17) -> delete_ack(18)
    // Delete sends a full signed tombstone blob (same fields as Data but type="delete", TTL=0)
    if (!written_blob_hash.empty()) {
        auto delete_msg = make_data_message(id, 106, {}, 0, static_cast<uint64_t>(std::time(nullptr)));
        delete_msg["type"] = "delete";
        auto resp = send_recv(delete_msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "delete_ack" &&
                  resp->contains("hash") && resp->contains("seq_num") &&
                  resp->contains("status");
        record("delete", ok,
               ok ? ("status=" + std::to_string((*resp)["status"].get<int>())) :
                     (resp ? resp->dump() : "no response"));

        // Drain tombstone notification (same as after data write)
        if (ok) {
            auto notif_frame = ws_recv_frame(fd);
            // Ignore — just clearing the buffer
        }
    } else {
        record("delete", false, "skipped -- no blob hash from write");
    }

    // =================================================================
    // Compound queries (existing + new)
    // =================================================================
    std::cout << "\n--- Compound queries ---\n";

    // node_info_request(39) -> node_info_response(40)
    {
        json msg = {{"type", "node_info_request"}, {"request_id", 10}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "node_info_response" &&
                  resp->contains("version");
        record("node_info_request", ok,
               ok ? "has version field" : (resp ? resp->dump() : "no response"));
    }

    // stats_request(35) -> stats_response(36)
    {
        json msg = {{"type", "stats_request"}, {"request_id", 11}, {"namespace", ns_hex}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "stats_response" &&
                  resp->contains("blob_count");
        record("stats_request", ok,
               ok ? "has blob_count field" : (resp ? resp->dump() : "no response"));
    }

    // storage_status_request(43) -> storage_status_response(44)
    {
        json msg = {{"type", "storage_status_request"}, {"request_id", 12}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "storage_status_response" &&
                  resp->contains("used_bytes") && resp->contains("capacity_bytes");
        record("storage_status_request", ok,
               ok ? "has used_bytes+capacity_bytes" : (resp ? resp->dump() : "no response"));
    }

    // exists_request(37) -> exists_response(38): not-found case
    {
        std::string zero_hash(64, '0');
        json msg = {{"type", "exists_request"}, {"request_id", 13},
                    {"namespace", ns_hex}, {"hash", zero_hash}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "exists_response" &&
                  resp->contains("exists");
        record("exists_request", ok,
               ok ? "has exists field" : (resp ? resp->dump() : "no response"));
    }

    // list_request(33) -> list_response(34)
    {
        json msg = {{"type", "list_request"}, {"request_id", 14}, {"namespace", ns_hex}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "list_response";
        record("list_request", ok,
               ok ? "list_response received" : (resp ? resp->dump() : "no response"));
    }

    // namespace_list_request(41) -> namespace_list_response(42)
    {
        json msg = {{"type", "namespace_list_request"}, {"request_id", 15}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "namespace_list_response";
        record("namespace_list_request", ok,
               ok ? "namespace_list_response received" : (resp ? resp->dump() : "no response"));
    }

    // peer_info_request(55) -> peer_info_response(56)
    {
        json msg = {{"type", "peer_info_request"}, {"request_id", 16}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "peer_info_response";
        record("peer_info_request", ok,
               ok ? "peer_info_response received" : (resp ? resp->dump() : "no response"));
    }

    // =================================================================
    // Remaining queries
    // =================================================================
    std::cout << "\n--- Remaining queries ---\n";

    // namespace_stats_request(45) -> namespace_stats_response(46)
    {
        json msg = {{"type", "namespace_stats_request"}, {"request_id", 200},
                    {"namespace", ns_hex}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "namespace_stats_response";
        record("namespace_stats_request", ok,
               ok ? "namespace_stats_response received" : (resp ? resp->dump() : "no response"));
    }

    // time_range_request(57) -> time_range_response(58)
    {
        json msg = {{"type", "time_range_request"}, {"request_id", 201},
                    {"namespace", ns_hex}, {"since", "0"},
                    {"until", std::to_string(static_cast<uint64_t>(std::time(nullptr)) + 86400)},
                    {"limit", 100}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "time_range_response";
        record("time_range_request", ok,
               ok ? "time_range_response received" : (resp ? resp->dump() : "no response"));
    }

    // delegation_list_request(51) -> delegation_list_response(52)
    {
        json msg = {{"type", "delegation_list_request"}, {"request_id", 202},
                    {"namespace", ns_hex}};
        auto resp = send_recv(msg);
        bool ok = resp && resp->contains("type") &&
                  (*resp)["type"] == "delegation_list_response";
        record("delegation_list_request", ok,
               ok ? "delegation_list_response received" : (resp ? resp->dump() : "no response"));
    }

    // =================================================================
    // Fire-and-forget
    // =================================================================
    std::cout << "\n--- Fire-and-forget ---\n";

    // ping(5): send, then verify connection alive with a probe request
    {
        json ping_msg = {{"type", "ping"}, {"request_id", 210}};
        ws_send_text(fd, ping_msg.dump());
        // Verify connection alive by sending a request that produces a response
        json probe = {{"type", "node_info_request"}, {"request_id", 211}};
        auto resp = send_recv(probe);
        bool ok = resp && (*resp)["type"] == "node_info_response";
        record("ping", ok,
               ok ? "connection alive after ping" : "connection broken after ping");
    }

    // pong(6): send, then verify connection alive
    {
        json pong_msg = {{"type", "pong"}, {"request_id", 220}};
        ws_send_text(fd, pong_msg.dump());
        json probe = {{"type", "node_info_request"}, {"request_id", 221}};
        auto resp = send_recv(probe);
        bool ok = resp && (*resp)["type"] == "node_info_response";
        record("pong", ok,
               ok ? "connection alive after pong" : "connection broken after pong");
    }

    // =================================================================
    // Error paths
    // =================================================================
    std::cout << "\n--- Error paths ---\n";

    // Read nonexistent blob -> read_response with status=0 (binary WS frame)
    {
        std::string zero_hash(64, '0');
        json msg = {{"type", "read_request"}, {"request_id", 300},
                    {"namespace", ns_hex}, {"hash", zero_hash}};
        auto frame = send_recv_frame(msg);
        bool ok = false;
        if (frame) {
            try {
                auto resp = json::parse(frame->payload);
                ok = resp.contains("type") && resp["type"] == "read_response" &&
                     resp.contains("status") && resp["status"] == 0;
            } catch (...) {}
        }
        record("error_read_nonexistent", ok,
               ok ? "read_response status=0" : "unexpected response");
    }

    // Metadata for nonexistent hash -> metadata_response found=false
    {
        std::string zero_hash(64, '0');
        json msg = {{"type", "metadata_request"}, {"request_id", 301},
                    {"namespace", ns_hex}, {"hash", zero_hash}};
        auto resp = send_recv(msg);
        bool ok = resp && (*resp)["type"] == "metadata_response" &&
                  resp->contains("found") && (*resp)["found"] == false;
        record("error_metadata_nonexistent", ok,
               ok ? "metadata_response found=false" : (resp ? resp->dump() : "no response"));
    }

    // Blocked type -> relay error response
    {
        json msg = {{"type", "blob_notify"}, {"request_id", 302}};
        ws_send_text(fd, msg.dump());
        auto frame = ws_recv_frame(fd);
        bool ok = false;
        if (frame) {
            try {
                auto resp = json::parse(frame->payload);
                ok = resp.contains("type") && resp["type"] == "error" &&
                     resp.contains("code");
            } catch (...) {}
        }
        record("error_blocked_type", ok,
               ok ? "error response with type+code" : "unexpected response");
    }

    // Stats for nonexistent namespace -> stats_response with zeros
    {
        std::string fake_ns(64, 'f');
        json msg = {{"type", "stats_request"}, {"request_id", 303},
                    {"namespace", fake_ns}};
        auto resp = send_recv(msg);
        bool ok = resp && (*resp)["type"] == "stats_response" &&
                  resp->contains("blob_count");
        record("error_stats_nonexistent_ns", ok,
               ok ? "stats_response with blob_count" : (resp ? resp->dump() : "no response"));
    }

    // =================================================================
    // Goodbye (last fire-and-forget -- may trigger disconnect)
    // =================================================================
    {
        json goodbye_msg = {{"type", "goodbye"}};
        bool sent = ws_send_text(fd, goodbye_msg.dump());
        record("goodbye", sent, sent ? "sent successfully" : "send failed");
    }

    // Unsubscribe (cleanup -- best effort after goodbye)
    {
        json unsubscribe_msg = {
            {"type", "unsubscribe"},
            {"request_id", 20},
            {"namespaces", json::array({ns_hex})}
        };
        ws_send_text(fd, unsubscribe_msg.dump());
    }

    // =====================================================================
    // Summary
    // =====================================================================
    ::close(fd);

    std::cout << "\n=== Summary ===\n";
    int passed = 0, failed = 0;
    for (const auto& r : results) {
        if (r.passed) ++passed;
        else ++failed;
    }
    std::cout << "  Passed: " << passed << "/" << results.size() << "\n";
    std::cout << "  Failed: " << failed << "/" << results.size() << "\n";

    if (failed > 0) {
        std::cout << "\n  Failed tests:\n";
        for (const auto& r : results) {
            if (!r.passed) {
                std::cout << "    - " << r.name;
                if (!r.detail.empty()) std::cout << ": " << r.detail;
                std::cout << "\n";
            }
        }
    }

    return failed > 0 ? 1 : 0;
}
