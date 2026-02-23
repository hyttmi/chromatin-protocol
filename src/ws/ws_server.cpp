#include "ws/ws_server.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>
#include <cstring>

#include <oqs/oqs.h>

namespace chromatin::ws {

template<bool SSL>
WsServer<SSL>::WsServer(const config::Config& cfg,
                        kademlia::Kademlia& kad,
                        storage::Storage& storage,
                        replication::ReplLog& repl_log,
                        const crypto::KeyPair& keypair)
    : cfg_(cfg)
    , kad_(kad)
    , storage_(storage)
    , repl_log_(repl_log)
    , keypair_(keypair)
    , workers_(config::defaults::WORKER_POOL_THREADS,
               config::defaults::WORKER_POOL_QUEUE_MAX) {}

template<bool SSL>
void WsServer<SSL>::run() {
    uWS::SocketContextOptions ssl_options = {};
    uWS::TemplatedApp<SSL> app(ssl_options);
    loop_ = uWS::Loop::get();

    app.template ws<Session>("/*", {
        .compression = uWS::DISABLED,
        // 512 KiB: base64-encoded 256 KiB blobs (~341 KiB) + JSON overhead
        .maxPayloadLength = 1048576 + 64,  // 1 MiB chunk + header overhead
        .idleTimeout = config::defaults::WS_IDLE_TIMEOUT,

        .open = [this](ws_t* ws) {
            connections_.insert(ws);
            auto* session = ws->getUserData();
            session->rate_limiter.tokens = config::defaults::RATE_LIMIT_TOKENS;
            session->rate_limiter.max_tokens = config::defaults::RATE_LIMIT_MAX;
            session->rate_limiter.refill_rate = config::defaults::RATE_LIMIT_REFILL;
            session->rate_limiter.last_refill = std::chrono::steady_clock::now();
            spdlog::info("WS: client connected");
        },

        .message = [this](ws_t* ws, std::string_view message, uWS::OpCode opCode) {
            if (opCode == uWS::OpCode::TEXT) {
                on_message(ws, message);
            } else if (opCode == uWS::OpCode::BINARY) {
                auto data = std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(message.data()), message.size());
                on_binary(ws, data);
            }
        },

        .close = [this](ws_t* ws, int /*code*/, std::string_view /*message*/) {
            connections_.erase(ws);
            auto* session = ws->getUserData();
            if (session->authenticated) {
                auto it = authenticated_.find(session->fingerprint);
                if (it != authenticated_.end()) {
                    it->second.erase(ws);
                    if (it->second.empty()) {
                        authenticated_.erase(it);
                        // Also clean up fingerprint rate limiter when no sessions remain
                        fp_rate_limiters_.erase(session->fingerprint);
                    }
                }
            }
            spdlog::info("WS: client disconnected");
        }
    });

    app.listen(cfg_.bind, cfg_.ws_port, [this](us_listen_socket_t* socket) {
        if (socket) {
            listen_socket_ = socket;
            listening_port_.store(static_cast<uint16_t>(
                us_socket_local_port(
                    SSL ? 1 : 0, reinterpret_cast<us_socket_t*>(socket))));
            spdlog::info("WS: listening on port {}", listening_port_.load());
        } else {
            spdlog::error("WS: failed to listen on port {}", cfg_.ws_port);
        }
    });

    // Periodic tick timer (200ms)
    auto* us_loop = reinterpret_cast<struct us_loop_t*>(loop_);
    struct TimerData { kademlia::Kademlia* kad; WsServer* server; };
    tick_timer_ = us_create_timer(us_loop, 0, sizeof(TimerData));
    auto* td = static_cast<TimerData*>(us_timer_ext(tick_timer_));
    td->kad = &kad_;
    td->server = this;
    us_timer_set(tick_timer_, [](struct us_timer_t* t) {
        auto* data = static_cast<TimerData*>(us_timer_ext(t));
        data->kad->tick();
        data->server->check_upload_timeouts();
    }, 200, 200);

    app.run();
}

template<bool SSL>
void WsServer<SSL>::stop() {
    if (loop_) {
        loop_->defer([this]() {
            if (tick_timer_) {
                us_timer_close(tick_timer_);
                tick_timer_ = nullptr;
            }
            if (listen_socket_) {
                us_listen_socket_close(SSL ? 1 : 0, listen_socket_);
                listen_socket_ = nullptr;
            }
            // uWS will exit run() when no listeners, timers, and connections remain.
        });
    }
}

template<bool SSL>
void WsServer<SSL>::on_message(ws_t* ws, std::string_view message) {
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

    // Command dispatch table: handler, requires_auth, rate_limit_cost
    struct Command {
        void (WsServer::*handler)(ws_t*, const Json::Value&);
        bool requires_auth;
        double rate_cost;  // 0 = exempt from rate limiting
    };

    static const std::unordered_map<std::string, Command> commands = {
        {"HELLO",           {&WsServer::handle_hello,           false, 1.0}},
        {"AUTH",            {&WsServer::handle_auth,            false, 1.0}},
        {"LIST",            {&WsServer::handle_list,            true,  1.0}},
        {"GET",             {&WsServer::handle_get,             true,  1.0}},
        {"SEND",            {&WsServer::handle_send,            true,  2.0}},
        {"ALLOW",           {&WsServer::handle_allow,           true,  1.0}},
        {"REVOKE",          {&WsServer::handle_revoke,          true,  1.0}},
        {"CONTACT_REQUEST", {&WsServer::handle_contact_request, true,  3.0}},
        {"DELETE",          {&WsServer::handle_delete,          true,  1.0}},
        {"STATUS",          {&WsServer::handle_status,          false, 0.0}},
        {"RESOLVE_NAME",   {&WsServer::handle_resolve_name,   true,  1.0}},
        {"GET_PROFILE",    {&WsServer::handle_get_profile,    true,  1.0}},
        {"LIST_REQUESTS",  {&WsServer::handle_list_requests,  true,  1.0}},
        {"SET_PROFILE",    {&WsServer::handle_set_profile,    true,  2.0}},
        {"REGISTER_NAME",  {&WsServer::handle_register_name,  true,  3.0}},
        {"GROUP_CREATE",   {&WsServer::handle_group_create,   true,  3.0}},
        {"GROUP_INFO",     {&WsServer::handle_group_info,     true,  1.0}},
        {"GROUP_SEND",     {&WsServer::handle_group_send,     true,  2.0}},
        {"GROUP_LIST",     {&WsServer::handle_group_list,     true,  1.0}},
        {"GROUP_GET",      {&WsServer::handle_group_get,      true,  1.0}},
        {"GROUP_UPDATE",   {&WsServer::handle_group_update,   true,  2.0}},
        {"GROUP_DELETE",   {&WsServer::handle_group_delete,   true,  1.0}},
        {"GROUP_DESTROY",  {&WsServer::handle_group_destroy,  true,  3.0}},
    };

    auto it = commands.find(type);
    if (it == commands.end()) {
        send_error(ws, id, 400, "unknown command: " + type);
        return;
    }

    const auto& cmd = it->second;
    if (cmd.requires_auth && !require_auth(ws, id)) return;
    if (cmd.rate_cost > 0.0) {
        auto* session = ws->getUserData();
        if (session->authenticated) {
            // Use shared per-fingerprint rate limiter
            auto fp_it = fp_rate_limiters_.find(session->fingerprint);
            if (fp_it != fp_rate_limiters_.end() && !fp_it->second.consume(cmd.rate_cost)) {
                send_error(ws, id, 429, "rate limit exceeded");
                return;
            }
        } else {
            // Pre-auth: use per-session rate limiter
            if (!session->rate_limiter.consume(cmd.rate_cost)) {
                send_error(ws, id, 429, "rate limit exceeded");
                return;
            }
        }
    }
    (this->*cmd.handler)(ws, root);
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

static constexpr size_t INLINE_THRESHOLD = 64 * 1024;  // 64 KB

} // anonymous namespace

template<bool SSL>
void WsServer<SSL>::on_binary(ws_t* ws, std::span<const uint8_t> data) {
    auto* session = ws->getUserData();

    // Must be authenticated
    if (!session->authenticated) {
        send_error(ws, 0, 401, "not authenticated");
        return;
    }

    // Minimum 7 bytes: frame_type(1) + request_id(4) + chunk_index(2)
    if (data.size() < 7) {
        send_error(ws, 0, 400, "binary frame too short");
        return;
    }

    // Parse header
    uint8_t frame_type = data[0];
    uint32_t request_id = (static_cast<uint32_t>(data[1]) << 24) |
                          (static_cast<uint32_t>(data[2]) << 16) |
                          (static_cast<uint32_t>(data[3]) << 8) |
                          static_cast<uint32_t>(data[4]);
    uint16_t chunk_index = (static_cast<uint16_t>(data[5]) << 8) |
                           static_cast<uint16_t>(data[6]);

    // Only accept UPLOAD_CHUNK (0x01) from client
    if (frame_type != 0x01) {
        send_error(ws, 0, 400, "unsupported binary frame type");
        return;
    }

    // Must have an active pending upload with matching request_id
    if (!session->pending_upload ||
        session->pending_upload->request_id != request_id) {
        send_error(ws, 0, 400, "no active upload for this request_id");
        return;
    }

    auto& upload = *session->pending_upload;

    // Chunk index must match expected next chunk
    if (chunk_index != upload.next_chunk) {
        send_error(ws, 0, 400, "unexpected chunk index");
        session->pending_upload.reset();
        return;
    }

    // Payload is everything after the 7-byte header
    auto payload = data.subspan(7);

    // Validate payload size <= 1 MiB
    static constexpr size_t MAX_CHUNK_SIZE = 1048576;  // 1 MiB
    if (payload.size() > MAX_CHUNK_SIZE) {
        send_error(ws, 0, 400, "chunk exceeds 1 MiB");
        session->pending_upload.reset();
        return;
    }

    // Guard against exceeding declared size
    if (upload.received + static_cast<uint32_t>(payload.size()) > upload.expected_size) {
        send_error(ws, upload.id, 400, "upload exceeds declared size");
        session->pending_upload.reset();
        return;
    }

    // Append payload to accumulated data
    upload.data.insert(upload.data.end(), payload.begin(), payload.end());
    upload.received += static_cast<uint32_t>(payload.size());
    upload.next_chunk++;

    // Check if upload is complete
    if (upload.received >= upload.expected_size) {
        spdlog::info("WS: chunked upload complete, request_id={}, total={} bytes",
                     upload.request_id, upload.received);

        // Truncate to expected size (last chunk may have filled exactly)
        upload.data.resize(upload.expected_size);

        if (upload.group_id.has_value()) {
            // ---- Group upload completion ----
            auto group_id = *upload.group_id;
            auto upload_msg_id = upload.msg_id;
            auto gek_version = upload.gek_version;
            auto sender_fp = session->fingerprint;
            int send_id = upload.id;
            auto blob_data = std::move(upload.data);
            session->pending_upload.reset();

            // Build GROUP_MESSAGE binary:
            // group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob
            auto now = std::chrono::system_clock::now();
            uint64_t timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
            uint32_t blob_len = static_cast<uint32_t>(blob_data.size());

            std::vector<uint8_t> msg_bytes;
            msg_bytes.reserve(112 + blob_len);
            msg_bytes.insert(msg_bytes.end(), group_id.begin(), group_id.end());
            msg_bytes.insert(msg_bytes.end(), sender_fp.begin(), sender_fp.end());
            msg_bytes.insert(msg_bytes.end(), upload_msg_id.begin(), upload_msg_id.end());
            for (int i = 7; i >= 0; --i)
                msg_bytes.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>((gek_version >> 24) & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>((gek_version >> 16) & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>((gek_version >> 8) & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>(gek_version & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
            msg_bytes.push_back(static_cast<uint8_t>(blob_len & 0xFF));
            msg_bytes.insert(msg_bytes.end(), blob_data.begin(), blob_data.end());

            // Compute DHT routing key
            auto group_key = crypto::sha3_256_prefixed("group:",
                std::span<const uint8_t>(group_id.data(), group_id.size()));

            auto msg_id_copy = upload_msg_id;

            if (!workers_.post([this, ws, send_id, group_key, group_id, sender_fp,
                                msg_bytes = std::move(msg_bytes),
                                msg_id_copy]() mutable {
                bool ok = kad_.store(group_key, 0x05, msg_bytes);

                // Ensure data is stored locally for GROUP_LIST/GET/DELETE
                if (ok) {
                    std::vector<uint8_t> idx_key;
                    idx_key.reserve(64);
                    idx_key.insert(idx_key.end(), group_id.begin(), group_id.end());
                    idx_key.insert(idx_key.end(), msg_id_copy.begin(), msg_id_copy.end());

                    if (!storage_.get(storage::TABLE_GROUP_BLOBS, idx_key)) {
                        std::vector<uint8_t> idx_value;
                        idx_value.reserve(48);
                        idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
                        idx_value.insert(idx_value.end(), msg_bytes.begin() + 96, msg_bytes.begin() + 104);
                        idx_value.insert(idx_value.end(), msg_bytes.begin() + 108, msg_bytes.begin() + 112);
                        idx_value.insert(idx_value.end(), msg_bytes.begin() + 104, msg_bytes.begin() + 108);

                        std::vector<uint8_t> blob(msg_bytes.begin() + 112, msg_bytes.end());

                        storage_.batch_put({
                            {storage::TABLE_GROUP_INDEX, idx_key, idx_value},
                            {storage::TABLE_GROUP_BLOBS, idx_key, blob},
                        });
                    }
                }

                loop_->defer([this, ws, send_id, ok, msg_id_copy]() {
                    if (connections_.count(ws) == 0) return;
                    if (ok) {
                        Json::Value resp;
                        resp["type"] = "OK";
                        resp["id"] = send_id;
                        resp["msg_id"] = to_hex(msg_id_copy);
                        send_json(ws, resp);
                    } else {
                        send_error(ws, send_id, 500, "store failed");
                    }
                });
            })) {
                send_error(ws, send_id, 503, "server overloaded");
                return;
            }
        } else {
            // ---- Regular inbox upload completion ----
            // Generate msg_id
            crypto::Hash msg_id{};
            OQS_randombytes(msg_id.data(), msg_id.size());

            auto now = std::chrono::system_clock::now();
            uint64_t timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count());

            // Capture upload data before resetting
            auto recipient_fp = upload.recipient_fp;
            auto sender_fp = session->fingerprint;
            int send_id = upload.id;
            auto blob_data = std::move(upload.data);
            session->pending_upload.reset();

            // Build index key/value (same format as small SEND)
            std::vector<uint8_t> idx_key;
            idx_key.reserve(64);
            idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
            idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

            std::vector<uint8_t> idx_value;
            idx_value.reserve(44);
            idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
            for (int i = 7; i >= 0; --i)
                idx_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
            uint32_t sz = static_cast<uint32_t>(blob_data.size());
            idx_value.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
            idx_value.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
            idx_value.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
            idx_value.push_back(static_cast<uint8_t>(sz & 0xFF));

            std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
            auto msg_id_copy = msg_id;

            // Build message_binary for Kademlia replication:
            // msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
            std::vector<uint8_t> message_binary;
            message_binary.reserve(32 + 32 + 8 + 4 + blob_data.size());
            message_binary.insert(message_binary.end(), msg_id.begin(), msg_id.end());
            message_binary.insert(message_binary.end(), sender_fp.begin(), sender_fp.end());
            for (int i = 7; i >= 0; --i)
                message_binary.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
            message_binary.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
            message_binary.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
            message_binary.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
            message_binary.push_back(static_cast<uint8_t>(sz & 0xFF));
            message_binary.insert(message_binary.end(), blob_data.begin(), blob_data.end());

            // Build kad_value: recipient_fp(32) || message_binary
            std::vector<uint8_t> kad_value;
            kad_value.reserve(32 + message_binary.size());
            kad_value.insert(kad_value.end(), recipient_fp.begin(), recipient_fp.end());
            kad_value.insert(kad_value.end(), message_binary.begin(), message_binary.end());

            auto inbox_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);

            // Dispatch to worker pool for storage
            if (!workers_.post([this, ws, send_id, idx_key = std::move(idx_key),
                           idx_value = std::move(idx_value),
                           blob_key = std::move(blob_key),
                           blob_data = std::move(blob_data),
                           kad_value = std::move(kad_value),
                           inbox_key,
                           msg_id_copy]() {
                bool ok = storage_.put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);
                if (ok) storage_.put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob_data);

                if (ok) {
                    kad_.store(inbox_key, 0x02, kad_value);

                    // Fire push notification directly — store_locally()
                    // will hit dedup (we pre-stored above) and skip
                    // on_store_, so we trigger the push explicitly.
                    on_kademlia_store(inbox_key, 0x02,
                                      std::span<const uint8_t>(kad_value));
                }

                loop_->defer([this, ws, send_id, ok, msg_id_copy]() {
                    if (connections_.count(ws) == 0) return;
                    if (ok) {
                        Json::Value resp;
                        resp["type"] = "OK";
                        resp["id"] = send_id;
                        resp["msg_id"] = to_hex(msg_id_copy);
                        send_json(ws, resp);
                    } else {
                        send_error(ws, send_id, 500, "store failed");
                    }
                });
            })) {
                send_error(ws, send_id, 503, "server overloaded");
                return;
            }
        }
    }
}

