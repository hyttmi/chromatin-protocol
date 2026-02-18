#include "ws/ws_server.h"

#include <spdlog/spdlog.h>

#include <random>
#include <sstream>

namespace chromatin::ws {

WsServer::WsServer(const config::Config& cfg,
                   kademlia::Kademlia& kad,
                   storage::Storage& storage,
                   replication::ReplLog& repl_log,
                   const crypto::KeyPair& keypair)
    : cfg_(cfg)
    , kad_(kad)
    , storage_(storage)
    , repl_log_(repl_log)
    , keypair_(keypair) {}

void WsServer::run() {
    uWS::App app;
    loop_ = uWS::Loop::get();

    app.ws<Session>("/*", {
        .compression = uWS::DISABLED,
        // 512 KiB: base64-encoded 256 KiB blobs (~341 KiB) + JSON overhead
        .maxPayloadLength = 512 * 1024,
        .idleTimeout = 120,

        .open = [](ws_t* /*ws*/) {
            spdlog::info("WS: client connected");
        },

        .message = [this](ws_t* ws, std::string_view message, uWS::OpCode opCode) {
            if (opCode != uWS::OpCode::TEXT) return;
            on_message(ws, message);
        },

        .close = [this](ws_t* ws, int /*code*/, std::string_view /*message*/) {
            auto* session = ws->getUserData();
            if (session->authenticated) {
                authenticated_.erase(session->fingerprint);
            }
            spdlog::info("WS: client disconnected");
        }
    });

    app.listen(cfg_.bind, cfg_.ws_port, [this](us_listen_socket_t* socket) {
        if (socket) {
            listen_socket_ = socket;
            listening_port_.store(static_cast<uint16_t>(
                us_socket_local_port(
                    /*ssl=*/0, reinterpret_cast<us_socket_t*>(socket))));
            spdlog::info("WS: listening on port {}", listening_port_.load());
        } else {
            spdlog::error("WS: failed to listen on port {}", cfg_.ws_port);
        }
    });

    // Periodic tick timer (200ms)
    auto* us_loop = reinterpret_cast<struct us_loop_t*>(loop_);
    struct TimerData { kademlia::Kademlia* kad; };
    tick_timer_ = us_create_timer(us_loop, 0, sizeof(TimerData));
    auto* td = static_cast<TimerData*>(us_timer_ext(tick_timer_));
    td->kad = &kad_;
    us_timer_set(tick_timer_, [](struct us_timer_t* t) {
        auto* data = static_cast<TimerData*>(us_timer_ext(t));
        data->kad->tick();
    }, 200, 200);

    app.run();
}

void WsServer::stop() {
    if (loop_) {
        loop_->defer([this]() {
            if (tick_timer_) {
                us_timer_close(tick_timer_);
                tick_timer_ = nullptr;
            }
            if (listen_socket_) {
                us_listen_socket_close(0, listen_socket_);
                listen_socket_ = nullptr;
            }
            // uWS will exit run() when no listeners, timers, and connections remain.
        });
    }
}

void WsServer::on_kademlia_store(const crypto::Hash& /*key*/,
                                  uint8_t /*data_type*/,
                                  std::span<const uint8_t> /*value*/) {
    // Will be implemented in Task 9 (push notifications).
    // For now, this is a no-op placeholder.
}

void WsServer::on_message(ws_t* ws, std::string_view message) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream{std::string(message)};

    if (!Json::parseFromStream(builder, stream, &root, &errs)) {
        send_error(ws, 0, 400, "invalid JSON");
        return;
    }

    std::string type = root.get("type", "").asString();
    int id = root.get("id", 0).asInt();

    if (type.empty()) {
        send_error(ws, id, 400, "missing type");
        return;
    }

    // Command dispatch
    if (type == "HELLO") {
        handle_hello(ws, root);
    } else if (type == "AUTH") {
        handle_auth(ws, root);
    } else if (type == "FETCH" || type == "SEND" || type == "ALLOW" ||
               type == "REVOKE" || type == "CONTACT_REQUEST") {
        if (!require_auth(ws, id)) return;
        // Individual handlers will be added in later tasks
        send_error(ws, id, 400, "not yet implemented: " + type);
    } else {
        send_error(ws, id, 400, "unknown command: " + type);
    }
}

// ---------- hex helpers (local to this TU) ----------

namespace {

std::string to_hex(std::span<const uint8_t> data) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        result.push_back(hex_chars[byte >> 4]);
        result.push_back(hex_chars[byte & 0x0F]);
    }
    return result;
}

std::optional<std::vector<uint8_t>> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto hi = hex[i];
        auto lo = hex[i + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        int h = nibble(hi);
        int l = nibble(lo);
        if (h < 0 || l < 0) return std::nullopt;
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
    }
    return bytes;
}

} // anonymous namespace

// ---------- auth handlers ----------

