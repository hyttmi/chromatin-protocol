#include "ws/ws_server.h"

#include <spdlog/spdlog.h>

#include <chrono>
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

        .open = [this](ws_t* ws) {
            connections_.insert(ws);
            spdlog::info("WS: client connected");
        },

        .message = [this](ws_t* ws, std::string_view message, uWS::OpCode opCode) {
            if (opCode != uWS::OpCode::TEXT) return;
            on_message(ws, message);
        },

        .close = [this](ws_t* ws, int /*code*/, std::string_view /*message*/) {
            connections_.erase(ws);
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
    } else if (type == "FETCH") {
        if (!require_auth(ws, id)) return;
        handle_fetch(ws, root);
    } else if (type == "SEND") {
        if (!require_auth(ws, id)) return;
        handle_send(ws, root);
    } else if (type == "ALLOW" ||
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

std::string to_base64(std::span<const uint8_t> data) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                          (static_cast<uint32_t>(data[i + 1]) << 8) |
                          static_cast<uint32_t>(data[i + 2]);
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(table[(triple >> 6) & 0x3F]);
        out.push_back(table[triple & 0x3F]);
    }

    if (i < data.size()) {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size())
            triple |= static_cast<uint32_t>(data[i + 1]) << 8;

        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        if (i + 1 < data.size())
            out.push_back(table[(triple >> 6) & 0x3F]);
        else
            out.push_back('=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::vector<uint8_t>> from_base64(const std::string& input) {
    static constexpr uint8_t decode_table[] = {
        // 0-42: invalid
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,255,255,255,255,
        // 43: '+' = 62
        62,
        // 44-46: invalid
        255,255,255,
        // 47: '/' = 63
        63,
        // 48-57: '0'-'9' = 52-61
        52,53,54,55,56,57,58,59,60,61,
        // 58-64: invalid
        255,255,255,255,255,255,255,
        // 65-90: 'A'-'Z' = 0-25
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
        // 91-96: invalid
        255,255,255,255,255,255,
        // 97-122: 'a'-'z' = 26-51
        26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51
    };

    std::vector<uint8_t> out;
    out.reserve((input.size() / 4) * 3);

    uint32_t accum = 0;
    int bits = 0;

    for (char c : input) {
        if (c == '=') break;
        if (c < 0 || static_cast<unsigned char>(c) >= sizeof(decode_table))
            return std::nullopt;
        uint8_t val = decode_table[static_cast<unsigned char>(c)];
        if (val == 255) return std::nullopt;

        accum = (accum << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }
    return out;
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

// ---------- command handlers ----------

void WsServer::handle_fetch(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Optional "since" timestamp filter (default: 0 = fetch all)
    uint64_t since = 0;
    if (msg.isMember("since")) {
        since = msg["since"].asUInt64();
    }

    // Key layout: recipient_fp(32) || timestamp(8 BE) || msg_id(32) = 72 bytes
    // Value layout: msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4 BE) || blob

    Json::Value messages(Json::arrayValue);

    storage_.scan(storage::TABLE_INBOXES, session->fingerprint,
                  [&](std::span<const uint8_t> key,
                      std::span<const uint8_t> value) -> bool {
        // Validate key length: 32 (fp) + 8 (timestamp) + 32 (msg_id) = 72
        if (key.size() != 72) return true;  // skip malformed

        // Extract timestamp from key (big-endian, bytes 32..39)
        uint64_t ts = 0;
        for (int i = 0; i < 8; ++i) {
            ts = (ts << 8) | key[32 + i];
        }

        // Skip messages older than 'since'
        if (ts < since) return true;

        // Validate minimum value size: 32 (msg_id) + 32 (sender_fp) + 8 (ts) + 4 (blob_len) = 76
        if (value.size() < 76) return true;  // skip malformed

        // Extract fields from value
        auto msg_id_span = value.subspan(0, 32);
        auto sender_fp_span = value.subspan(32, 32);
        // timestamp from value (bytes 64..71)
        uint64_t val_ts = 0;
        for (int i = 0; i < 8; ++i) {
            val_ts = (val_ts << 8) | value[64 + i];
        }
        // blob_len (4 bytes big-endian at offset 72)
        uint32_t blob_len = (static_cast<uint32_t>(value[72]) << 24) |
                            (static_cast<uint32_t>(value[73]) << 16) |
                            (static_cast<uint32_t>(value[74]) << 8) |
                            static_cast<uint32_t>(value[75]);

        if (value.size() < 76 + blob_len) return true;  // skip malformed
        auto blob_span = value.subspan(76, blob_len);

        Json::Value entry;
        entry["msg_id"] = to_hex(msg_id_span);
        entry["from"] = to_hex(sender_fp_span);
        entry["timestamp"] = Json::Value(val_ts);
        entry["blob"] = to_base64(blob_span);

        messages.append(entry);
        return true;  // continue scanning
    });

    Json::Value resp;
    resp["type"] = "MESSAGES";
    resp["id"] = id;
    resp["messages"] = messages;
    send_json(ws, resp);
}

void WsServer::handle_send(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse recipient fingerprint
    std::string to_hex_str = msg.get("to", "").asString();
    if (to_hex_str.size() != 64) {
        send_error(ws, id, 400, "to must be 64 hex chars");
        return;
    }
    auto to_bytes = from_hex(to_hex_str);
    if (!to_bytes) {
        send_error(ws, id, 400, "invalid hex in to");
        return;
    }
    crypto::Hash recipient_fp{};
    std::copy(to_bytes->begin(), to_bytes->end(), recipient_fp.begin());

    // Parse blob (base64)
    std::string blob_b64 = msg.get("blob", "").asString();
    if (blob_b64.empty()) {
        send_error(ws, id, 400, "missing blob");
        return;
    }
    auto blob = from_base64(blob_b64);
    if (!blob) {
        send_error(ws, id, 400, "invalid base64 in blob");
        return;
    }
    static constexpr size_t MAX_BLOB_SIZE = 256 * 1024;  // 256 KiB
    if (blob->size() > MAX_BLOB_SIZE) {
        send_error(ws, id, 400, "blob exceeds 256 KiB");
        return;
    }

    // Generate random 32-byte msg_id
    crypto::Hash msg_id{};
    std::random_device rd;
    for (auto& byte : msg_id) {
        byte = static_cast<uint8_t>(rd());
    }

    // Timestamp (seconds since epoch)
    auto now = std::chrono::system_clock::now();
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count());

    // Build inbox message binary:
    // msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    uint32_t blob_len = static_cast<uint32_t>(blob->size());
    std::vector<uint8_t> message_binary;
    message_binary.reserve(32 + 32 + 8 + 4 + blob_len);
    message_binary.insert(message_binary.end(), msg_id.begin(), msg_id.end());
    message_binary.insert(message_binary.end(),
                          session->fingerprint.begin(), session->fingerprint.end());
    for (int i = 7; i >= 0; --i) {
        message_binary.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    message_binary.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    message_binary.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    message_binary.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    message_binary.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    message_binary.insert(message_binary.end(), blob->begin(), blob->end());

    // Build composite inbox storage key:
    // recipient_fp(32) || timestamp(8 BE) || msg_id(32) = 72 bytes
    std::vector<uint8_t> inbox_storage_key;
    inbox_storage_key.reserve(72);
    inbox_storage_key.insert(inbox_storage_key.end(),
                             recipient_fp.begin(), recipient_fp.end());
    for (int i = 7; i >= 0; --i) {
        inbox_storage_key.push_back(
            static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    inbox_storage_key.insert(inbox_storage_key.end(), msg_id.begin(), msg_id.end());

    // Compute inbox_key = SHA3-256("inbox:" || recipient_fp)
    // Used for Kademlia routing (responsibility check) and replication.
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);

    // Capture values needed by the worker/deferred callbacks
    auto msg_id_copy = msg_id;

    // Dispatch to worker pool
    workers_.post([this, ws, id, inbox_key,
                   inbox_storage_key = std::move(inbox_storage_key),
                   message_binary = std::move(message_binary),
                   msg_id_copy]() {
        // Store locally with the composite key so FETCH can scan by fingerprint
        bool ok = storage_.put(storage::TABLE_INBOXES,
                               inbox_storage_key, message_binary);

        // Also replicate via Kademlia (fire-and-forget for the response)
        if (ok) {
            kad_.store(inbox_key, 0x02, message_binary);
        }

        // Defer response back to the uWS event loop thread
        loop_->defer([this, ws, id, ok, msg_id_copy]() {
            // Check if the connection is still alive
            if (connections_.count(ws) == 0) return;

            if (ok) {
                Json::Value resp;
                resp["type"] = "SEND_ACK";
                resp["id"] = id;
                resp["msg_id"] = to_hex(msg_id_copy);
                send_json(ws, resp);
            } else {
                send_error(ws, id, 500, "store failed");
            }
        });
    });
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