// ---------- auth handlers ----------

template<bool SSL>
void WsServer<SSL>::handle_hello(ws_t* ws, const Json::Value& msg) {
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
        // REDIRECT — query responsible nodes for their repl_log seq,
        // then return sorted by highest seq so client connects to most up-to-date.
        auto nodes = kad_.responsible_nodes(inbox_key);
        if (!workers_.post([this, ws, id, inbox_key, nodes]() {
            auto node_seqs = kad_.query_remote_seqs(inbox_key, nodes);

            loop_->defer([this, ws, id, node_seqs = std::move(node_seqs)]() {
                if (connections_.count(ws) == 0) return;

                Json::Value resp;
                resp["type"] = "REDIRECT";
                resp["id"] = id;
                Json::Value node_list(Json::arrayValue);
                for (const auto& ns : node_seqs) {
                    Json::Value n;
                    n["address"] = ns.node.address;
                    n["ws_port"] = ns.node.ws_port;
                    n["seq"] = static_cast<Json::UInt64>(ns.seq);
                    node_list.append(n);
                }
                resp["nodes"] = node_list;
                send_json(ws, resp);
                ws->close();
            });
        })) {
            send_error(ws, id, 503, "server overloaded");
        }
        return;
    }

    // Generate random 32-byte nonce for challenge (CSPRNG)
    crypto::Hash nonce{};
    OQS_randombytes(nonce.data(), nonce.size());

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