void WsServer::handle_hello(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();
    std::string fp_hex = msg.get("fingerprint", "").asString();
    if (fp_hex.size() != 64) {
        send_error(ws, id, 400, "fingerprint must be 64 hex chars");
        return;
    }

    auto fp_bytes = from_hex(fp_hex);
    if (!fp_bytes) {
        send_error(ws, 0, 400, "invalid hex in fingerprint");
        return;
    }

    crypto::Hash fingerprint{};
    std::copy(fp_bytes->begin(), fp_bytes->end(), fingerprint.begin());

    // Compute inbox key for responsibility check
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", fingerprint);

    if (!kad_.is_responsible(inbox_key)) {
        // REDIRECT — tell client which nodes are responsible.
        // TODO: query each node's repl_log seq via worker pool for proper ordering.
        // For now, seq is 0 (placeholder) since remote seq queries need TCP round-trips.
        auto nodes = kad_.responsible_nodes(inbox_key);
        Json::Value resp;
        resp["type"] = "REDIRECT";
        resp["id"] = id;
        Json::Value node_list(Json::arrayValue);
        for (const auto& node : nodes) {
            Json::Value n;
            n["address"] = node.address;
            n["ws_port"] = node.ws_port;
            n["seq"] = 0;
            node_list.append(n);
        }
        resp["nodes"] = node_list;
        send_json(ws, resp);
        ws->close();
        return;
    }

    // Generate random 32-byte nonce for challenge
    crypto::Hash nonce{};
    std::random_device rd;
    for (auto& byte : nonce) {
        byte = static_cast<uint8_t>(rd());
    }

    // Store in session
    auto* session = ws->getUserData();
    session->fingerprint = fingerprint;
    session->challenge_nonce = nonce;

    // Send CHALLENGE
    Json::Value resp;
    resp["type"] = "CHALLENGE";
    resp["id"] = id;
    resp["nonce"] = to_hex(nonce);
    send_json(ws, resp);
}

void WsServer::handle_auth(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Must have received HELLO first (nonce must be set)
    bool nonce_empty = true;
    for (auto b : session->challenge_nonce) {
        if (b != 0) { nonce_empty = false; break; }
    }
    if (nonce_empty) {
        send_error(ws, id, 400, "send HELLO first");
        return;
    }

    std::string sig_hex = msg.get("signature", "").asString();
    std::string pk_hex = msg.get("pubkey", "").asString();

    if (sig_hex.empty() || pk_hex.empty()) {
        send_error(ws, id, 400, "missing signature or pubkey");
        return;
    }

    auto sig_bytes = from_hex(sig_hex);
    auto pk_bytes = from_hex(pk_hex);

    if (!sig_bytes || !pk_bytes) {
        send_error(ws, id, 400, "invalid hex in signature or pubkey");
        return;
    }

    if (pk_bytes->size() != crypto::PUBLIC_KEY_SIZE) {
        send_error(ws, id, 400, "invalid pubkey size");
        return;
    }

    if (sig_bytes->size() != crypto::SIGNATURE_SIZE) {
        send_error(ws, id, 400, "invalid signature size");
        return;
    }

    // Verify SHA3-256(pubkey) == session fingerprint
    auto pk_hash = crypto::sha3_256(*pk_bytes);
    if (pk_hash != session->fingerprint) {
        send_error(ws, id, 401, "pubkey does not match fingerprint");
        return;
    }

    // Verify signature over the nonce
    std::span<const uint8_t> nonce_span(session->challenge_nonce.data(),
                                         session->challenge_nonce.size());
    if (!crypto::verify(nonce_span, *sig_bytes, *pk_bytes)) {
        send_error(ws, id, 401, "invalid signature");
        return;
    }

    // Auth succeeded
    session->authenticated = true;
    session->pubkey = std::move(*pk_bytes);
    authenticated_[session->fingerprint] = ws;

    // Count pending inbox messages
    int pending = 0;
    storage_.scan(storage::TABLE_INBOXES, session->fingerprint,
                  [&pending](std::span<const uint8_t> /*key*/,
                             std::span<const uint8_t> /*value*/) -> bool {
                      ++pending;
                      return true;  // continue scanning
                  });

    Json::Value resp;
    resp["type"] = "OK";
    resp["id"] = id;
    resp["pending_messages"] = pending;
    send_json(ws, resp);

    spdlog::info("WS: client authenticated, fingerprint={}, pending={}",
                 to_hex(session->fingerprint), pending);
}

bool WsServer::require_auth(ws_t* ws, int id) {
    auto* session = ws->getUserData();
    if (!session->authenticated) {
        send_error(ws, id, 401, "not authenticated");
        return false;
    }
    return true;
}

void WsServer::send_json(ws_t* ws, const Json::Value& msg) {
    Json::StreamWriterBuilder writer_builder;
    writer_builder["indentation"] = "";
    std::string json = Json::writeString(writer_builder, msg);
    ws->send(json, uWS::OpCode::TEXT);
}

void WsServer::send_error(ws_t* ws, int id, int code, const std::string& reason) {
    Json::Value err;
    err["type"] = "ERROR";
    err["id"] = id;
    err["code"] = code;
    err["reason"] = reason;
    send_json(ws, err);
}

} // namespace chromatin::ws
