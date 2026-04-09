#include "relay/ws/ws_session.h"
#include "relay/ws/session_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <optional>

namespace chromatindb::relay::ws {

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

// ---------------------------------------------------------------------------
// Hex utilities (file-local)
// ---------------------------------------------------------------------------

static std::string to_hex(std::span<const uint8_t> data) {
    std::string s;
    s.reserve(data.size() * 2);
    for (auto b : data) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
    }
    return s;
}

static std::optional<std::vector<uint8_t>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;

    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t hi, lo;

        char c = hex[i];
        if (c >= '0' && c <= '9') hi = static_cast<uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f') hi = static_cast<uint8_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi = static_cast<uint8_t>(c - 'A' + 10);
        else return std::nullopt;

        c = hex[i + 1];
        if (c >= '0' && c <= '9') lo = static_cast<uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f') lo = static_cast<uint8_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') lo = static_cast<uint8_t>(c - 'A' + 10);
        else return std::nullopt;

        result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Construction / Factory
// ---------------------------------------------------------------------------

WsSession::WsSession(Stream stream, SessionManager& manager,
                     asio::any_io_executor executor, size_t max_send_queue,
                     core::Authenticator& authenticator, asio::io_context& ioc)
    : stream_(std::move(stream))
    , session_(executor, max_send_queue)
    , manager_(manager)
    , executor_(executor)
    , authenticator_(authenticator)
    , ioc_(ioc)
    , auth_timer_(executor)
    , idle_timer_(executor) {
    last_pong_ = std::chrono::steady_clock::now();
}

std::shared_ptr<WsSession> WsSession::create(
    Stream stream,
    SessionManager& manager,
    asio::any_io_executor executor,
    size_t max_send_queue,
    core::Authenticator& authenticator,
    asio::io_context& ioc) {
    // Cannot use make_shared due to private constructor.
    return std::shared_ptr<WsSession>(
        new WsSession(std::move(stream), manager, executor, max_send_queue,
                       authenticator, ioc));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WsSession::start(uint64_t session_id) {
    session_id_ = session_id;

    // Inject write callback -- shared_from_this() is safe here (called after create()).
    auto self = shared_from_this();
    session_.set_write_callback(
        [self](std::span<const uint8_t> data) -> asio::awaitable<bool> {
            return self->write_frame(data);
        });

    // Spawn session coroutines.
    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> { co_await self->read_loop(); },
        asio::detached);

    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> { co_await self->session_.drain_send_queue(); },
        asio::detached);

    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> { co_await self->ping_loop(); },
        asio::detached);

    // Send challenge immediately after WS upgrade (per D-02).
    // Idle timer NOT started until AUTHENTICATED (Pitfall 4).
    send_challenge();
}

// ---------------------------------------------------------------------------
// Challenge / Auth helpers
// ---------------------------------------------------------------------------

void WsSession::send_challenge() {
    challenge_ = authenticator_.generate_challenge();

    nlohmann::json j = {
        {"type", "challenge"},
        {"nonce", to_hex(challenge_)}
    };
    send_json(j);

    // Start 10s auth timeout (per D-02, D-33).
    auth_timer_.expires_after(AUTH_TIMEOUT);
    auto self = shared_from_this();
    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> {
            auto [ec] = co_await self->auth_timer_.async_wait(use_nothrow);
            if (ec) co_return;  // Cancelled (auth succeeded or session closed)
            if (!self->closing_ && self->state_ == SessionState::AWAITING_AUTH) {
                spdlog::warn("session {}: auth timeout", self->session_id_);
                self->send_json({{"type", "auth_error"}, {"reason", "timeout"}});
                self->close(CLOSE_AUTH_FAILURE, "auth timeout");
            }
        }, asio::detached);
}

void WsSession::send_json(const nlohmann::json& j) {
    auto self = shared_from_this();
    auto text = j.dump();
    asio::co_spawn(executor_,
        [self, text = std::move(text)]() mutable -> asio::awaitable<void> {
            co_await self->session_.enqueue(std::move(text));
        }, asio::detached);
}