template<bool SSL>
void WsServer<SSL>::handle_auth(ws_t* ws, const Json::Value& msg) {
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

    // Verify signature over domain-separated data: "chromatin-auth:" || nonce
    const std::string prefix = "chromatin-auth:";
    std::vector<uint8_t> signed_data(prefix.begin(), prefix.end());
    signed_data.insert(signed_data.end(),
                       session->challenge_nonce.begin(),
                       session->challenge_nonce.end());
    if (!crypto::verify(signed_data, *sig_bytes, *pk_bytes)) {
        send_error(ws, id, 401, "invalid signature");
        return;
    }

    // Auth succeeded
    session->authenticated = true;
    session->pubkey = std::move(*pk_bytes);
    authenticated_[session->fingerprint].insert(ws);

    // Initialize per-fingerprint rate limiter if this is the first connection
    // for this fingerprint. Uses try_emplace to avoid overwriting an existing
    // limiter when a second device authenticates with the same identity.
    auto [it_fp, inserted] = fp_rate_limiters_.try_emplace(session->fingerprint);
    if (inserted) {
        it_fp->second.tokens = config::defaults::RATE_LIMIT_TOKENS;
        it_fp->second.max_tokens = config::defaults::RATE_LIMIT_MAX;
        it_fp->second.refill_rate = config::defaults::RATE_LIMIT_REFILL;
        it_fp->second.last_refill = std::chrono::steady_clock::now();
    }

    // Count pending inbox messages
    int pending = 0;
    storage_.scan(storage::TABLE_INBOX_INDEX, session->fingerprint,
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

template<bool SSL>
bool WsServer<SSL>::require_auth(ws_t* ws, int id) {
    auto* session = ws->getUserData();
    if (!session->authenticated) {
        send_error(ws, id, 401, "not authenticated");
        return false;
    }
    return true;
}

// ---------- command handlers ----------

template<bool SSL>
void WsServer<SSL>::handle_list(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // INDEX key: recipient_fp(32) || msg_id(32) = 64 bytes
    // INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE) = 44 bytes

    // Pagination: limit (default 50, max 200), after (msg_id hex to skip past)
    int limit = msg.get("limit", 50).asInt();
    if (limit <= 0) limit = 50;
    if (limit > 200) limit = 200;

    std::vector<uint8_t> after_id;
    std::string after_hex = msg.get("after", "").asString();
    if (after_hex.size() == 64) {
        auto parsed = from_hex(after_hex);
        if (parsed) after_id = std::move(*parsed);
    }
    bool skip_until_after = !after_id.empty();

    // Collect index entries (scan holds a read txn, can't nest another).
    struct IndexEntry {
        std::vector<uint8_t> msg_id;
        std::string from_hex;
        uint64_t timestamp;
        uint32_t size;
    };
    std::vector<IndexEntry> entries;
    bool has_more = false;

    storage_.scan(storage::TABLE_INBOX_INDEX, session->fingerprint,
                  [&](std::span<const uint8_t> key,
                      std::span<const uint8_t> value) -> bool {
        if (key.size() != 64) return true;
        if (value.size() != 44) return true;

        auto msg_id_span = key.subspan(32, 32);

        // Skip entries until we pass the "after" cursor
        if (skip_until_after) {
            if (std::equal(msg_id_span.begin(), msg_id_span.end(), after_id.begin())) {
                skip_until_after = false;
            }
            return true;
        }

        if (static_cast<int>(entries.size()) >= limit) {
            has_more = true;
            return false;  // stop scanning
        }

        IndexEntry e;
        e.msg_id.assign(msg_id_span.begin(), msg_id_span.end());
        e.from_hex = to_hex(value.subspan(0, 32));

        e.timestamp = 0;
        for (int i = 0; i < 8; ++i)
            e.timestamp = (e.timestamp << 8) | value[32 + i];

        e.size = (static_cast<uint32_t>(value[40]) << 24) |
                 (static_cast<uint32_t>(value[41]) << 16) |
                 (static_cast<uint32_t>(value[42]) << 8) |
                 static_cast<uint32_t>(value[43]);

        entries.push_back(std::move(e));
        return true;
    });

    // Now build the JSON response, fetching blobs outside the scan transaction.
    Json::Value messages(Json::arrayValue);
    for (const auto& e : entries) {
        Json::Value entry;
        entry["msg_id"] = to_hex(e.msg_id);
        entry["from"] = e.from_hex;
        entry["timestamp"] = Json::UInt64(e.timestamp);
        entry["size"] = e.size;

        if (e.size <= INLINE_THRESHOLD) {
            // Fetch blob from TABLE_MESSAGE_BLOBS and inline as base64
            auto blob = storage_.get(storage::TABLE_MESSAGE_BLOBS, e.msg_id);
            if (blob) {
                entry["blob"] = to_base64(*blob);
            } else {
                entry["blob"] = Json::nullValue;
            }
        } else {
            entry["blob"] = Json::nullValue;
        }

        messages.append(entry);
    }

    Json::Value resp;
    resp["type"] = "OK";
    resp["id"] = id;
    resp["messages"] = messages;
    resp["has_more"] = has_more;
    send_json(ws, resp);
}

template<bool SSL>
void WsServer<SSL>::handle_get(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Extract and validate msg_id (64 hex chars = 32 bytes)
    std::string msg_id_hex = msg.get("msg_id", "").asString();
    if (msg_id_hex.size() != 64) {
        send_error(ws, id, 400, "msg_id must be 64 hex chars");
        return;
    }
    auto msg_id_bytes = from_hex(msg_id_hex);
    if (!msg_id_bytes) {
        send_error(ws, id, 400, "invalid hex in msg_id");
        return;
    }

    // Authorization: verify the message belongs to this user's inbox
    std::vector<uint8_t> index_key;
    index_key.reserve(64);
    index_key.insert(index_key.end(),
                     session->fingerprint.begin(), session->fingerprint.end());
    index_key.insert(index_key.end(),
                     msg_id_bytes->begin(), msg_id_bytes->end());
    if (!storage_.get(storage::TABLE_INBOX_INDEX, index_key)) {
        send_error(ws, id, 404, "message not found");
        return;
    }

    // Look up the blob in TABLE_MESSAGE_BLOBS
    auto blob = storage_.get(storage::TABLE_MESSAGE_BLOBS, *msg_id_bytes);
    if (!blob) {
        send_error(ws, id, 404, "message not found");
        return;
    }



    if (blob->size() <= INLINE_THRESHOLD) {
        Json::Value resp;
        resp["type"] = "OK";
        resp["id"] = id;
        resp["msg_id"] = msg_id_hex;
        resp["blob"] = to_base64(*blob);
        send_json(ws, resp);
    } else {
        // Chunked download: send JSON header then binary DOWNLOAD_CHUNK frames
        static constexpr size_t CHUNK_SIZE = 1048576;  // 1 MiB
        uint32_t num_chunks = static_cast<uint32_t>(
            (blob->size() + CHUNK_SIZE - 1) / CHUNK_SIZE);
        uint32_t request_id = next_request_id_.fetch_add(1);

        // Send JSON header with size and chunk count
        Json::Value resp;
        resp["type"] = "OK";
        resp["id"] = id;
        resp["msg_id"] = msg_id_hex;
        resp["size"] = static_cast<Json::UInt>(blob->size());
        resp["chunks"] = num_chunks;
        send_json(ws, resp);

        // Send binary DOWNLOAD_CHUNK frames
        // Frame format: [0x02][4B request_id BE][2B chunk_index BE][payload]
        for (uint32_t i = 0; i < num_chunks; ++i) {
            size_t offset = static_cast<size_t>(i) * CHUNK_SIZE;
            size_t payload_size = std::min(CHUNK_SIZE, blob->size() - offset);

            std::vector<uint8_t> frame;
            frame.reserve(7 + payload_size);
            frame.push_back(0x02);  // DOWNLOAD_CHUNK frame type
            frame.push_back(static_cast<uint8_t>((request_id >> 24) & 0xFF));
            frame.push_back(static_cast<uint8_t>((request_id >> 16) & 0xFF));
            frame.push_back(static_cast<uint8_t>((request_id >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(request_id & 0xFF));
            uint16_t chunk_index = static_cast<uint16_t>(i);
            frame.push_back(static_cast<uint8_t>((chunk_index >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(chunk_index & 0xFF));
            frame.insert(frame.end(),
                         blob->data() + offset,
                         blob->data() + offset + payload_size);

            std::string_view sv(reinterpret_cast<const char*>(frame.data()),
                                frame.size());
            ws->send(sv, uWS::OpCode::BINARY);
        }
    }
}

template<bool SSL>
void WsServer<SSL>::handle_send(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse recipient fingerprint (needed by both small and large paths)
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

    // Detect large vs small path:
    // If "size" field is present and "blob" is empty/missing -> large chunked upload
    // Otherwise -> small inline SEND
    std::string blob_b64 = msg.get("blob", "").asString();
    bool has_size = msg.isMember("size") && msg["size"].isUInt64();

    if (has_size && blob_b64.empty()) {
        // ---- Large chunked SEND path ----
        uint64_t declared_size = msg["size"].asUInt64();

        if (declared_size > config::protocol::MAX_MESSAGE_SIZE) {
            send_error(ws, id, 413, "attachment too large");
            return;
        }

        // Check allowlist: sender must be allowed to write to recipient's inbox.
        // If no allowlist exists for the recipient, inbox is open (anyone can send).
        // This is a best-effort local check — real enforcement is at the Kademlia
        // STORE validation on the recipient's responsible nodes.
        {
            auto allowlist_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);
            std::vector<uint8_t> allow_check;
            allow_check.reserve(64);
            allow_check.insert(allow_check.end(), allowlist_key.begin(), allowlist_key.end());
            allow_check.insert(allow_check.end(),
                               session->fingerprint.begin(), session->fingerprint.end());
            auto allowed = storage_.get(storage::TABLE_ALLOWLISTS, allow_check);
            bool sender_allowed = allowed && allowed->size() > 64 && (*allowed)[64] == 0x01;
            if (!sender_allowed) {
                // Check if any allowlist exists for this recipient
                bool has_allowlist = false;
                storage_.scan(storage::TABLE_ALLOWLISTS,
                              std::vector<uint8_t>(allowlist_key.begin(), allowlist_key.end()),
                              [&](auto, auto) { has_allowlist = true; return false; });
                if (has_allowlist) {
                    send_error(ws, id, 403, "not on allowlist");
                    return;
                }
                // No allowlist at all = open inbox, allow the message
            }
        }

        if (session->pending_upload) {
            send_error(ws, id, 429, "upload already in progress");
            return;
        }

        uint32_t request_id = next_request_id_.fetch_add(1);

        Session::PendingUpload upload;
        upload.request_id = request_id;
        upload.recipient_fp = recipient_fp;
        upload.id = id;
        upload.expected_size = static_cast<uint32_t>(declared_size);
        upload.received = 0;
        upload.next_chunk = 0;
        upload.started = std::chrono::steady_clock::now();
        session->pending_upload = std::move(upload);

        Json::Value resp;
        resp["type"] = "SEND_READY";
        resp["id"] = id;
        resp["request_id"] = request_id;
        send_json(ws, resp);
        return;
    }

    // ---- Small inline SEND path ----
    if (blob_b64.empty()) {
        send_error(ws, id, 400, "missing blob");
        return;
    }
    auto blob = from_base64(blob_b64);
    if (!blob) {
        send_error(ws, id, 400, "invalid base64 in blob");
        return;
    }
    if (blob->size() > INLINE_THRESHOLD) {
        send_error(ws, id, 400, "blob exceeds inline threshold, use chunked upload");
        return;
    }

    // Check allowlist: sender must be allowed to write to recipient's inbox.
    // If no allowlist exists for the recipient, inbox is open (anyone can send).
    {
        auto allowlist_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);
        std::vector<uint8_t> allow_check;
        allow_check.reserve(64);
        allow_check.insert(allow_check.end(), allowlist_key.begin(), allowlist_key.end());
        allow_check.insert(allow_check.end(),
                           session->fingerprint.begin(), session->fingerprint.end());
        auto allowed = storage_.get(storage::TABLE_ALLOWLISTS, allow_check);
        bool sender_allowed = allowed && allowed->size() > 64 && (*allowed)[64] == 0x01;
        if (!sender_allowed) {
            // Check if any allowlist exists for this recipient
            bool has_allowlist = false;
            storage_.scan(storage::TABLE_ALLOWLISTS,
                          std::vector<uint8_t>(allowlist_key.begin(), allowlist_key.end()),
                          [&](auto, auto) { has_allowlist = true; return false; });
            if (has_allowlist) {
                send_error(ws, id, 403, "not on allowlist");
                return;
            }
            // No allowlist at all = open inbox, allow the message
        }
    }

    // Generate random 32-byte msg_id
    crypto::Hash msg_id{};
    OQS_randombytes(msg_id.data(), msg_id.size());

    // Timestamp (milliseconds since epoch)
    auto now = std::chrono::system_clock::now();
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
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

    // Build INDEX key: recipient_fp(32) || msg_id(32) = 64 bytes
    std::vector<uint8_t> index_key;
    index_key.reserve(64);
    index_key.insert(index_key.end(),
                     recipient_fp.begin(), recipient_fp.end());
    index_key.insert(index_key.end(), msg_id.begin(), msg_id.end());

    // Build INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE) = 44 bytes
    std::vector<uint8_t> index_value;
    index_value.reserve(44);
    index_value.insert(index_value.end(),
                       session->fingerprint.begin(), session->fingerprint.end());
    for (int i = 7; i >= 0; --i) {
        index_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    index_value.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    index_value.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    index_value.push_back(static_cast<uint8_t>(blob_len & 0xFF));

    // BLOB key: msg_id(32), value: raw blob bytes
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    auto blob_value = std::move(*blob);

    // Compute inbox_key = SHA3-256("inbox:" || recipient_fp)
    // Used for Kademlia routing (responsibility check) and replication.
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);

    // Capture values needed by the worker/deferred callbacks
    auto msg_id_copy = msg_id;

    // Dispatch to worker pool
    if (!workers_.post([this, ws, id, inbox_key,
                   index_key = std::move(index_key),
                   index_value = std::move(index_value),
                   blob_key = std::move(blob_key),
                   blob_value = std::move(blob_value),
                   message_binary = std::move(message_binary),
                   msg_id_copy]() {
        // Store locally in the two-table model so LIST can scan by fingerprint
        bool ok = storage_.put(storage::TABLE_INBOX_INDEX,
                               index_key, index_value);
        if (ok) {
            ok = storage_.put(storage::TABLE_MESSAGE_BLOBS,
                              blob_key, blob_value);
        }

        // Replicate via Kademlia: prepend recipient_fp so receiving nodes
        // can build the two-table inbox model (INDEX key needs recipient_fp).
        if (ok) {
            auto inbox_key_local = inbox_key;
            // Retrieve recipient_fp from index_key (first 32 bytes)
            std::vector<uint8_t> kad_value;
            kad_value.reserve(32 + message_binary.size());
            kad_value.insert(kad_value.end(), index_key.begin(), index_key.begin() + 32);
            kad_value.insert(kad_value.end(), message_binary.begin(), message_binary.end());
            kad_.store(inbox_key_local, 0x02, kad_value);

            // Fire push notification directly — store_locally() will
            // hit dedup (we pre-stored above) and skip on_store_,
            // so we trigger the push explicitly.
            on_kademlia_store(inbox_key_local, 0x02,
                              std::span<const uint8_t>(kad_value));
        }

        // Defer response back to the uWS event loop thread
        loop_->defer([this, ws, id, ok, msg_id_copy]() {
            // Check if the connection is still alive
            if (connections_.count(ws) == 0) return;

            if (ok) {
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                resp["msg_id"] = to_hex(msg_id_copy);
                send_json(ws, resp);
            } else {
                send_error(ws, id, 500, "store failed");
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

// ---------- allowlist handlers ----------

template<bool SSL>
void WsServer<SSL>::handle_allow(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse allowed contact fingerprint (64 hex chars = 32 bytes)
    std::string fp_hex = msg.get("fingerprint", "").asString();
    if (fp_hex.size() != 64) {
        send_error(ws, id, 400, "fingerprint must be 64 hex chars");
        return;
    }
    auto fp_bytes = from_hex(fp_hex);
    if (!fp_bytes) {
        send_error(ws, id, 400, "invalid hex in fingerprint");
        return;
    }
    crypto::Hash allowed_fp{};
    std::copy(fp_bytes->begin(), fp_bytes->end(), allowed_fp.begin());

    // Parse sequence number
    if (!msg.isMember("sequence") || !msg["sequence"].isUInt64()) {
        send_error(ws, id, 400, "missing or invalid sequence");
        return;
    }
    uint64_t sequence = msg["sequence"].asUInt64();

    // Parse signature
    std::string sig_hex = msg.get("signature", "").asString();
    if (sig_hex.empty()) {
        send_error(ws, id, 400, "missing signature");
        return;
    }
    auto sig_bytes = from_hex(sig_hex);
    if (!sig_bytes) {
        send_error(ws, id, 400, "invalid hex in signature");
        return;
    }
    if (sig_bytes->size() != crypto::SIGNATURE_SIZE) {
        send_error(ws, id, 400, "invalid signature size");
        return;
    }

    // Build signed data with domain separation:
    // "chromatin:allowlist:" || owner_fp(32) || action(1) || allowed_fp(32) || sequence(8 BE)
    const std::string domain = "chromatin:allowlist:";
    std::vector<uint8_t> signed_data;
    signed_data.reserve(domain.size() + 32 + 1 + 32 + 8);
    signed_data.insert(signed_data.end(), domain.begin(), domain.end());
    signed_data.insert(signed_data.end(), session->fingerprint.begin(), session->fingerprint.end());
    signed_data.push_back(0x01);  // action = allow
    signed_data.insert(signed_data.end(), allowed_fp.begin(), allowed_fp.end());
    for (int i = 7; i >= 0; --i) {
        signed_data.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    }

    // Verify ML-DSA-87 signature using session's pubkey
    if (!crypto::verify(signed_data, *sig_bytes, session->pubkey)) {
        send_error(ws, id, 401, "invalid signature");
        return;
    }

    // Compute allowlist_key = SHA3-256("inbox:" || owner_fp) — co-located with inbox
    auto allowlist_key = crypto::sha3_256_prefixed("inbox:", session->fingerprint);

    // Build storage key: allowlist_key(32) || allowed_fp(32) = 64 bytes
    std::vector<uint8_t> storage_key;
    storage_key.reserve(64);
    storage_key.insert(storage_key.end(), allowlist_key.begin(), allowlist_key.end());
    storage_key.insert(storage_key.end(), allowed_fp.begin(), allowed_fp.end());

    // Check sequence > currently stored sequence
    auto existing = storage_.get(storage::TABLE_ALLOWLISTS, storage_key);
    if (existing) {
        // Stored format: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
        if (existing->size() >= 73) {
            uint64_t stored_seq = 0;
            for (int i = 0; i < 8; ++i) {
                stored_seq = (stored_seq << 8) | (*existing)[65 + i];
            }
            if (sequence <= stored_seq) {
                send_error(ws, id, 400, "sequence must be greater than current");
                return;
            }
        }
    }

    // Build full allowlist entry value: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
    uint16_t pk_len = static_cast<uint16_t>(session->pubkey.size());
    std::vector<uint8_t> entry_value;
    entry_value.reserve(32 + 32 + 1 + 8 + 2 + pk_len + sig_bytes->size());
    entry_value.insert(entry_value.end(), session->fingerprint.begin(), session->fingerprint.end());
    entry_value.insert(entry_value.end(), allowed_fp.begin(), allowed_fp.end());
    entry_value.push_back(0x01);  // action = allow
    for (int i = 7; i >= 0; --i) {
        entry_value.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    }
    entry_value.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    entry_value.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    entry_value.insert(entry_value.end(), session->pubkey.begin(), session->pubkey.end());
    entry_value.insert(entry_value.end(), sig_bytes->begin(), sig_bytes->end());

    // Dispatch to worker pool
    if (!workers_.post([this, ws, id, allowlist_key, allowed_fp,
                   storage_key = std::move(storage_key),
                   entry_value = std::move(entry_value)]() {
        bool ok = storage_.put(storage::TABLE_ALLOWLISTS, storage_key, entry_value);

        if (ok) {
            // entry_value already has the full format for Kademlia replication
            kad_.store(allowlist_key, 0x04, entry_value);
        }

        loop_->defer([this, ws, id, ok]() {
            if (connections_.count(ws) == 0) return;

            if (ok) {
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                send_json(ws, resp);
            } else {
                send_error(ws, id, 500, "store failed");
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

template<bool SSL>
void WsServer<SSL>::handle_revoke(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse allowed contact fingerprint (64 hex chars = 32 bytes)
    std::string fp_hex = msg.get("fingerprint", "").asString();
    if (fp_hex.size() != 64) {
        send_error(ws, id, 400, "fingerprint must be 64 hex chars");
        return;
    }
    auto fp_bytes = from_hex(fp_hex);
    if (!fp_bytes) {
        send_error(ws, id, 400, "invalid hex in fingerprint");
        return;
    }
    crypto::Hash allowed_fp{};
    std::copy(fp_bytes->begin(), fp_bytes->end(), allowed_fp.begin());

    // Parse sequence number
    if (!msg.isMember("sequence") || !msg["sequence"].isUInt64()) {
        send_error(ws, id, 400, "missing or invalid sequence");
        return;
    }
    uint64_t sequence = msg["sequence"].asUInt64();

    // Parse signature
    std::string sig_hex = msg.get("signature", "").asString();
    if (sig_hex.empty()) {
        send_error(ws, id, 400, "missing signature");
        return;
    }
    auto sig_bytes = from_hex(sig_hex);
    if (!sig_bytes) {
        send_error(ws, id, 400, "invalid hex in signature");
        return;
    }
    if (sig_bytes->size() != crypto::SIGNATURE_SIZE) {
        send_error(ws, id, 400, "invalid signature size");
        return;
    }

    // Build signed data with domain separation:
    // "chromatin:allowlist:" || owner_fp(32) || action(1) || allowed_fp(32) || sequence(8 BE)
    const std::string domain = "chromatin:allowlist:";
    std::vector<uint8_t> signed_data;
    signed_data.reserve(domain.size() + 32 + 1 + 32 + 8);
    signed_data.insert(signed_data.end(), domain.begin(), domain.end());
    signed_data.insert(signed_data.end(), session->fingerprint.begin(), session->fingerprint.end());
    signed_data.push_back(0x00);  // action = revoke
    signed_data.insert(signed_data.end(), allowed_fp.begin(), allowed_fp.end());
    for (int i = 7; i >= 0; --i) {
        signed_data.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    }

    // Verify ML-DSA-87 signature using session's pubkey
    if (!crypto::verify(signed_data, *sig_bytes, session->pubkey)) {
        send_error(ws, id, 401, "invalid signature");
        return;
    }

    // Compute allowlist_key = SHA3-256("inbox:" || owner_fp) — co-located with inbox
    auto allowlist_key = crypto::sha3_256_prefixed("inbox:", session->fingerprint);

    // Build storage key: allowlist_key(32) || allowed_fp(32) = 64 bytes
    std::vector<uint8_t> storage_key;
    storage_key.reserve(64);
    storage_key.insert(storage_key.end(), allowlist_key.begin(), allowlist_key.end());
    storage_key.insert(storage_key.end(), allowed_fp.begin(), allowed_fp.end());

    // Check sequence > currently stored sequence
    auto existing = storage_.get(storage::TABLE_ALLOWLISTS, storage_key);
    if (existing) {
        // Stored format: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
        if (existing->size() >= 73) {
            uint64_t stored_seq = 0;
            for (int i = 0; i < 8; ++i) {
                stored_seq = (stored_seq << 8) | (*existing)[65 + i];
            }
            if (sequence <= stored_seq) {
                send_error(ws, id, 400, "sequence must be greater than current");
                return;
            }
        }
    }

    // Capture owner_fp and pubkey for the worker lambda
    crypto::Hash owner_fp = session->fingerprint;
    std::vector<uint8_t> owner_pubkey(session->pubkey.begin(), session->pubkey.end());

    // Dispatch to worker pool — store revoke entry and replicate
    if (!workers_.post([this, ws, id, allowlist_key, allowed_fp, owner_fp,
                   sig_bytes = std::move(*sig_bytes),
                   owner_pubkey = std::move(owner_pubkey),
                   sequence]() {
        // Build revoke entry for replication: owner_fp(32) || allowed_fp(32) || action(0x00) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
        uint16_t pk_len = static_cast<uint16_t>(owner_pubkey.size());
        std::vector<uint8_t> revoke_value;
        revoke_value.reserve(32 + 32 + 1 + 8 + 2 + pk_len + sig_bytes.size());
        revoke_value.insert(revoke_value.end(), owner_fp.begin(), owner_fp.end());
        revoke_value.insert(revoke_value.end(), allowed_fp.begin(), allowed_fp.end());
        revoke_value.push_back(0x00);  // action = revoke
        for (int i = 7; i >= 0; --i) {
            revoke_value.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
        }
        revoke_value.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        revoke_value.push_back(static_cast<uint8_t>(pk_len & 0xFF));
        revoke_value.insert(revoke_value.end(), owner_pubkey.begin(), owner_pubkey.end());
        revoke_value.insert(revoke_value.end(), sig_bytes.begin(), sig_bytes.end());

        kad_.store(allowlist_key, 0x04, revoke_value);

        loop_->defer([this, ws, id]() {
            if (connections_.count(ws) == 0) return;

            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

// ---------- CONTACT_REQUEST handler ----------

template<bool SSL>
void WsServer<SSL>::handle_contact_request(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse recipient fingerprint (64 hex chars = 32 bytes)
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

    // Parse blob (base64, max 64 KiB)
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
    if (blob->size() > config::protocol::MAX_REQUEST_BLOB_SIZE) {
        send_error(ws, id, 400, "blob exceeds max size");
        return;
    }

    // Parse pow_nonce
    if (!msg.isMember("pow_nonce") || !msg["pow_nonce"].isUInt64()) {
        send_error(ws, id, 400, "missing or invalid pow_nonce");
        return;
    }
    uint64_t pow_nonce = msg["pow_nonce"].asUInt64();

    // Parse client-provided timestamp (milliseconds since epoch)
    if (!msg.isMember("timestamp") || !msg["timestamp"].isUInt64()) {
        send_error(ws, id, 400, "missing or invalid timestamp");
        return;
    }
    uint64_t timestamp = msg["timestamp"].asUInt64();

    // Validate timestamp: must be within 1 hour of current time
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    constexpr uint64_t MAX_TIMESTAMP_DRIFT = 3'600'000;  // 1 hour in ms
    if (timestamp > now + MAX_TIMESTAMP_DRIFT || now > timestamp + MAX_TIMESTAMP_DRIFT) {
        send_error(ws, id, 400, "timestamp too far from server time");
        return;
    }

    // Verify PoW with domain separation:
    // preimage = "chromatin:request:" || sender_fp || recipient_fp || timestamp(8 BE)
    std::vector<uint8_t> preimage;
    const std::string prefix = "chromatin:request:";
    preimage.reserve(prefix.size() + 32 + 32 + 8);
    preimage.insert(preimage.end(), prefix.begin(), prefix.end());
    preimage.insert(preimage.end(),
                    session->fingerprint.begin(), session->fingerprint.end());
    preimage.insert(preimage.end(), recipient_fp.begin(), recipient_fp.end());
    for (int i = 7; i >= 0; --i) {
        preimage.push_back(
            static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }

    if (!crypto::verify_pow(preimage, pow_nonce, config::protocol::CONTACT_POW_DIFFICULTY)) {
        send_error(ws, id, 400, "invalid PoW");
        return;
    }

    // Build contact request binary:
    // recipient_fp(32) || sender_fp(32) || pow_nonce(8 BE) || timestamp(8 BE) || blob_length(4 BE) || blob
    uint32_t blob_len = static_cast<uint32_t>(blob->size());
    std::vector<uint8_t> request_binary;
    request_binary.reserve(32 + 32 + 8 + 8 + 4 + blob->size());
    request_binary.insert(request_binary.end(),
                          recipient_fp.begin(), recipient_fp.end());
    request_binary.insert(request_binary.end(),
                          session->fingerprint.begin(), session->fingerprint.end());
    for (int i = 7; i >= 0; --i) {
        request_binary.push_back(
            static_cast<uint8_t>((pow_nonce >> (i * 8)) & 0xFF));
    }
    for (int i = 7; i >= 0; --i) {
        request_binary.push_back(
            static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }
    request_binary.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    request_binary.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    request_binary.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    request_binary.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    request_binary.insert(request_binary.end(), blob->begin(), blob->end());

    // Compute requests_key = SHA3-256("inbox:" || recipient_fp)
    // Uses "inbox:" prefix to co-locate with inbox data on the same responsible nodes.
    auto requests_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);

    // Dispatch to worker pool
    if (!workers_.post([this, ws, id, requests_key,
                   request_binary = std::move(request_binary)]() {
        kad_.store(requests_key, 0x03, request_binary);

        loop_->defer([this, ws, id]() {
            if (connections_.count(ws) == 0) return;

            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

// ---------- DELETE handler ----------

template<bool SSL>
void WsServer<SSL>::handle_delete(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    if (!msg.isMember("msg_ids") || !msg["msg_ids"].isArray()) {
        send_error(ws, id, 400, "missing msg_ids array");
        return;
    }

    // Parse msg_ids on the event loop (lightweight)
    std::vector<std::vector<uint8_t>> parsed_ids;
    const auto& ids = msg["msg_ids"];
    for (const auto& mid : ids) {
        std::string mid_hex = mid.asString();
        if (mid_hex.size() != 64) continue;
        auto mid_bytes = from_hex(mid_hex);
        if (!mid_bytes) continue;
        parsed_ids.push_back(std::move(*mid_bytes));
    }

    auto fp = session->fingerprint;

    // Compute inbox routing key for repl_log
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", fp);

    // Dispatch storage ops to worker pool
    if (!workers_.post([this, ws, id, fp, inbox_key, parsed_ids = std::move(parsed_ids)]() {
        for (const auto& mid_bytes : parsed_ids) {
            // Delete from TABLE_INBOX_INDEX: key = fingerprint(32) || msg_id(32)
            std::vector<uint8_t> idx_key;
            idx_key.reserve(64);
            idx_key.insert(idx_key.end(), fp.begin(), fp.end());
            idx_key.insert(idx_key.end(), mid_bytes.begin(), mid_bytes.end());
            storage_.del(storage::TABLE_INBOX_INDEX, idx_key);

            // Delete from TABLE_MESSAGE_BLOBS: key = msg_id(32)
            storage_.del(storage::TABLE_MESSAGE_BLOBS, mid_bytes);

            // Replicate deletion: record DEL in repl_log and Kademlia sync
            std::vector<uint8_t> delete_info;
            delete_info.reserve(64);
            delete_info.insert(delete_info.end(), fp.begin(), fp.end());
            delete_info.insert(delete_info.end(), mid_bytes.begin(), mid_bytes.end());
            kad_.delete_value(inbox_key, 0x02, delete_info);
        }

        loop_->defer([this, ws, id]() {
            if (connections_.count(ws) == 0) return;

            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

// ---------- push notifications ----------

template<bool SSL>
void WsServer<SSL>::on_kademlia_store(const crypto::Hash& key,
                                      uint8_t data_type,
                                      std::span<const uint8_t> value) {
    // Push inbox messages (0x02), contact requests (0x03), group messages (0x05)
    if (data_type != 0x02 && data_type != 0x03 && data_type != 0x05) return;

    // Copy value for the deferred lambda (span may be invalidated)
    std::vector<uint8_t> value_copy(value.begin(), value.end());
    auto key_copy = key;

    loop_->defer([this, key_copy, data_type, value_copy = std::move(value_copy)]() {
        if (data_type == 0x05) {
            // NEW_GROUP_MESSAGE push
            // Value: group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob
            if (value_copy.size() < 112) return;
            crypto::Hash group_id{};
            std::copy(value_copy.begin(), value_copy.begin() + 32, group_id.begin());
            auto sender = std::span<const uint8_t>(value_copy.data() + 32, 32);
            auto msg_id = std::span<const uint8_t>(value_copy.data() + 64, 32);
            uint32_t gek_version = (static_cast<uint32_t>(value_copy[104]) << 24) |
                                   (static_cast<uint32_t>(value_copy[105]) << 16) |
                                   (static_cast<uint32_t>(value_copy[106]) << 8) |
                                   static_cast<uint32_t>(value_copy[107]);
            uint32_t blob_len = (static_cast<uint32_t>(value_copy[108]) << 24) |
                                (static_cast<uint32_t>(value_copy[109]) << 16) |
                                (static_cast<uint32_t>(value_copy[110]) << 8) |
                                static_cast<uint32_t>(value_copy[111]);

            const auto* meta = get_group_meta(group_id);
            if (!meta) return;

            Json::Value push;
            push["type"] = "NEW_GROUP_MESSAGE";
            push["group_id"] = to_hex(group_id);
            push["msg_id"] = to_hex(msg_id);
            push["sender"] = to_hex(sender);
            push["size"] = blob_len;
            push["gek_version"] = gek_version;

            for (const auto& member : meta->members) {
                auto it = authenticated_.find(member.fingerprint);
                if (it == authenticated_.end()) continue;
                for (auto* client_ws : it->second) {
                    if (connections_.count(client_ws) > 0) {
                        send_json(client_ws, push);
                    }
                }
            }
            return;
        }

        // Extract recipient fingerprint from value (first 32 bytes for both 0x02 and 0x03)
        if (value_copy.size() < 32) return;
        crypto::Hash recipient_fp{};
        std::copy(value_copy.begin(), value_copy.begin() + 32, recipient_fp.begin());

        auto it = authenticated_.find(recipient_fp);
        if (it == authenticated_.end()) return;

        if (data_type == 0x02) {
            // NEW_MESSAGE push
            // Value layout: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
            if (value_copy.size() < 108) return;
            auto msg_id = std::span<const uint8_t>(value_copy.data() + 32, 32);
            auto sender = std::span<const uint8_t>(value_copy.data() + 64, 32);
            uint64_t ts = 0;
            for (int i = 0; i < 8; ++i) {
                ts = (ts << 8) | value_copy[96 + i];
            }
            uint32_t blob_len = (static_cast<uint32_t>(value_copy[104]) << 24) |
                                (static_cast<uint32_t>(value_copy[105]) << 16) |
                                (static_cast<uint32_t>(value_copy[106]) << 8) |
                                static_cast<uint32_t>(value_copy[107]);

            if (value_copy.size() < 108 + blob_len) return;
            auto blob = std::span<const uint8_t>(value_copy.data() + 108, blob_len);

            Json::Value push;
            push["type"] = "NEW_MESSAGE";
            push["msg_id"] = to_hex(msg_id);
            push["from"] = to_hex(sender);
            push["timestamp"] = Json::UInt64(ts);
            push["size"] = blob_len;

            if (blob_len <= INLINE_THRESHOLD) {
                push["blob"] = to_base64(blob);
            } else {
                push["blob"] = Json::nullValue;
            }

            for (auto* client_ws : it->second) {
                if (connections_.count(client_ws) > 0) {
                    send_json(client_ws, push);
                }
            }
        } else {
            // CONTACT_REQUEST push
            // Value layout: recipient_fp(32) || sender_fp(32) || pow_nonce(8 BE) || timestamp(8 BE) || blob_length(4 BE) || blob
            if (value_copy.size() < 84) return;
            auto sender = std::span<const uint8_t>(value_copy.data() + 32, 32);
            uint32_t cr_blob_len = (static_cast<uint32_t>(value_copy[80]) << 24) |
                                   (static_cast<uint32_t>(value_copy[81]) << 16) |
                                   (static_cast<uint32_t>(value_copy[82]) << 8) |
                                   static_cast<uint32_t>(value_copy[83]);
            if (value_copy.size() < 84 + cr_blob_len) return;
            auto blob = std::span<const uint8_t>(
                value_copy.data() + 84, cr_blob_len);

            Json::Value push;
            push["type"] = "CONTACT_REQUEST";
            push["from"] = to_hex(sender);
            push["blob"] = to_base64(blob);

            for (auto* client_ws : it->second) {
                if (connections_.count(client_ws) > 0) {
                    send_json(client_ws, push);
                }
            }
        }
    });
}

// ---------- STATUS ----------

template<bool SSL>
void WsServer<SSL>::handle_status(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

    Json::Value resp;
    resp["type"] = "OK";
    resp["id"] = id;
    resp["node_id"] = to_hex(kad_.self().id.id);
    resp["uptime_seconds"] = static_cast<Json::Int64>(uptime);
    resp["connected_clients"] = static_cast<Json::UInt64>(connections_.size());
    resp["authenticated_clients"] = static_cast<Json::UInt64>(authenticated_.size());
    resp["routing_table_size"] = static_cast<Json::UInt64>(kad_.routing_table_size());
    send_json(ws, resp);
}

// ---------- RESOLVE_NAME handler ----------

template<bool SSL>
void WsServer<SSL>::handle_resolve_name(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();

    std::string name = msg.get("name", "").asString();
    if (name.empty()) {
        send_error(ws, id, 400, "missing name");
        return;
    }

    // Look up in TABLE_NAMES: key = SHA3-256("name:" || name)
    std::vector<uint8_t> name_bytes(name.begin(), name.end());
    auto name_key = crypto::sha3_256_prefixed("name:", name_bytes);

    // Quorum read: query all R responsible nodes for the name record,
    // then apply lower-fingerprint tiebreaker for conflict resolution.
    auto nodes = kad_.responsible_nodes(name_key);

    if (!workers_.post([this, ws, id, name_key, nodes]() {
        auto results = kad_.query_remote_values(name_key, nodes);

        // Helper: extract fingerprint from a name record
        // Format: name_len(1) || name(N) || fingerprint(32) || ...
        auto extract_fp = [](const std::vector<uint8_t>& record) -> std::optional<crypto::Hash> {
            if (record.empty()) return std::nullopt;
            uint8_t name_len = record[0];
            size_t fp_offset = 1 + name_len;
            if (record.size() < fp_offset + 32) return std::nullopt;
            crypto::Hash fp{};
            std::copy_n(record.data() + fp_offset, 32, fp.begin());
            return fp;
        };

        // Collect all valid name records with their fingerprints
        std::optional<crypto::Hash> winner_fp;
        std::optional<std::vector<uint8_t>> winner_record;

        for (const auto& nv : results) {
            if (!nv.value || nv.value->empty()) continue;
            auto fp = extract_fp(*nv.value);
            if (!fp) continue;

            // Lower fingerprint wins (deterministic tiebreaker)
            if (!winner_fp || *fp < *winner_fp) {
                winner_fp = *fp;
                winner_record = nv.value;
            }
        }

        loop_->defer([this, ws, id, winner_fp, winner_record]() {
            if (connections_.count(ws) == 0) return;

            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;

            if (!winner_fp) {
                resp["found"] = false;
            } else {
                resp["found"] = true;
                resp["fingerprint"] = to_hex(*winner_fp);
            }
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
    }
}

// ---------- GET_PROFILE handler ----------

template<bool SSL>
void WsServer<SSL>::handle_get_profile(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();

    std::string fp_hex = msg.get("fingerprint", "").asString();
    if (fp_hex.size() != 64) {
        send_error(ws, id, 400, "fingerprint must be 64 hex chars");
        return;
    }
    auto fp_bytes = from_hex(fp_hex);
    if (!fp_bytes) {
        send_error(ws, id, 400, "invalid hex in fingerprint");
        return;
    }
    crypto::Hash fingerprint{};
    std::copy(fp_bytes->begin(), fp_bytes->end(), fingerprint.begin());

    // Look up in TABLE_PROFILES: key = SHA3-256("profile:" || fingerprint)
    auto profile_key = crypto::sha3_256_prefixed("profile:", fingerprint);

    auto profile = storage_.get(storage::TABLE_PROFILES,
        std::vector<uint8_t>(profile_key.begin(), profile_key.end()));

    Json::Value resp;
    resp["type"] = "OK";
    resp["id"] = id;

    if (!profile || profile->empty()) {
        resp["found"] = false;
        send_json(ws, resp);
        return;
    }

    resp["found"] = true;

    // Parse profile binary:
    // fingerprint(32) || pubkey_len(2 BE) || pubkey || kem_pubkey_len(2 BE) || kem_pubkey
    // || bio_len(2 BE) || bio || avatar_len(4 BE) || avatar || social_count(1)
    // || [platform_len(1) || platform || handle_len(1) || handle]...
    // || sequence(8 BE) || sig_len(2 BE) || signature
    const auto& data = *profile;
    size_t offset = 0;

    if (data.size() < 53) {
        resp["found"] = false;
        send_json(ws, resp);
        return;
    }

    // fingerprint (32)
    resp["fingerprint"] = to_hex(std::span<const uint8_t>(data.data(), 32));
    offset += 32;

    // pubkey_len + pubkey
    uint16_t pk_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    offset += 2;
    if (offset + pk_len > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    resp["pubkey"] = to_hex(std::span<const uint8_t>(data.data() + offset, pk_len));
    offset += pk_len;

    // kem_pubkey_len + kem_pubkey
    if (offset + 2 > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    uint16_t kem_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    offset += 2;
    if (offset + kem_len > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    resp["kem_pubkey"] = to_hex(std::span<const uint8_t>(data.data() + offset, kem_len));
    offset += kem_len;

    // bio_len + bio
    if (offset + 2 > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    uint16_t bio_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    offset += 2;
    if (offset + bio_len > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    resp["bio"] = std::string(reinterpret_cast<const char*>(data.data() + offset), bio_len);
    offset += bio_len;

    // avatar_len + avatar (base64)
    if (offset + 4 > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    uint32_t avatar_len = (static_cast<uint32_t>(data[offset]) << 24)
                        | (static_cast<uint32_t>(data[offset + 1]) << 16)
                        | (static_cast<uint32_t>(data[offset + 2]) << 8)
                        | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    if (offset + avatar_len > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    if (avatar_len > 0) {
        resp["avatar"] = to_base64(std::span<const uint8_t>(data.data() + offset, avatar_len));
    }
    offset += avatar_len;

    // social links
    if (offset + 1 > data.size()) { resp["found"] = false; send_json(ws, resp); return; }
    uint8_t social_count = data[offset];
    offset += 1;
    Json::Value socials(Json::arrayValue);
    for (uint8_t i = 0; i < social_count; ++i) {
        if (offset + 1 > data.size()) break;
        uint8_t platform_len = data[offset]; offset += 1;
        if (offset + platform_len > data.size()) break;
        std::string platform(reinterpret_cast<const char*>(data.data() + offset), platform_len);
        offset += platform_len;
        if (offset + 1 > data.size()) break;
        uint8_t handle_len = data[offset]; offset += 1;
        if (offset + handle_len > data.size()) break;
        std::string handle(reinterpret_cast<const char*>(data.data() + offset), handle_len);
        offset += handle_len;

        Json::Value link;
        link["platform"] = platform;
        link["handle"] = handle;
        socials.append(link);
    }
    resp["social_links"] = socials;

    send_json(ws, resp);
}

// ---------- LIST_REQUESTS handler ----------

template<bool SSL>
void WsServer<SSL>::handle_list_requests(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Prefix scan TABLE_REQUESTS with recipient_fp (authenticated user's fingerprint)
    // Contact request storage key: recipient_fp(32) || sender_fp(32)
    // Contact request value: recipient_fp(32) || sender_fp(32) || pow_nonce(8) || timestamp(8) || blob_len(4) || blob
    std::vector<uint8_t> prefix(session->fingerprint.begin(), session->fingerprint.end());

    Json::Value resp;
    resp["type"] = "OK";
    resp["id"] = id;
    Json::Value requests(Json::arrayValue);

    storage_.scan(storage::TABLE_REQUESTS, prefix,
        [&](std::span<const uint8_t> key, std::span<const uint8_t> value) {
            if (value.size() < 84) return true;  // skip malformed

            // sender_fp at offset 32
            Json::Value entry;
            entry["from"] = to_hex(std::span<const uint8_t>(value.data() + 32, 32));

            // timestamp at offset 72 (8 bytes BE, milliseconds)
            uint64_t ts = 0;
            for (int i = 0; i < 8; ++i) {
                ts = (ts << 8) | value[72 + i];
            }
            entry["timestamp"] = Json::UInt64(ts);

            // blob at offset 84
            uint32_t blob_len = (static_cast<uint32_t>(value[80]) << 24)
                              | (static_cast<uint32_t>(value[81]) << 16)
                              | (static_cast<uint32_t>(value[82]) << 8)
                              | static_cast<uint32_t>(value[83]);
            if (84 + blob_len <= value.size()) {
                entry["blob"] = to_base64(
                    std::span<const uint8_t>(value.data() + 84, blob_len));
            }

            requests.append(entry);
            return true;  // continue scanning
        });

    resp["requests"] = requests;
    send_json(ws, resp);
}

// ---------- SET_PROFILE handler ----------

template<bool SSL>
void WsServer<SSL>::handle_set_profile(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    std::string profile_b64 = msg.get("profile", "").asString();
    if (profile_b64.empty()) {
        send_error(ws, id, 400, "missing profile");
        return;
    }
    auto profile_data = from_base64(profile_b64);
    if (!profile_data) {
        send_error(ws, id, 400, "invalid base64 in profile");
        return;
    }
    if (profile_data->size() > config::protocol::MAX_PROFILE_SIZE) {
        send_error(ws, id, 400, "profile exceeds max size");
        return;
    }

    // Verify the profile's fingerprint matches the authenticated session
    if (profile_data->size() < 32) {
        send_error(ws, id, 400, "profile too short");
        return;
    }
    crypto::Hash profile_fp{};
    std::copy_n(profile_data->data(), 32, profile_fp.begin());
    if (profile_fp != session->fingerprint) {
        send_error(ws, id, 403, "profile fingerprint does not match authenticated identity");
        return;
    }

    // Compute storage key: SHA3-256("profile:" || fingerprint)
    auto profile_key = crypto::sha3_256_prefixed("profile:", session->fingerprint);

    // Dispatch to worker: validate + store + replicate via Kademlia
    auto value = std::make_shared<std::vector<uint8_t>>(std::move(*profile_data));
    auto key = profile_key;
    auto ws_ptr = ws;

    if (!workers_.post([this, ws_ptr, id, key, value]() {
        bool ok = kad_.store(key, 0x00, *value);

        loop_->defer([this, ws_ptr, id, ok]() {
            if (connections_.count(ws_ptr) == 0) return;
            if (ok) {
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                send_json(ws_ptr, resp);
            } else {
                send_error(ws_ptr, id, 500, "failed to store profile");
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

// ---------- REGISTER_NAME handler ----------

template<bool SSL>
void WsServer<SSL>::handle_register_name(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    std::string record_b64 = msg.get("name_record", "").asString();
    if (record_b64.empty()) {
        send_error(ws, id, 400, "missing name_record");
        return;
    }
    auto record_data = from_base64(record_b64);
    if (!record_data) {
        send_error(ws, id, 400, "invalid base64 in name_record");
        return;
    }

    // Parse name_len and name from the record to compute the storage key
    if (record_data->empty()) {
        send_error(ws, id, 400, "name_record too short");
        return;
    }
    uint8_t name_len = (*record_data)[0];
    if (1 + name_len + 32 > record_data->size()) {
        send_error(ws, id, 400, "name_record too short for name + fingerprint");
        return;
    }
    std::string name(reinterpret_cast<const char*>(record_data->data() + 1), name_len);

    // Verify the name record's fingerprint matches the authenticated session
    crypto::Hash record_fp{};
    std::copy_n(record_data->data() + 1 + name_len, 32, record_fp.begin());
    if (record_fp != session->fingerprint) {
        send_error(ws, id, 403, "name record fingerprint does not match authenticated identity");
        return;
    }

    // Compute storage key: SHA3-256("name:" || name)
    std::vector<uint8_t> name_bytes(name.begin(), name.end());
    auto name_key = crypto::sha3_256_prefixed("name:", name_bytes);

    // Dispatch to worker: validate + store + replicate via Kademlia
    auto value = std::make_shared<std::vector<uint8_t>>(std::move(*record_data));
    auto key = name_key;
    auto ws_ptr = ws;

    if (!workers_.post([this, ws_ptr, id, key, value]() {
        bool ok = kad_.store(key, 0x01, *value);

        loop_->defer([this, ws_ptr, id, ok]() {
            if (connections_.count(ws_ptr) == 0) return;
            if (ok) {
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                send_json(ws_ptr, resp);
            } else {
                send_error(ws_ptr, id, 500, "failed to register name");
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

// ---------- upload timeout ----------

template<bool SSL>
void WsServer<SSL>::check_upload_timeouts() {
    auto upload_timeout = std::chrono::seconds(config::defaults::UPLOAD_TIMEOUT);
    auto now = std::chrono::steady_clock::now();

    for (auto* ws : connections_) {
        auto* session = ws->getUserData();
        if (session->pending_upload &&
            (now - session->pending_upload->started) > upload_timeout) {
            send_error(ws, session->pending_upload->id, 408, "upload timeout");
            session->pending_upload.reset();
        }
    }
}


// ---------- group meta cache & ACL helpers ----------

template<bool SSL>
std::optional<GroupMeta> WsServer<SSL>::parse_group_meta(std::span<const uint8_t> data) {
    // Binary format:
    //   group_id(32) || owner_fp(32) || version(4 BE) || member_count(2 BE) ||
    //   per-member[fp(32) + role(1) + kem_ciphertext(1568)] × member_count ||
    //   sig_len(2 BE) || signature

    constexpr size_t HEADER_SIZE = 32 + 32 + 4 + 2;  // 70 bytes
    constexpr size_t MEMBER_SIZE = 32 + 1 + 1568;     // 1601 bytes per member

    if (data.size() < HEADER_SIZE) return std::nullopt;

    GroupMeta meta;

    // group_id (32 bytes)
    std::memcpy(meta.group_id.data(), data.data(), 32);

    // owner_fp (32 bytes)
    std::memcpy(meta.owner_fingerprint.data(), data.data() + 32, 32);

    // version (4 bytes BE)
    meta.version = (static_cast<uint32_t>(data[64]) << 24) |
                   (static_cast<uint32_t>(data[65]) << 16) |
                   (static_cast<uint32_t>(data[66]) << 8) |
                   static_cast<uint32_t>(data[67]);

    // member_count (2 bytes BE)
    uint16_t member_count = (static_cast<uint16_t>(data[68]) << 8) |
                            static_cast<uint16_t>(data[69]);

    // Validate total size: header + members + sig_len(2) + signature(variable)
    size_t members_end = HEADER_SIZE + static_cast<size_t>(member_count) * MEMBER_SIZE;
    if (data.size() < members_end + 2) return std::nullopt;

    // Parse members (extract fingerprint and role, skip kem_ciphertext)
    meta.members.reserve(member_count);
    for (uint16_t i = 0; i < member_count; ++i) {
        size_t offset = HEADER_SIZE + static_cast<size_t>(i) * MEMBER_SIZE;
        GroupMember member;
        std::memcpy(member.fingerprint.data(), data.data() + offset, 32);
        member.role = data[offset + 32];
        meta.members.push_back(member);
    }

    // Validate signature length field exists (we don't verify the signature here)
    uint16_t sig_len = (static_cast<uint16_t>(data[members_end]) << 8) |
                       static_cast<uint16_t>(data[members_end + 1]);
    if (data.size() < members_end + 2 + sig_len) return std::nullopt;

    return meta;
}

template<bool SSL>
const GroupMeta* WsServer<SSL>::get_group_meta(const crypto::Hash& group_id) {
    // Check cache first
    auto it = group_meta_cache_.find(group_id);
    if (it != group_meta_cache_.end()) {
        return &it->second;
    }

    // Load from storage (keyed by routing key = SHA3-256("group:" || group_id))
    auto gid_span = std::span<const uint8_t>(group_id.data(), group_id.size());
    auto routing_key = crypto::sha3_256_prefixed("group:", gid_span);
    auto raw = storage_.get(storage::TABLE_GROUP_META, routing_key);
    if (!raw) return nullptr;

    auto meta = parse_group_meta(*raw);
    if (!meta) return nullptr;

    auto [ins, _] = group_meta_cache_.emplace(group_id, std::move(*meta));
    return &ins->second;
}

template<bool SSL>
void WsServer<SSL>::invalidate_group_meta(const crypto::Hash& group_id) {
    group_meta_cache_.erase(group_id);
}

template<bool SSL>
bool WsServer<SSL>::check_group_role(const crypto::Hash& group_id,
                                     const crypto::Hash& fingerprint,
                                     uint8_t min_role) {
    const auto* meta = get_group_meta(group_id);
    if (!meta) return false;

    for (const auto& member : meta->members) {
        if (member.fingerprint == fingerprint && member.role >= min_role) {
            return true;
        }
    }
    return false;
}

// ---------- fetch_group_meta (blocking, worker pool only) ----------

template<bool SSL>
std::optional<GroupMeta> WsServer<SSL>::fetch_group_meta(const crypto::Hash& group_id) {
    // Try local storage first (keyed by routing key)
    auto gid_span = std::span<const uint8_t>(group_id.data(), group_id.size());
    auto routing_key = crypto::sha3_256_prefixed("group:", gid_span);
    auto raw = storage_.get(storage::TABLE_GROUP_META, routing_key);
    if (raw) {
        auto parsed = parse_group_meta(*raw);
        if (parsed) return parsed;
    }

    // Not found locally — remote quorum read via FIND_VALUE
    auto nodes = kad_.responsible_nodes(routing_key);
    auto results = kad_.query_remote_values(routing_key, nodes);

    for (const auto& nv : results) {
        if (!nv.value) continue;
        auto parsed = parse_group_meta(*nv.value);
        if (!parsed) continue;

        // Validate structure + key derivation before caching
        if (!kad_.validate_readonly(routing_key, 0x06, *nv.value)) continue;

        // Cache in local storage for future use (keyed by routing key)
        storage_.put(storage::TABLE_GROUP_META,
                     std::span<const uint8_t>(routing_key.data(), routing_key.size()),
                     *nv.value);

        // Invalidate in-memory cache on uWS thread so next get_group_meta() re-reads
        auto group_id_copy = group_id;
        loop_->defer([this, group_id_copy]() {
            invalidate_group_meta(group_id_copy);
        });

        return parsed;
    }
    return std::nullopt;
}

// ---------- group commands ----------

template<bool SSL>
void WsServer<SSL>::handle_group_create(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse group_meta hex string
    std::string meta_hex = msg.get("group_meta", "").asString();
    if (meta_hex.empty()) {
        send_error(ws, id, 400, "missing group_meta");
        return;
    }
    auto meta_bytes_opt = from_hex(meta_hex);
    if (!meta_bytes_opt) {
        send_error(ws, id, 400, "invalid hex in group_meta");
        return;
    }
    auto& meta_bytes = *meta_bytes_opt;

    // Parse and validate structure
    auto parsed = parse_group_meta(meta_bytes);
    if (!parsed) {
        send_error(ws, id, 400, "invalid group_meta");
        return;
    }

    // Version must be 1 for initial creation
    if (parsed->version != 1) {
        send_error(ws, id, 400, "initial version must be 1");
        return;
    }

    // Authenticated client must be an owner (role=0x02) in the meta
    bool is_owner = false;
    for (const auto& member : parsed->members) {
        if (member.fingerprint == session->fingerprint && member.role == 0x02) {
            is_owner = true;
            break;
        }
    }
    if (!is_owner) {
        send_error(ws, id, 403, "must be owner in group_meta");
        return;
    }

    auto group_id = parsed->group_id;

    // Dispatch existence check + store to worker pool
    auto meta_bytes_copy = std::move(meta_bytes);
    if (!workers_.post([this, ws, id, group_id,
                        meta_bytes_copy = std::move(meta_bytes_copy)]() mutable {
        // Check if group already exists (local + remote quorum read)
        auto existing = fetch_group_meta(group_id);
        if (existing) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 409, "group already exists");
            });
            return;
        }

        // Compute DHT routing key and store
        auto group_id_span = std::span<const uint8_t>(group_id.data(), group_id.size());
        auto group_key = crypto::sha3_256_prefixed("group:", group_id_span);

        bool ok = kad_.store(group_key, 0x06, meta_bytes_copy);

        auto group_id_copy = group_id;
        loop_->defer([this, ws, id, ok, group_id_copy]() {
            if (connections_.count(ws) == 0) return;

            if (ok) {
                invalidate_group_meta(group_id_copy);
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                resp["group_id"] = to_hex(group_id_copy);
                send_json(ws, resp);
            } else {
                send_error(ws, id, 500, "store failed");
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

template<bool SSL>
void WsServer<SSL>::handle_group_info(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse group_id hex string (64 hex chars = 32 bytes)
    std::string group_id_hex = msg.get("group_id", "").asString();
    if (group_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    auto group_id_bytes = from_hex(group_id_hex);
    if (!group_id_bytes) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }

    crypto::Hash group_id{};
    std::copy(group_id_bytes->begin(), group_id_bytes->end(), group_id.begin());

    auto fp_copy = session->fingerprint;

    if (!workers_.post([this, ws, id, group_id, fp_copy]() {
        auto meta = fetch_group_meta(group_id);
        if (!meta) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "group not found");
            });
            return;
        }

        // Check membership
        bool is_member = false;
        for (const auto& m : meta->members) {
            if (m.fingerprint == fp_copy) { is_member = true; break; }
        }
        if (!is_member) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "not a member");
            });
            return;
        }

        // Load raw GROUP_META from storage (keyed by routing key)
        auto gid_span = std::span<const uint8_t>(group_id.data(), group_id.size());
        auto routing_key = crypto::sha3_256_prefixed("group:", gid_span);
        auto raw = storage_.get(storage::TABLE_GROUP_META, routing_key);
        auto raw_copy = raw ? std::move(*raw) : std::vector<uint8_t>{};

        loop_->defer([this, ws, id, raw_copy = std::move(raw_copy)]() {
            if (connections_.count(ws) == 0) return;
            if (raw_copy.empty()) {
                send_error(ws, id, 404, "group not found");
                return;
            }
            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;
            resp["group_meta"] = to_hex(raw_copy);
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
    }
}

template<bool SSL>
void WsServer<SSL>::handle_group_update(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // 1. Parse group_meta hex string (lightweight, on uWS thread)
    std::string meta_hex = msg.get("group_meta", "").asString();
    if (meta_hex.empty()) {
        send_error(ws, id, 400, "missing group_meta");
        return;
    }
    auto meta_bytes_opt = from_hex(meta_hex);
    if (!meta_bytes_opt) {
        send_error(ws, id, 400, "invalid hex in group_meta");
        return;
    }
    auto meta_bytes = std::move(*meta_bytes_opt);

    // 2. Parse and validate structure
    auto parsed = parse_group_meta(meta_bytes);
    if (!parsed) {
        send_error(ws, id, 400, "invalid group_meta");
        return;
    }

    // 8. Must have at least one owner (role=0x02) — can check before dispatch
    bool has_owner = false;
    for (const auto& m : parsed->members) {
        if (m.role == 0x02) { has_owner = true; break; }
    }
    if (!has_owner && !parsed->members.empty()) {
        send_error(ws, id, 400, "must have at least one owner");
        return;
    }

    auto group_id = parsed->group_id;
    auto new_version = parsed->version;
    auto new_members = parsed->members;
    auto fp_copy = session->fingerprint;

    if (!workers_.post([this, ws, id, group_id, fp_copy, new_version,
                        new_members = std::move(new_members),
                        meta_bytes = std::move(meta_bytes)]() mutable {
        // 3. Load current GROUP_META (local + remote quorum read)
        auto current = fetch_group_meta(group_id);

        // 4. Group must exist
        if (!current) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "group not found");
            });
            return;
        }

        // 5. Verify signer is owner or admin in the CURRENT stored meta
        uint8_t signer_role = 0x00;
        bool is_admin_or_owner = false;
        for (const auto& m : current->members) {
            if (m.fingerprint == fp_copy) {
                signer_role = m.role;
                if (m.role >= 0x01) is_admin_or_owner = true;
                break;
            }
        }
        if (!is_admin_or_owner) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "insufficient role");
            });
            return;
        }

        // 6. Verify new version > current version
        if (new_version <= current->version) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 409, "version must be greater than current");
            });
            return;
        }

        // 7. Admin restriction: if signer is admin (not owner), enforce restrictions
        if (signer_role == 0x01) {
            std::unordered_map<crypto::Hash, uint8_t, crypto::HashHash> current_roles;
            for (const auto& m : current->members)
                current_roles[m.fingerprint] = m.role;

            std::unordered_map<crypto::Hash, uint8_t, crypto::HashHash> new_roles_map;
            for (const auto& m : new_members)
                new_roles_map[m.fingerprint] = m.role;

            for (const auto& m : new_members) {
                auto it = current_roles.find(m.fingerprint);
                if (it != current_roles.end()) {
                    if (it->second != m.role) {
                        loop_->defer([this, ws, id]() {
                            if (connections_.count(ws) == 0) return;
                            send_error(ws, id, 403, "admin cannot change roles");
                        });
                        return;
                    }
                } else {
                    if (m.role != 0x00) {
                        loop_->defer([this, ws, id]() {
                            if (connections_.count(ws) == 0) return;
                            send_error(ws, id, 403, "admin can only add regular members");
                        });
                        return;
                    }
                }
            }

            for (const auto& m : current->members) {
                if (new_roles_map.find(m.fingerprint) == new_roles_map.end()) {
                    if (m.role > 0x00) {
                        loop_->defer([this, ws, id]() {
                            if (connections_.count(ws) == 0) return;
                            send_error(ws, id, 403, "admin cannot remove admins or owners");
                        });
                        return;
                    }
                }
            }
        }

        // 9. Auto-destroy if member count is 0
        if (new_members.empty()) {
            auto group_id_span = std::span<const uint8_t>(group_id.data(), group_id.size());
            auto routing_key = crypto::sha3_256_prefixed("group:", group_id_span);

            storage_.del(storage::TABLE_GROUP_META, routing_key);

            std::vector<std::vector<uint8_t>> index_keys;
            storage_.scan(storage::TABLE_GROUP_INDEX, group_id_span,
                [&](std::span<const uint8_t> key, std::span<const uint8_t>) -> bool {
                    index_keys.emplace_back(key.begin(), key.end());
                    return true;
                });
            for (const auto& k : index_keys)
                storage_.del(storage::TABLE_GROUP_INDEX, k);

            std::vector<std::vector<uint8_t>> blob_keys;
            storage_.scan(storage::TABLE_GROUP_BLOBS, group_id_span,
                [&](std::span<const uint8_t> key, std::span<const uint8_t>) -> bool {
                    blob_keys.emplace_back(key.begin(), key.end());
                    return true;
                });
            for (const auto& k : blob_keys)
                storage_.del(storage::TABLE_GROUP_BLOBS, k);

            auto group_id_copy = group_id;
            loop_->defer([this, ws, id, group_id_copy]() {
                if (connections_.count(ws) == 0) return;
                invalidate_group_meta(group_id_copy);
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                resp["destroyed"] = true;
                send_json(ws, resp);
            });
            return;
        }

        // 10. Compute DHT routing key and store
        auto group_id_span = std::span<const uint8_t>(group_id.data(), group_id.size());
        auto group_key = crypto::sha3_256_prefixed("group:", group_id_span);

        bool ok = kad_.store(group_key, 0x06, meta_bytes);

        auto group_id_copy = group_id;
        loop_->defer([this, ws, id, ok, group_id_copy]() {
            if (connections_.count(ws) == 0) return;

            if (ok) {
                invalidate_group_meta(group_id_copy);
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                send_json(ws, resp);
            } else {
                send_error(ws, id, 500, "store failed");
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

template<bool SSL>
void WsServer<SSL>::handle_group_send(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse and validate group_id (64 hex chars = 32 bytes)
    std::string group_id_hex = msg.get("group_id", "").asString();
    if (group_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    auto group_id_bytes = from_hex(group_id_hex);
    if (!group_id_bytes) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    crypto::Hash group_id{};
    std::copy(group_id_bytes->begin(), group_id_bytes->end(), group_id.begin());

    // Parse and validate msg_id (64 hex chars = 32 bytes)
    std::string msg_id_hex = msg.get("msg_id", "").asString();
    if (msg_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid msg_id");
        return;
    }
    auto msg_id_bytes = from_hex(msg_id_hex);
    if (!msg_id_bytes) {
        send_error(ws, id, 400, "invalid msg_id");
        return;
    }

    // Parse gek_version
    if (!msg.isMember("gek_version") || !msg["gek_version"].isIntegral()) {
        send_error(ws, id, 400, "missing gek_version");
        return;
    }
    uint32_t gek_version = msg["gek_version"].asUInt();

    // Detect large vs small path:
    // If "size" field is present and "blob" is empty/missing -> large chunked upload
    // Otherwise -> small inline GROUP_SEND
    std::string blob_hex = msg.get("blob", "").asString();
    bool has_size = msg.isMember("size") && msg["size"].isUInt64();

    if (has_size && blob_hex.empty()) {
        // ---- Large chunked GROUP_SEND path ----
        uint64_t declared_size = msg["size"].asUInt64();

        static constexpr size_t MAX_BLOB_SIZE = 50ULL * 1024 * 1024;
        if (declared_size > MAX_BLOB_SIZE) {
            send_error(ws, id, 413, "attachment too large");
            return;
        }

        if (session->pending_upload) {
            send_error(ws, id, 429, "upload already in progress");
            return;
        }

        auto fp_copy = session->fingerprint;
        crypto::Hash msg_id_hash{};
        std::copy(msg_id_bytes->begin(), msg_id_bytes->end(), msg_id_hash.begin());

        // Dispatch membership check to worker, then defer upload setup back to uWS
        if (!workers_.post([this, ws, id, group_id, fp_copy, declared_size,
                            msg_id_hash, gek_version]() {
            auto meta = fetch_group_meta(group_id);
            if (!meta) {
                loop_->defer([this, ws, id]() {
                    if (connections_.count(ws) == 0) return;
                    send_error(ws, id, 404, "group not found");
                });
                return;
            }
            bool is_member = false;
            for (const auto& m : meta->members) {
                if (m.fingerprint == fp_copy) { is_member = true; break; }
            }
            if (!is_member) {
                loop_->defer([this, ws, id]() {
                    if (connections_.count(ws) == 0) return;
                    send_error(ws, id, 403, "not a member");
                });
                return;
            }

            loop_->defer([this, ws, id, group_id, declared_size, msg_id_hash, gek_version]() {
                if (connections_.count(ws) == 0) return;
                auto* s = ws->getUserData();
                if (s->pending_upload) {
                    send_error(ws, id, 429, "upload already in progress");
                    return;
                }

                uint32_t request_id = next_request_id_.fetch_add(1);

                Session::PendingUpload upload;
                upload.request_id = request_id;
                upload.id = id;
                upload.expected_size = static_cast<uint32_t>(declared_size);
                upload.received = 0;
                upload.next_chunk = 0;
                upload.started = std::chrono::steady_clock::now();
                upload.group_id = group_id;
                upload.msg_id = msg_id_hash;
                upload.gek_version = gek_version;
                s->pending_upload = std::move(upload);

                Json::Value resp;
                resp["type"] = "SEND_READY";
                resp["id"] = id;
                resp["request_id"] = request_id;
                send_json(ws, resp);
            });
        })) {
            send_error(ws, id, 503, "server overloaded");
        }
        return;
    }

    // ---- Small inline GROUP_SEND path ----
    if (blob_hex.empty()) {
        send_error(ws, id, 400, "missing blob");
        return;
    }
    auto blob_bytes = from_hex(blob_hex);
    if (!blob_bytes) {
        send_error(ws, id, 400, "invalid hex in blob");
        return;
    }

    // Max blob size check: 50 MiB
    static constexpr size_t MAX_BLOB_SIZE = 50ULL * 1024 * 1024;
    if (blob_bytes->size() > MAX_BLOB_SIZE) {
        send_error(ws, id, 413, "attachment too large");
        return;
    }

    if (blob_bytes->size() > INLINE_THRESHOLD) {
        send_error(ws, id, 400, "blob exceeds inline threshold, use chunked upload");
        return;
    }

    // Build GROUP_MESSAGE binary:
    // group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob
    auto now = std::chrono::system_clock::now();
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    uint32_t blob_len = static_cast<uint32_t>(blob_bytes->size());

    std::vector<uint8_t> msg_bytes;
    msg_bytes.reserve(112 + blob_len);
    msg_bytes.insert(msg_bytes.end(), group_id.begin(), group_id.end());
    msg_bytes.insert(msg_bytes.end(), session->fingerprint.begin(), session->fingerprint.end());
    msg_bytes.insert(msg_bytes.end(), msg_id_bytes->begin(), msg_id_bytes->end());
    for (int i = 7; i >= 0; --i)
        msg_bytes.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>((gek_version >> 24) & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>((gek_version >> 16) & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>((gek_version >> 8) & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>(gek_version & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    msg_bytes.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    msg_bytes.insert(msg_bytes.end(), blob_bytes->begin(), blob_bytes->end());

    auto group_id_span = std::span<const uint8_t>(group_id.data(), group_id.size());
    auto group_key = crypto::sha3_256_prefixed("group:", group_id_span);
    auto fp_copy = session->fingerprint;

    // Dispatch membership check + store to worker pool
    auto msg_id_copy = *msg_id_bytes;
    if (!workers_.post([this, ws, id, group_id, group_key, fp_copy,
                        msg_bytes = std::move(msg_bytes),
                        msg_id_copy]() mutable {
        auto meta = fetch_group_meta(group_id);
        if (!meta) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "group not found");
            });
            return;
        }
        bool is_member = false;
        for (const auto& m : meta->members) {
            if (m.fingerprint == fp_copy) { is_member = true; break; }
        }
        if (!is_member) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "not a member");
            });
            return;
        }

        bool ok = kad_.store(group_key, 0x05, msg_bytes);

        // Ensure data is stored locally for GROUP_LIST/GET/DELETE,
        // even if this node isn't responsible for the group key.
        // kad_.store() only stores locally on responsible nodes.
        if (ok) {
            std::vector<uint8_t> idx_key;
            idx_key.reserve(64);
            idx_key.insert(idx_key.end(), group_id.begin(), group_id.end());
            idx_key.insert(idx_key.end(), msg_id_copy.begin(), msg_id_copy.end());

            if (!storage_.get(storage::TABLE_GROUP_BLOBS, idx_key)) {
                std::vector<uint8_t> idx_value;
                idx_value.reserve(48);
                idx_value.insert(idx_value.end(), fp_copy.begin(), fp_copy.end());
                idx_value.insert(idx_value.end(), msg_bytes.begin() + 96, msg_bytes.begin() + 104);
                idx_value.insert(idx_value.end(), msg_bytes.begin() + 108, msg_bytes.begin() + 112);
                idx_value.insert(idx_value.end(), msg_bytes.begin() + 104, msg_bytes.begin() + 108);

                std::vector<uint8_t> blob(msg_bytes.begin() + 112, msg_bytes.end());

                storage_.batch_put({
                    {storage::TABLE_GROUP_INDEX, idx_key, idx_value},
                    {storage::TABLE_GROUP_BLOBS, idx_key, blob},
                });
            }
        }

        loop_->defer([this, ws, id, ok, msg_id_copy]() {
            if (connections_.count(ws) == 0) return;
            if (ok) {
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                resp["msg_id"] = to_hex(msg_id_copy);
                send_json(ws, resp);
            } else {
                send_error(ws, id, 500, "store failed");
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

template<bool SSL>
void WsServer<SSL>::handle_group_list(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse and validate group_id (64 hex chars = 32 bytes)
    std::string group_id_hex = msg.get("group_id", "").asString();
    if (group_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    auto group_id_bytes = from_hex(group_id_hex);
    if (!group_id_bytes) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    crypto::Hash group_id{};
    std::copy(group_id_bytes->begin(), group_id_bytes->end(), group_id.begin());

    // Parse optional "after" (msg_id to paginate from, 64 hex chars = 32 bytes)
    std::vector<uint8_t> after_id;
    if (msg.isMember("after") && !msg["after"].asString().empty()) {
        std::string after_hex = msg["after"].asString();
        if (after_hex.size() != 64) {
            send_error(ws, id, 400, "invalid after");
            return;
        }
        auto after_bytes = from_hex(after_hex);
        if (!after_bytes) {
            send_error(ws, id, 400, "invalid after");
            return;
        }
        after_id = std::move(*after_bytes);
    }

    // Parse optional "limit" (default 50, max 200)
    int limit = msg.get("limit", 50).asInt();
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;

    auto fp_copy = session->fingerprint;

    if (!workers_.post([this, ws, id, group_id, fp_copy,
                        after_id = std::move(after_id), limit]() {
        // Verify membership via quorum read
        auto meta = fetch_group_meta(group_id);
        if (!meta) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "group not found");
            });
            return;
        }
        bool is_member = false;
        for (const auto& m : meta->members) {
            if (m.fingerprint == fp_copy) { is_member = true; break; }
        }
        if (!is_member) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "not a member");
            });
            return;
        }

        // Prefix-scan TABLE_GROUP_INDEX
        auto group_id_span = std::span<const uint8_t>(group_id.data(), group_id.size());

        struct IndexEntry {
            std::vector<uint8_t> blob_key;
            std::string msg_id_hex;
            std::string sender_hex;
            uint64_t timestamp;
            uint32_t size;
            uint32_t gek_version;
        };
        std::vector<IndexEntry> entries;
        int count = 0;
        bool past_after = after_id.empty();

        storage_.scan(storage::TABLE_GROUP_INDEX, group_id_span,
            [&](std::span<const uint8_t> key, std::span<const uint8_t> value) -> bool {
                if (key.size() != 64) return true;
                if (value.size() != 48) return true;
                auto msg_id_part = key.subspan(32, 32);
                if (!past_after) {
                    int cmp = std::memcmp(msg_id_part.data(), after_id.data(), 32);
                    if (cmp <= 0) return true;
                    past_after = true;
                }
                if (count >= limit) return false;
                auto sender_fp = value.subspan(0, 32);
                uint64_t ts = 0;
                for (int i = 0; i < 8; ++i) ts = (ts << 8) | value[32 + i];
                uint32_t sz = (static_cast<uint32_t>(value[40]) << 24)
                            | (static_cast<uint32_t>(value[41]) << 16)
                            | (static_cast<uint32_t>(value[42]) << 8)
                            | static_cast<uint32_t>(value[43]);
                uint32_t gek_ver = (static_cast<uint32_t>(value[44]) << 24)
                                 | (static_cast<uint32_t>(value[45]) << 16)
                                 | (static_cast<uint32_t>(value[46]) << 8)
                                 | static_cast<uint32_t>(value[47]);
                entries.push_back({
                    std::vector<uint8_t>(key.begin(), key.end()),
                    to_hex(msg_id_part), to_hex(sender_fp),
                    ts, sz, gek_ver
                });
                ++count;
                return true;
            });

        // Build JSON, fetching inline blobs outside the scan transaction
        Json::Value messages(Json::arrayValue);
        for (const auto& e : entries) {
            Json::Value entry;
            entry["msg_id"] = e.msg_id_hex;
            entry["sender"] = e.sender_hex;
            entry["timestamp"] = Json::Value::UInt64(e.timestamp);
            entry["size"] = e.size;
            entry["gek_version"] = e.gek_version;
            if (e.size <= INLINE_THRESHOLD) {
                auto blob = storage_.get(storage::TABLE_GROUP_BLOBS, e.blob_key);
                entry["blob"] = blob ? Json::Value(to_hex(*blob)) : Json::nullValue;
            } else {
                entry["blob"] = Json::nullValue;
            }
            messages.append(entry);
        }

        loop_->defer([this, ws, id, messages = std::move(messages)]() {
            if (connections_.count(ws) == 0) return;
            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;
            resp["messages"] = messages;
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
    }
}

template<bool SSL>
void WsServer<SSL>::handle_group_get(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse and validate group_id (64 hex chars = 32 bytes)
    std::string group_id_hex = msg.get("group_id", "").asString();
    if (group_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    auto group_id_bytes = from_hex(group_id_hex);
    if (!group_id_bytes) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    crypto::Hash group_id{};
    std::copy(group_id_bytes->begin(), group_id_bytes->end(), group_id.begin());

    // Parse and validate msg_id (64 hex chars = 32 bytes)
    std::string msg_id_hex = msg.get("msg_id", "").asString();
    if (msg_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid msg_id");
        return;
    }
    auto msg_id_bytes = from_hex(msg_id_hex);
    if (!msg_id_bytes) {
        send_error(ws, id, 400, "invalid msg_id");
        return;
    }

    auto fp_copy = session->fingerprint;

    // Build composite key: group_id(32) || msg_id(32)
    std::vector<uint8_t> blob_key;
    blob_key.reserve(64);
    blob_key.insert(blob_key.end(), group_id.begin(), group_id.end());
    blob_key.insert(blob_key.end(), msg_id_bytes->begin(), msg_id_bytes->end());

    if (!workers_.post([this, ws, id, group_id, fp_copy,
                        blob_key = std::move(blob_key)]() {
        // Verify membership via quorum read
        auto meta = fetch_group_meta(group_id);
        if (!meta) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "group not found");
            });
            return;
        }
        bool is_member = false;
        for (const auto& m : meta->members) {
            if (m.fingerprint == fp_copy) { is_member = true; break; }
        }
        if (!is_member) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "not a member");
            });
            return;
        }

        // Read blob from storage
        auto blob = storage_.get(storage::TABLE_GROUP_BLOBS, blob_key);
        if (!blob) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "message not found");
            });
            return;
        }

        auto blob_copy = std::move(*blob);

        loop_->defer([this, ws, id, blob_copy = std::move(blob_copy)]() {
            if (connections_.count(ws) == 0) return;

            if (blob_copy.size() <= INLINE_THRESHOLD) {
                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                resp["blob"] = to_hex(blob_copy);
                send_json(ws, resp);
            } else {
                static constexpr size_t CHUNK_SIZE = 1048576;
                uint32_t num_chunks = static_cast<uint32_t>(
                    (blob_copy.size() + CHUNK_SIZE - 1) / CHUNK_SIZE);
                uint32_t request_id = next_request_id_.fetch_add(1);

                Json::Value resp;
                resp["type"] = "OK";
                resp["id"] = id;
                resp["size"] = static_cast<Json::UInt>(blob_copy.size());
                resp["chunks"] = num_chunks;
                resp["chunked"] = true;
                send_json(ws, resp);

                for (uint32_t i = 0; i < num_chunks; ++i) {
                    size_t offset = static_cast<size_t>(i) * CHUNK_SIZE;
                    size_t payload_size = std::min(CHUNK_SIZE, blob_copy.size() - offset);

                    std::vector<uint8_t> frame;
                    frame.reserve(7 + payload_size);
                    frame.push_back(0x02);
                    frame.push_back(static_cast<uint8_t>((request_id >> 24) & 0xFF));
                    frame.push_back(static_cast<uint8_t>((request_id >> 16) & 0xFF));
                    frame.push_back(static_cast<uint8_t>((request_id >> 8) & 0xFF));
                    frame.push_back(static_cast<uint8_t>(request_id & 0xFF));
                    uint16_t chunk_index = static_cast<uint16_t>(i);
                    frame.push_back(static_cast<uint8_t>((chunk_index >> 8) & 0xFF));
                    frame.push_back(static_cast<uint8_t>(chunk_index & 0xFF));
                    frame.insert(frame.end(),
                                 blob_copy.data() + offset,
                                 blob_copy.data() + offset + payload_size);

                    std::string_view sv(reinterpret_cast<const char*>(frame.data()),
                                        frame.size());
                    ws->send(sv, uWS::OpCode::BINARY);
                }
            }
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
    }
}

template<bool SSL>
void WsServer<SSL>::handle_group_delete(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse and validate group_id (64 hex chars = 32 bytes)
    std::string group_id_hex = msg.get("group_id", "").asString();
    if (group_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    auto group_id_bytes = from_hex(group_id_hex);
    if (!group_id_bytes) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    crypto::Hash group_id{};
    std::copy(group_id_bytes->begin(), group_id_bytes->end(), group_id.begin());

    // Parse and validate msg_id (64 hex chars = 32 bytes)
    std::string msg_id_hex = msg.get("msg_id", "").asString();
    if (msg_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid msg_id");
        return;
    }
    auto msg_id_bytes = from_hex(msg_id_hex);
    if (!msg_id_bytes) {
        send_error(ws, id, 400, "invalid msg_id");
        return;
    }

    auto fp_copy = session->fingerprint;

    // Build composite key: group_id(32) || msg_id(32)
    std::vector<uint8_t> composite_key;
    composite_key.reserve(64);
    composite_key.insert(composite_key.end(), group_id.begin(), group_id.end());
    composite_key.insert(composite_key.end(), msg_id_bytes->begin(), msg_id_bytes->end());

    if (!workers_.post([this, ws, id, group_id, fp_copy,
                        composite_key = std::move(composite_key)]() {
        // Verify membership via quorum read
        auto meta = fetch_group_meta(group_id);
        if (!meta) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "group not found");
            });
            return;
        }
        bool is_member = false;
        for (const auto& m : meta->members) {
            if (m.fingerprint == fp_copy) { is_member = true; break; }
        }
        if (!is_member) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "not a member");
            });
            return;
        }

        // Read GROUP_INDEX entry to get sender fingerprint
        auto index_val = storage_.get(storage::TABLE_GROUP_INDEX, composite_key);
        if (!index_val) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "message not found");
            });
            return;
        }

        if (index_val->size() != 48) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 500, "corrupt index entry");
            });
            return;
        }

        // Extract sender_fp
        crypto::Hash sender_fp{};
        std::copy(index_val->begin(), index_val->begin() + 32, sender_fp.begin());

        // Permission: requester is the sender, OR has role >= 0x01 (admin/owner)
        bool is_sender = (fp_copy == sender_fp);
        bool is_admin = false;
        for (const auto& m : meta->members) {
            if (m.fingerprint == fp_copy && m.role >= 0x01) { is_admin = true; break; }
        }
        if (!is_sender && !is_admin) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "permission denied");
            });
            return;
        }

        // Delete from both tables
        storage_.del(storage::TABLE_GROUP_INDEX, composite_key);
        storage_.del(storage::TABLE_GROUP_BLOBS, composite_key);

        loop_->defer([this, ws, id]() {
            if (connections_.count(ws) == 0) return;
            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
    }
}

template<bool SSL>
void WsServer<SSL>::handle_group_destroy(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse and validate group_id (64 hex chars = 32 bytes)
    std::string group_id_hex = msg.get("group_id", "").asString();
    if (group_id_hex.size() != 64) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    auto group_id_bytes = from_hex(group_id_hex);
    if (!group_id_bytes) {
        send_error(ws, id, 400, "invalid group_id");
        return;
    }
    crypto::Hash group_id{};
    std::copy(group_id_bytes->begin(), group_id_bytes->end(), group_id.begin());

    auto fp_copy = session->fingerprint;

    // Dispatch ownership check + cleanup to worker pool
    if (!workers_.post([this, ws, id, group_id, fp_copy]() {
        // Verify requester is an owner (role=0x02) via quorum read
        auto meta = fetch_group_meta(group_id);
        if (!meta) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 404, "group not found");
            });
            return;
        }
        bool is_owner = false;
        for (const auto& m : meta->members) {
            if (m.fingerprint == fp_copy && m.role == 0x02) { is_owner = true; break; }
        }
        if (!is_owner) {
            loop_->defer([this, ws, id]() {
                if (connections_.count(ws) == 0) return;
                send_error(ws, id, 403, "only owners can destroy");
            });
            return;
        }

        auto group_id_span = std::span<const uint8_t>(group_id.data(), group_id.size());
        auto routing_key = crypto::sha3_256_prefixed("group:", group_id_span);

        // Delete GROUP_META (stored by routing key)
        storage_.del(storage::TABLE_GROUP_META, routing_key);

        // Propagate GROUP_META deletion to responsible nodes immediately
        kad_.delete_remote(routing_key, 0x06);

        // Also record in repl_log for SYNC propagation
        kad_.delete_value(routing_key, 0x06,
            std::span<const uint8_t>(routing_key.data(), routing_key.size()));

        // Collect GROUP_INDEX keys with group_id prefix, then delete
        std::vector<std::vector<uint8_t>> keys;
        storage_.scan(storage::TABLE_GROUP_INDEX, group_id_span,
            [&](std::span<const uint8_t> key, std::span<const uint8_t>) -> bool {
                keys.emplace_back(key.begin(), key.end());
                return true;
            });
        for (const auto& k : keys) {
            storage_.del(storage::TABLE_GROUP_INDEX, k);
            storage_.del(storage::TABLE_GROUP_BLOBS, k);
            // Propagate each group message deletion via repl_log → SYNC
            kad_.delete_value(routing_key, 0x05,
                std::span<const uint8_t>(k.data(), k.size()));
        }

        auto group_id_copy = group_id;
        loop_->defer([this, ws, id, group_id_copy]() {
            if (connections_.count(ws) == 0) return;
            invalidate_group_meta(group_id_copy);
            Json::Value resp;
            resp["type"] = "OK";
            resp["id"] = id;
            send_json(ws, resp);
        });
    })) {
        send_error(ws, id, 503, "server overloaded");
        return;
    }
}

// ---------- helpers ----------

template<bool SSL>
void WsServer<SSL>::send_json(ws_t* ws, const Json::Value& msg) {
    Json::StreamWriterBuilder writer_builder;
    writer_builder["indentation"] = "";
    std::string json = Json::writeString(writer_builder, msg);
    ws->send(json, uWS::OpCode::TEXT);
}

template<bool SSL>
void WsServer<SSL>::send_error(ws_t* ws, int id, int code, const std::string& reason) {
    Json::Value err;
    err["type"] = "ERROR";
    err["id"] = id;
    err["code"] = code;
    err["reason"] = reason;
    send_json(ws, err);
}

// Explicit template instantiations
template class WsServer<false>;
template class WsServer<true>;

} // namespace chromatin::ws
