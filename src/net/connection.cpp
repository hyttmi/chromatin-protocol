#include "net/connection.h"

#include <spdlog/spdlog.h>
#include <cstring>

namespace chromatin::net {

Connection::Connection(asio::ip::tcp::socket socket,
                       const identity::NodeIdentity& identity,
                       bool is_initiator)
    : socket_(std::move(socket))
    , identity_(identity)
    , is_initiator_(is_initiator) {}

Connection::~Connection() {
    close();
}

Connection::Ptr Connection::create_inbound(asio::ip::tcp::socket socket,
                                            const identity::NodeIdentity& identity) {
    return Ptr(new Connection(std::move(socket), identity, false));
}

Connection::Ptr Connection::create_outbound(asio::ip::tcp::socket socket,
                                             const identity::NodeIdentity& identity) {
    return Ptr(new Connection(std::move(socket), identity, true));
}

void Connection::close() {
    if (closed_) return;
    closed_ = true;
    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

// =============================================================================
// Raw (unencrypted) frame IO -- for KEM exchange
// =============================================================================

asio::awaitable<bool> Connection::send_raw(std::span<const uint8_t> data) {
    // Write 4-byte BE length prefix + data
    uint32_t len = static_cast<uint32_t>(data.size());
    std::array<uint8_t, 4> header;
    header[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
    header[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>(len & 0xFF);

    auto [ec1, _n1] = co_await asio::async_write(
        socket_, asio::buffer(header), use_nothrow);
    if (ec1) co_return false;

    auto [ec2, _n2] = co_await asio::async_write(
        socket_, asio::buffer(data.data(), data.size()), use_nothrow);
    co_return !ec2;
}

asio::awaitable<std::optional<std::vector<uint8_t>>> Connection::recv_raw() {
    // Read 4-byte length header
    std::array<uint8_t, 4> header;
    auto [ec1, _n1] = co_await asio::async_read(
        socket_, asio::buffer(header), use_nothrow);
    if (ec1) co_return std::nullopt;

    uint32_t len = (static_cast<uint32_t>(header[0]) << 24) |
                   (static_cast<uint32_t>(header[1]) << 16) |
                   (static_cast<uint32_t>(header[2]) << 8) |
                   static_cast<uint32_t>(header[3]);

    if (len > MAX_FRAME_SIZE) {
        spdlog::warn("received frame exceeding max size: {}", len);
        co_return std::nullopt;
    }

    std::vector<uint8_t> data(len);
    auto [ec2, _n2] = co_await asio::async_read(
        socket_, asio::buffer(data), use_nothrow);
    if (ec2) co_return std::nullopt;

    co_return data;
}

// =============================================================================
// Encrypted frame IO -- after handshake
// =============================================================================

asio::awaitable<bool> Connection::send_encrypted(std::span<const uint8_t> plaintext) {
    auto nonce = make_nonce(send_counter_++);
    std::span<const uint8_t> empty_ad{};
    auto ciphertext = crypto::AEAD::encrypt(plaintext, empty_ad, nonce, session_keys_.send_key.span());
    co_return co_await send_raw(ciphertext);
}

asio::awaitable<std::optional<std::vector<uint8_t>>> Connection::recv_encrypted() {
    // Read encrypted frame: [4B header][ciphertext]
    auto raw = co_await recv_raw();
    if (!raw) co_return std::nullopt;

    // Decrypt the raw data (it IS the ciphertext, not a framed message)
    // The raw data we received is the ciphertext (no header -- we already stripped it in recv_raw)
    auto nonce = make_nonce(recv_counter_++);
    std::span<const uint8_t> empty_ad{};
    auto plaintext = crypto::AEAD::decrypt(*raw, empty_ad, nonce, session_keys_.recv_key.span());
    if (!plaintext) {
        spdlog::warn("AEAD decrypt failed (nonce desync or tampered)");
        co_return std::nullopt;
    }
    co_return plaintext;
}

// =============================================================================
// Handshake
// =============================================================================

asio::awaitable<bool> Connection::do_handshake() {
    if (is_initiator_) {
        HandshakeInitiator hs(identity_);

        // Step 1: Send KEM pubkey
        auto kem_pk_msg = hs.start();
        if (!co_await send_raw(kem_pk_msg)) {
            spdlog::warn("handshake: failed to send KEM pubkey");
            co_return false;
        }

        // Step 2: Receive KEM ciphertext
        auto kem_ct = co_await recv_raw();
        if (!kem_ct) {
            spdlog::warn("handshake: failed to receive KEM ciphertext");
            co_return false;
        }

        auto err = hs.receive_kem_ciphertext(*kem_ct);
        if (err != HandshakeError::Success) {
            spdlog::warn("handshake: KEM ciphertext processing failed");
            co_return false;
        }

        // Keys are derived now -- switch to encrypted communication
        // Temporarily take the keys for auth phase
        auto temp_keys = hs.take_session_keys();

        // Step 3: Send encrypted auth (using handshake's internal counters)
        // The handshake creates the encrypted frame internally
        HandshakeInitiator hs2(identity_);
        // Actually, we can't re-derive keys. Let me restructure.
        // The handshake class already encrypts auth messages internally.
        // We just need to send the bytes it produces over the network.

        // Re-approach: don't take_session_keys yet. Let the handshake manage counters.
        // Problem: we already called take_session_keys...

        // Simplest fix: recreate the handshake flow using raw keys directly.
        // Actually, the cleaner approach: the handshake returns encrypted frames.
        // We just send those frames as raw (they're already encrypted + framed).
        // But the handshake's write_frame adds its own length-prefix framing inside the
        // encrypted data. We need to be careful about framing layers.

        // Let me reconsider: HandshakeInitiator.create_auth_message() returns:
        // write_frame(TransportMessage, key, counter) which is:
        //   [4B header][ciphertext]
        // We send that as raw, which adds ANOTHER 4B header on top.
        // The receiver calls recv_raw() which strips the outer header,
        // giving them [4B header][ciphertext] which they pass to read_frame().

        // Wait, that's double-framing. The issue: write_frame already creates
        // [4B header][ciphertext]. If we send_raw() that, recv_raw() returns
        // [4B header][ciphertext]. Then we'd call read_frame() on that which
        // correctly parses the inner header + ciphertext. This actually works!

        // Let's restart cleanly. Don't take keys early.
        session_keys_ = std::move(temp_keys);

        // Step 3: Send auth -- create the encrypted auth frame
        auto sig = identity_.sign(session_keys_.session_fingerprint);
        // Encode auth payload
        std::vector<uint8_t> auth_payload;
        auto pk = identity_.public_key();
        uint32_t pk_size = static_cast<uint32_t>(pk.size());
        auth_payload.push_back(static_cast<uint8_t>(pk_size & 0xFF));
        auth_payload.push_back(static_cast<uint8_t>((pk_size >> 8) & 0xFF));
        auth_payload.push_back(static_cast<uint8_t>((pk_size >> 16) & 0xFF));
        auth_payload.push_back(static_cast<uint8_t>((pk_size >> 24) & 0xFF));
        auth_payload.insert(auth_payload.end(), pk.begin(), pk.end());
        auth_payload.insert(auth_payload.end(), sig.begin(), sig.end());

        auto msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
        if (!co_await send_encrypted(msg)) {
            spdlog::warn("handshake: failed to send auth");
            co_return false;
        }

        // Step 4: Receive responder's auth
        auto resp_auth = co_await recv_encrypted();
        if (!resp_auth) {
            spdlog::warn("handshake: failed to receive peer auth");
            co_return false;
        }

        auto decoded = TransportCodec::decode(*resp_auth);
        if (!decoded || decoded->type != wire::TransportMsgType_AuthSignature) {
            spdlog::warn("handshake: invalid auth message from peer");
            co_return false;
        }

        // Parse auth payload
        if (decoded->payload.size() < 4) co_return false;
        uint32_t rpk_size = static_cast<uint32_t>(decoded->payload[0]) |
                            (static_cast<uint32_t>(decoded->payload[1]) << 8) |
                            (static_cast<uint32_t>(decoded->payload[2]) << 16) |
                            (static_cast<uint32_t>(decoded->payload[3]) << 24);
        if (decoded->payload.size() < 4 + rpk_size) co_return false;

        std::vector<uint8_t> resp_pk(decoded->payload.begin() + 4,
                                      decoded->payload.begin() + 4 + rpk_size);
        std::vector<uint8_t> resp_sig(decoded->payload.begin() + 4 + rpk_size,
                                       decoded->payload.end());

        bool valid = crypto::Signer::verify(
            session_keys_.session_fingerprint, resp_sig, resp_pk);
        if (!valid) {
            spdlog::warn("handshake: peer auth signature invalid");
            co_return false;
        }

        peer_pubkey_ = std::move(resp_pk);
        authenticated_ = true;
        spdlog::info("handshake complete (initiator)");
        co_return true;

    } else {
        // Responder side
        HandshakeResponder hs(identity_);

        // Step 1: Receive KEM pubkey
        auto kem_pk = co_await recv_raw();
        if (!kem_pk) {
            spdlog::warn("handshake: failed to receive KEM pubkey");
            co_return false;
        }

        auto [err, kem_ct_msg] = hs.receive_kem_pubkey(*kem_pk);
        if (err != HandshakeError::Success) {
            spdlog::warn("handshake: KEM pubkey processing failed");
            co_return false;
        }

        // Step 2: Send KEM ciphertext
        if (!co_await send_raw(kem_ct_msg)) {
            spdlog::warn("handshake: failed to send KEM ciphertext");
            co_return false;
        }

        // Keys derived now
        session_keys_ = hs.take_session_keys();

        // Step 3: Receive initiator's auth
        auto init_auth = co_await recv_encrypted();
        if (!init_auth) {
            spdlog::warn("handshake: failed to receive peer auth");
            co_return false;
        }

        auto decoded = TransportCodec::decode(*init_auth);
        if (!decoded || decoded->type != wire::TransportMsgType_AuthSignature) {
            spdlog::warn("handshake: invalid auth message from peer");
            co_return false;
        }

        // Parse and verify initiator auth
        if (decoded->payload.size() < 4) co_return false;
        uint32_t ipk_size = static_cast<uint32_t>(decoded->payload[0]) |
                            (static_cast<uint32_t>(decoded->payload[1]) << 8) |
                            (static_cast<uint32_t>(decoded->payload[2]) << 16) |
                            (static_cast<uint32_t>(decoded->payload[3]) << 24);
        if (decoded->payload.size() < 4 + ipk_size) co_return false;

        std::vector<uint8_t> init_pk(decoded->payload.begin() + 4,
                                      decoded->payload.begin() + 4 + ipk_size);
        std::vector<uint8_t> init_sig(decoded->payload.begin() + 4 + ipk_size,
                                       decoded->payload.end());

        bool valid = crypto::Signer::verify(
            session_keys_.session_fingerprint, init_sig, init_pk);
        if (!valid) {
            spdlog::warn("handshake: peer auth signature invalid");
            co_return false;
        }

        peer_pubkey_ = std::move(init_pk);

        // Step 4: Send our auth
        auto sig = identity_.sign(session_keys_.session_fingerprint);
        std::vector<uint8_t> auth_payload;
        auto pk = identity_.public_key();
        uint32_t pk_size = static_cast<uint32_t>(pk.size());
        auth_payload.push_back(static_cast<uint8_t>(pk_size & 0xFF));
        auth_payload.push_back(static_cast<uint8_t>((pk_size >> 8) & 0xFF));
        auth_payload.push_back(static_cast<uint8_t>((pk_size >> 16) & 0xFF));
        auth_payload.push_back(static_cast<uint8_t>((pk_size >> 24) & 0xFF));
        auth_payload.insert(auth_payload.end(), pk.begin(), pk.end());
        auth_payload.insert(auth_payload.end(), sig.begin(), sig.end());

        auto msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
        if (!co_await send_encrypted(msg)) {
            spdlog::warn("handshake: failed to send auth");
            co_return false;
        }

        authenticated_ = true;
        spdlog::info("handshake complete (responder)");
        co_return true;
    }
}

// =============================================================================
// Message loop
// =============================================================================

asio::awaitable<void> Connection::message_loop() {
    while (!closed_) {
        auto msg = co_await recv_encrypted();
        if (!msg) {
            if (!closed_) {
                spdlog::info("connection lost (read failed)");
            }
            break;
        }

        auto decoded = TransportCodec::decode(*msg);
        if (!decoded) {
            spdlog::warn("received invalid transport message");
            continue;
        }

        switch (decoded->type) {
            case wire::TransportMsgType_Ping: {
                // Reply with Pong
                std::span<const uint8_t> empty{};
                auto pong = TransportCodec::encode(wire::TransportMsgType_Pong, empty);
                co_await send_encrypted(pong);
                break;
            }
            case wire::TransportMsgType_Pong:
                // Heartbeat received -- nothing to do for now
                break;
            case wire::TransportMsgType_Goodbye:
                spdlog::info("peer sent goodbye");
                received_goodbye_ = true;
                close();
                break;
            default:
                if (message_cb_) {
                    message_cb_(shared_from_this(), decoded->type,
                                std::move(decoded->payload));
                }
                break;
        }
    }
}

// =============================================================================
// Connection lifecycle
// =============================================================================

asio::awaitable<bool> Connection::run() {
    auto self = shared_from_this();

    bool hs_ok = co_await do_handshake();
    if (!hs_ok) {
        close();
        if (close_cb_) close_cb_(self, false);
        co_return false;
    }

    co_await message_loop();

    if (close_cb_) close_cb_(self, received_goodbye_);
    co_return true;
}

asio::awaitable<bool> Connection::send_message(wire::TransportMsgType type,
                                                std::span<const uint8_t> payload) {
    auto msg = TransportCodec::encode(type, payload);
    co_return co_await send_encrypted(msg);
}

asio::awaitable<void> Connection::close_gracefully() {
    if (closed_ || !authenticated_) {
        close();
        co_return;
    }

    // Send goodbye message
    std::span<const uint8_t> empty{};
    auto goodbye = TransportCodec::encode(wire::TransportMsgType_Goodbye, empty);
    co_await send_encrypted(goodbye);

    close();
}

} // namespace chromatin::net
