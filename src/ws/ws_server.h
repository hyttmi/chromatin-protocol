#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <App.h>
#include <json/json.h>

#include "config/config.h"
#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "replication/repl_log.h"
#include "storage/storage.h"
#include "ws/worker_pool.h"

namespace chromatin::ws {

struct RateLimiter {
    double tokens = 50.0;
    double max_tokens = 50.0;
    double refill_rate = 10.0;  // tokens per second
    std::chrono::steady_clock::time_point last_refill = std::chrono::steady_clock::now();

    bool consume(double cost = 1.0) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_refill).count();
        last_refill = now;
        tokens = std::min(max_tokens, tokens + elapsed * refill_rate);
        if (tokens < cost) return false;
        tokens -= cost;
        return true;
    }
};

struct Session {
    crypto::Hash fingerprint{};
    std::vector<uint8_t> pubkey;
    bool authenticated = false;
    crypto::Hash challenge_nonce{};
    RateLimiter rate_limiter;

    struct PendingUpload {
        uint32_t request_id = 0;
        crypto::Hash recipient_fp{};
        int id = 0;                // JSON id for the SEND that started this
        uint32_t expected_size = 0;
        uint32_t received = 0;
        uint16_t next_chunk = 0;
        std::vector<uint8_t> data; // accumulated blob data
        std::chrono::steady_clock::time_point started;
    };
    std::optional<PendingUpload> pending_upload;
};

class WsServer {
public:
    WsServer(const config::Config& cfg,
             kademlia::Kademlia& kad,
             storage::Storage& storage,
             replication::ReplLog& repl_log,
             const crypto::KeyPair& keypair);

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // Block on uWS event loop (call from dedicated thread).
    void run();

    // Stop the event loop (thread-safe).
    void stop();

    // Called from TCP thread when a STORE arrives via Kademlia.
    // Pushes NEW_MESSAGE / CONTACT_REQUEST to connected clients via defer().
    void on_kademlia_store(const crypto::Hash& key,
                           uint8_t data_type,
                           std::span<const uint8_t> value);

    // Get the listening port (0 if not yet listening).
    uint16_t listening_port() const { return listening_port_.load(); }

private:
    using ws_t = uWS::WebSocket<false, true, Session>;

    const config::Config& cfg_;
    kademlia::Kademlia& kad_;
    storage::Storage& storage_;
    replication::ReplLog& repl_log_;
    const crypto::KeyPair& keypair_;

    WorkerPool workers_{4};
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
    uWS::Loop* loop_ = nullptr;
    us_listen_socket_t* listen_socket_ = nullptr;
    struct us_timer_t* tick_timer_ = nullptr;
    std::atomic<uint16_t> listening_port_{0};

    // All open connections (uWS thread only).  Used by deferred callbacks
    // to verify a ws pointer is still valid before sending a reply.
    std::unordered_set<ws_t*> connections_;

    // Authenticated sessions: fingerprint -> set of ws pointers (uWS thread only).
    // Multiple devices can be connected with the same identity simultaneously.
    std::unordered_map<crypto::Hash, std::unordered_set<ws_t*>, crypto::HashHash> authenticated_;

    // Command dispatch
    void on_message(ws_t* ws, std::string_view message);
    void on_binary(ws_t* ws, std::span<const uint8_t> data);
    std::atomic<uint32_t> next_request_id_{1};

    // Auth handlers
    void handle_hello(ws_t* ws, const Json::Value& msg);
    void handle_auth(ws_t* ws, const Json::Value& msg);
    bool require_auth(ws_t* ws, int id);

    // Command handlers
    void handle_list(ws_t* ws, const Json::Value& msg);
    void handle_get(ws_t* ws, const Json::Value& msg);
    void handle_send(ws_t* ws, const Json::Value& msg);
    void handle_allow(ws_t* ws, const Json::Value& msg);
    void handle_revoke(ws_t* ws, const Json::Value& msg);
    void handle_contact_request(ws_t* ws, const Json::Value& msg);
    void handle_delete(ws_t* ws, const Json::Value& msg);
    void handle_status(ws_t* ws, const Json::Value& msg);

    // Helpers
    void send_json(ws_t* ws, const Json::Value& msg);
    void send_error(ws_t* ws, int id, int code, const std::string& reason);
    void check_upload_timeouts();
};

} // namespace chromatin::ws