// ---------------------------------------------------------------------------
// Async I/O helpers (dual-mode: plain TCP / TLS)
// ---------------------------------------------------------------------------

asio::awaitable<size_t> WsSession::async_read(asio::mutable_buffer buf) {
    return std::visit([&](auto& stream) -> asio::awaitable<size_t> {
        auto [ec, n] = co_await stream.async_read_some(buf, use_nothrow);
        if (ec) co_return 0;
        co_return n;
    }, stream_);
}

asio::awaitable<size_t> WsSession::async_write(asio::const_buffer buf) {
    return std::visit([&](auto& stream) -> asio::awaitable<size_t> {
        auto [ec, n] = co_await asio::async_write(stream, buf, use_nothrow);
        if (ec) co_return 0;
        co_return n;
    }, stream_);
}

asio::awaitable<bool> WsSession::send_raw(const std::string& frame) {
    auto n = co_await async_write(asio::buffer(frame));
    co_return n == frame.size();
}

// ---------------------------------------------------------------------------
// Write callback (invoked by Session drain coroutine)
// ---------------------------------------------------------------------------

asio::awaitable<bool> WsSession::write_frame(std::span<const uint8_t> data) {
    if (closing_) co_return false;

    // Encode as text frame. Phase 102+ will distinguish text vs binary.
    auto frame = encode_frame(OPCODE_TEXT, data);
    auto n = co_await async_write(asio::buffer(frame));
    co_return n == frame.size();
}

// ---------------------------------------------------------------------------
// Read loop
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::read_loop() {
    while (!closing_) {
        auto n = co_await async_read(asio::buffer(read_buf_));
        if (n == 0) break;  // Connection closed or error

        // Append to pending data buffer.
        pending_data_.insert(pending_data_.end(),
                           read_buf_.data(), read_buf_.data() + n);

        // Parse frames from pending data.
        while (!pending_data_.empty() && !closing_) {
            auto header = parse_frame_header(
                std::span<const uint8_t>(pending_data_));
            if (!header) break;  // Insufficient bytes for header

            size_t total_frame = header->header_size + header->payload_length;
            if (pending_data_.size() < total_frame) break;  // Incomplete payload

            // Validate: client frames MUST be masked (per D-10).
            if (!header->masked) {
                spdlog::warn("session {}: client frame not masked -- protocol error",
                            session_id_);
                auto frame = encode_close_frame(CLOSE_PROTOCOL_ERROR,
                                               "client frames must be masked");
                co_await send_raw(frame);
                close(CLOSE_PROTOCOL_ERROR, "client frames must be masked");
                co_return;
            }

            // Extract and unmask payload.
            std::vector<uint8_t> payload(
                pending_data_.begin() + static_cast<ptrdiff_t>(header->header_size),
                pending_data_.begin() + static_cast<ptrdiff_t>(total_frame));
            apply_mask(payload, header->mask_key);

            // Consume from pending buffer.
            pending_data_.erase(pending_data_.begin(),
                               pending_data_.begin() + static_cast<ptrdiff_t>(total_frame));

            // Feed into fragment assembler.
            auto msg = assembler_.feed(header->opcode, header->fin,
                                       std::span<const uint8_t>(payload));

            switch (msg.result) {
            case AssemblyResult::COMPLETE:
                co_await on_message(msg.opcode, std::move(msg.data));
                break;

            case AssemblyResult::CONTROL:
                co_await handle_control(msg);
                break;

            case AssemblyResult::ERROR:
                spdlog::warn("session {}: frame assembly error: {}",
                            session_id_, msg.error_reason);
                {
                    auto frame = encode_close_frame(CLOSE_PROTOCOL_ERROR,
                                                   msg.error_reason);
                    co_await send_raw(frame);
                }
                close(CLOSE_PROTOCOL_ERROR, msg.error_reason);
                co_return;

            case AssemblyResult::INCOMPLETE:
                // More frames needed, continue reading.
                break;
            }
        }
    }

    // Read loop exited -- clean up session.
    if (!closing_) {
        spdlog::debug("session {}: connection closed by peer", session_id_);
    }
    manager_.remove_session(session_id_);
    session_.close();
    shutdown_socket();
}

// ---------------------------------------------------------------------------
// Control frame handling
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::handle_control(const AssembledMessage& msg) {
    switch (msg.opcode) {
    case OPCODE_PING: {
        // Respond with Pong carrying same payload.
        auto pong = encode_frame(OPCODE_PONG,
                                std::span<const uint8_t>(msg.data));
        co_await send_raw(pong);
        break;
    }

    case OPCODE_PONG:
        // Update keepalive timestamp.
        last_pong_ = std::chrono::steady_clock::now();
        break;

    case OPCODE_CLOSE: {
        // Echo close frame with same status code (per D-16).
        auto close_frame = encode_close_frame(msg.close_code);
        co_await send_raw(close_frame);
        close(msg.close_code, "");
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Ping/pong keepalive (per D-15)
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::ping_loop() {
    while (!closing_) {
        asio::steady_timer timer(executor_);
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(use_nothrow);
        if (ec || closing_) break;

        // Check if pong was received within 60s.
        auto elapsed = std::chrono::steady_clock::now() - last_pong_;
        if (elapsed > std::chrono::seconds(60)) {
            spdlog::warn("session {}: pong timeout ({}s since last pong)",
                        session_id_,
                        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            close(CLOSE_PROTOCOL_ERROR, "pong timeout");
            break;
        }

        // Send Ping frame.
        auto ping = encode_frame(OPCODE_PING, {});
        co_await send_raw(ping);
    }
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::on_message(uint8_t opcode, std::vector<uint8_t> data) {
    if (state_ == SessionState::AWAITING_AUTH) {
        co_await handle_auth_message(std::move(data));
        co_return;
    }

    // AUTHENTICATED state:
    // Phase 102: log and drop. Phase 103 adds JSON parsing + UDS forwarding.
    // Plan 02 will add message filter check here.
    spdlog::debug("session {}: received {} frame, {} bytes (authenticated, no handler yet)",
                 session_id_,
                 opcode == OPCODE_TEXT ? "text" : "binary",
                 data.size());
}

// ---------------------------------------------------------------------------
// Auth state machine (per D-01 through D-17)
// ---------------------------------------------------------------------------

asio::awaitable<void> WsSession::handle_auth_message(std::vector<uint8_t> data) {
    // 1. Parse JSON from data bytes.
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(data.begin(), data.end());
    } catch (const nlohmann::json::parse_error&) {
        send_json({{"type", "error"}, {"code", "bad_json"}, {"message", "invalid JSON"}});
        close(CLOSE_AUTH_FAILURE, "bad json");
        co_return;
    }

    // 2. Extract "type" field.
    if (!j.contains("type") || !j["type"].is_string()) {
        send_json({{"type", "error"}, {"code", "missing_type"}, {"message", "missing type field"}});
        close(CLOSE_AUTH_FAILURE, "missing type");
        co_return;
    }

    auto type = j["type"].get<std::string>();

    // 3. Per D-13: duplicate challenge_response after already authenticated -> ignore.
    if (type == "challenge_response" && state_ == SessionState::AUTHENTICATED) {
        spdlog::debug("session {}: duplicate challenge_response, ignoring", session_id_);
        co_return;
    }

    // 4. Per D-03: non-auth message before auth -> reject.
    if (type != "challenge_response") {
        send_json({{"type", "error"}, {"code", "not_authenticated"}, {"message", "authenticate first"}});
        close(CLOSE_AUTH_FAILURE, "not authenticated");
        co_return;
    }

    // 5. Extract pubkey and signature hex strings.
    if (!j.contains("pubkey") || !j["pubkey"].is_string() ||
        !j.contains("signature") || !j["signature"].is_string()) {
        send_json({{"type", "auth_error"}, {"reason", "missing_field"}});
        close(CLOSE_AUTH_FAILURE, "missing field");
        co_return;
    }

    auto pubkey_hex = j["pubkey"].get<std::string>();
    auto sig_hex = j["signature"].get<std::string>();

    // 6. Decode hex pubkey (per Pitfall 3).
    auto pubkey_bytes = from_hex(pubkey_hex);
    if (!pubkey_bytes || pubkey_bytes->size() != 2592) {
        send_json({{"type", "auth_error"}, {"reason", "bad_pubkey_format"}});
        close(CLOSE_AUTH_FAILURE, "bad pubkey format");
        co_return;
    }

    // 7. Decode hex signature.
    auto sig_bytes = from_hex(sig_hex);
    if (!sig_bytes) {
        send_json({{"type", "auth_error"}, {"reason", "bad_signature_format"}});
        close(CLOSE_AUTH_FAILURE, "bad signature format");
        co_return;
    }

    // 8. Offload verification to thread pool (per D-08).
    co_await asio::post(ioc_, asio::use_awaitable);
    auto result = authenticator_.verify(challenge_, *pubkey_bytes, *sig_bytes);
    co_await asio::post(executor_, asio::use_awaitable);

    // 9. Check if session closed during verify.
    if (closing_) co_return;

    // 10. Handle verification failure (per D-04).
    if (!result.success) {
        send_json({{"type", "auth_error"}, {"reason", result.error_code}});
        close(CLOSE_AUTH_FAILURE, result.error_code);
        co_return;
    }

    // 11. Success (per D-06, D-16, D-10).
    client_pubkey_ = std::move(result.public_key);
    client_namespace_ = result.namespace_hash;
    state_ = SessionState::AUTHENTICATED;  // D-16: state change before sending auth_ok
    auth_timer_.cancel();                   // Stop 10s timeout

    // Start idle timer now that we're authenticated (30s idle in AUTHENTICATED state).
    idle_timer_.expires_after(IDLE_TIMEOUT);
    auto self = shared_from_this();
    asio::co_spawn(executor_,
        [self]() -> asio::awaitable<void> {
            auto [ec] = co_await self->idle_timer_.async_wait(use_nothrow);
            if (ec) co_return;  // Cancelled (message arrived or session closed)
            if (self->state_ == SessionState::AUTHENTICATED && !self->closing_) {
                spdlog::warn("session {} idle timeout -- no message after auth", self->session_id_);
                self->close(CLOSE_PROTOCOL_ERROR, "idle timeout");
            }
        }, asio::detached);

    send_json({{"type", "auth_ok"}, {"namespace", to_hex(client_namespace_)}});
    spdlog::info("session {}: authenticated, namespace {}", session_id_, to_hex(client_namespace_));
}

// ---------------------------------------------------------------------------
// Close handling (per D-16)
// ---------------------------------------------------------------------------

void WsSession::close(uint16_t code, std::string_view reason) {
    if (closing_) return;
    closing_ = true;

    spdlog::debug("session {}: closing with code {} reason '{}'",
                 session_id_, code, reason);

    // Cancel auth timer if still running.
    auth_timer_.cancel();

    // Send close frame directly (bypass send queue for control frames).
    auto self = shared_from_this();
    asio::co_spawn(executor_,
        [self, code, reason = std::string(reason)]() -> asio::awaitable<void> {
            auto frame = encode_close_frame(code, reason);
            co_await self->send_raw(frame);

            // Wait up to 5 seconds for client close echo, then force shutdown.
            asio::steady_timer timer(self->executor_);
            timer.expires_after(std::chrono::seconds(5));
            auto [ec] = co_await timer.async_wait(use_nothrow);

            self->shutdown_socket();
            self->session_.close();
            self->manager_.remove_session(self->session_id_);
        }, asio::detached);
}

// ---------------------------------------------------------------------------
// Socket shutdown
// ---------------------------------------------------------------------------

void WsSession::shutdown_socket() {
    std::visit([this](auto& stream) {
        using T = std::decay_t<decltype(stream)>;
        asio::error_code ec;

        if constexpr (std::is_same_v<T, asio::ip::tcp::socket>) {
            stream.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            stream.close(ec);
        } else {
            // TLS: shutdown the SSL layer, then close the underlying socket.
            // Use sync shutdown (best-effort, we're tearing down anyway).
            stream.shutdown(ec);
            stream.lowest_layer().close(ec);
        }
    }, stream_);
}

} // namespace chromatindb::relay::ws
