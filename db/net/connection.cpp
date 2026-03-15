#include "db/net/connection.h"

#include "db/crypto/signing.h"

#include <sodium.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace chromatindb::net {

Connection::Connection(asio::ip::tcp::socket socket,
                       const identity::NodeIdentity& identity,
                       bool is_initiator)
    : socket_(std::move(socket))
    , identity_(identity)
    , is_initiator_(is_initiator) {
    // Capture remote address before any socket operations
    asio::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (!ec) {
        remote_addr_ = ep.address().to_string() + ":" + std::to_string(ep.port());
    }
}

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
    // Determine trust based on remote IP
    asio::error_code ec;
    auto remote_ep = socket_.remote_endpoint(ec);
    bool peer_is_trusted = !ec && trust_check_ && trust_check_(remote_ep.address());

    if (is_initiator_) {
        if (peer_is_trusted) {
            co_return co_await do_handshake_initiator_trusted();
        } else {
            co_return co_await do_handshake_initiator_pq();
        }
    } else {
        // Responder: read first message, then decide
        auto first_msg = co_await recv_raw();
        if (!first_msg) {
            spdlog::warn("handshake: failed to receive first message");
            co_return false;
        }

        auto decoded = TransportCodec::decode(*first_msg);
        if (!decoded) {
            spdlog::warn("handshake: invalid first message");
            co_return false;
        }

        if (decoded->type == wire::TransportMsgType_TrustedHello) {
            if (peer_is_trusted) {
                co_return co_await do_handshake_responder_trusted(
                    std::move(decoded->payload));
            } else {
                // Mismatch: initiator trusts us, we don't trust them -> force PQ
                co_return co_await do_handshake_responder_pq_fallback(
                    std::move(decoded->payload));
            }
        } else if (decoded->type == wire::TransportMsgType_KemPubkey) {
            co_return co_await do_handshake_responder_pq(std::move(*first_msg));
        } else {
            spdlog::warn("handshake: unexpected first message type");
            co_return false;
        }
    }
}

// =============================================================================
// Lightweight handshake: initiator trusts peer
// =============================================================================

asio::awaitable<bool> Connection::do_handshake_initiator_trusted() {
    // Generate 32-byte nonce
    std::array<uint8_t, 32> nonce_i{};
    randombytes_buf(nonce_i.data(), nonce_i.size());

    // Build TrustedHello payload: [nonce:32][signing_pubkey:2592]
    auto signing_pk = identity_.public_key();
    std::vector<uint8_t> payload;
    payload.reserve(32 + signing_pk.size());
    payload.insert(payload.end(), nonce_i.begin(), nonce_i.end());
    payload.insert(payload.end(), signing_pk.begin(), signing_pk.end());

    auto msg = TransportCodec::encode(wire::TransportMsgType_TrustedHello, payload);
    if (!co_await send_raw(msg)) {
        spdlog::warn("handshake: failed to send TrustedHello");
        co_return false;
    }

    // Read response
    auto response = co_await recv_raw();
    if (!response) {
        spdlog::warn("handshake: failed to receive response to TrustedHello");
        co_return false;
    }

    auto decoded = TransportCodec::decode(*response);
    if (!decoded) {
        spdlog::warn("handshake: invalid response to TrustedHello");
        co_return false;
    }

    if (decoded->type == wire::TransportMsgType_TrustedHello) {
        // Both sides trust: lightweight path
        constexpr size_t NONCE_SIZE = 32;
        size_t expected_size = NONCE_SIZE + crypto::Signer::PUBLIC_KEY_SIZE;
        if (decoded->payload.size() != expected_size) {
            spdlog::warn("handshake: invalid TrustedHello payload size");
            co_return false;
        }

        auto nonce_r = std::span<const uint8_t>(decoded->payload.data(), NONCE_SIZE);
        auto resp_signing_pk = std::span<const uint8_t>(
            decoded->payload.data() + NONCE_SIZE, crypto::Signer::PUBLIC_KEY_SIZE);

        session_keys_ = derive_lightweight_session_keys(
            nonce_i, nonce_r,
            signing_pk, resp_signing_pk,
            true);

        peer_pubkey_.assign(resp_signing_pk.begin(), resp_signing_pk.end());
        authenticated_ = true;
        spdlog::info("handshake complete (initiator, lightweight)");
        co_return true;

    } else if (decoded->type == wire::TransportMsgType_PQRequired) {
        // Mismatch: responder doesn't trust us -> fall back to PQ
        spdlog::info("handshake: peer requires PQ, falling back");

        // Generate KEM and proceed as PQ initiator
        HandshakeInitiator hs(identity_);
        auto kem_pk_msg = hs.start();
        if (!co_await send_raw(kem_pk_msg)) {
            spdlog::warn("handshake: failed to send KEM pubkey (fallback)");
            co_return false;
        }

        // Receive KEM ciphertext
        auto kem_ct = co_await recv_raw();
        if (!kem_ct) {
            spdlog::warn("handshake: failed to receive KEM ciphertext (fallback)");
            co_return false;
        }

        auto err = hs.receive_kem_ciphertext(*kem_ct);
        if (err != HandshakeError::Success) {
            spdlog::warn("handshake: KEM ciphertext processing failed (fallback)");
            co_return false;
        }

        session_keys_ = hs.take_session_keys();

        // Auth exchange (same as PQ initiator)
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

        auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
        if (!co_await send_encrypted(auth_msg)) {
            spdlog::warn("handshake: failed to send auth (fallback)");
            co_return false;
        }

        auto resp_auth = co_await recv_encrypted();
        if (!resp_auth) {
            spdlog::warn("handshake: failed to receive peer auth (fallback)");
            co_return false;
        }

        auto resp_decoded = TransportCodec::decode(*resp_auth);
        if (!resp_decoded || resp_decoded->type != wire::TransportMsgType_AuthSignature) {
            spdlog::warn("handshake: invalid auth message from peer (fallback)");
            co_return false;
        }

        if (resp_decoded->payload.size() < 4) co_return false;
        uint32_t rpk_size = static_cast<uint32_t>(resp_decoded->payload[0]) |
                            (static_cast<uint32_t>(resp_decoded->payload[1]) << 8) |
                            (static_cast<uint32_t>(resp_decoded->payload[2]) << 16) |
                            (static_cast<uint32_t>(resp_decoded->payload[3]) << 24);
        if (resp_decoded->payload.size() < 4 + rpk_size) co_return false;

        std::vector<uint8_t> resp_pk(resp_decoded->payload.begin() + 4,
                                      resp_decoded->payload.begin() + 4 + rpk_size);
        std::vector<uint8_t> resp_sig(resp_decoded->payload.begin() + 4 + rpk_size,
                                       resp_decoded->payload.end());

        bool valid = crypto::Signer::verify(
            session_keys_.session_fingerprint, resp_sig, resp_pk);
        if (!valid) {
            spdlog::warn("handshake: peer auth signature invalid (fallback)");
            co_return false;
        }

        peer_pubkey_ = std::move(resp_pk);
        authenticated_ = true;
        spdlog::info("handshake complete (initiator, PQ, trust mismatch)");
        co_return true;

    } else {
        spdlog::warn("handshake: unexpected response type to TrustedHello");
        co_return false;
    }
}

// =============================================================================
// PQ handshake: initiator path (unchanged from original)
// =============================================================================

asio::awaitable<bool> Connection::do_handshake_initiator_pq() {
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

    session_keys_ = hs.take_session_keys();

    // Step 3: Send encrypted auth
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
    spdlog::info("handshake complete (initiator, PQ)");
    co_return true;
}

// =============================================================================
// Lightweight handshake: responder trusts peer
// =============================================================================

asio::awaitable<bool> Connection::do_handshake_responder_trusted(
    std::vector<uint8_t> payload) {

    // Parse initiator's TrustedHello: [nonce:32][signing_pubkey:2592]
    constexpr size_t NONCE_SIZE = 32;
    size_t expected_size = NONCE_SIZE + crypto::Signer::PUBLIC_KEY_SIZE;
    if (payload.size() != expected_size) {
        spdlog::warn("handshake: invalid TrustedHello payload size from initiator");
        co_return false;
    }

    auto nonce_i = std::span<const uint8_t>(payload.data(), NONCE_SIZE);
    auto init_signing_pk = std::span<const uint8_t>(
        payload.data() + NONCE_SIZE, crypto::Signer::PUBLIC_KEY_SIZE);

    // Generate our nonce
    std::array<uint8_t, 32> nonce_r{};
    randombytes_buf(nonce_r.data(), nonce_r.size());

    // Build our TrustedHello: [nonce:32][signing_pubkey:2592]
    auto our_pk = identity_.public_key();
    std::vector<uint8_t> resp_payload;
    resp_payload.reserve(32 + our_pk.size());
    resp_payload.insert(resp_payload.end(), nonce_r.begin(), nonce_r.end());
    resp_payload.insert(resp_payload.end(), our_pk.begin(), our_pk.end());

    auto msg = TransportCodec::encode(wire::TransportMsgType_TrustedHello, resp_payload);
    if (!co_await send_raw(msg)) {
        spdlog::warn("handshake: failed to send TrustedHello response");
        co_return false;
    }

    // Derive session keys (responder side)
    session_keys_ = derive_lightweight_session_keys(
        nonce_i, nonce_r,
        init_signing_pk, our_pk,
        false);

    peer_pubkey_.assign(init_signing_pk.begin(), init_signing_pk.end());
    authenticated_ = true;
    spdlog::info("handshake complete (responder, lightweight)");
    co_return true;
}

// =============================================================================
// PQ fallback: responder received TrustedHello but doesn't trust
// =============================================================================

asio::awaitable<bool> Connection::do_handshake_responder_pq_fallback(
    std::vector<uint8_t> payload) {

    // Send PQRequired (empty payload) to force PQ handshake
    std::span<const uint8_t> empty{};
    auto pq_req = TransportCodec::encode(wire::TransportMsgType_PQRequired, empty);
    if (!co_await send_raw(pq_req)) {
        spdlog::warn("handshake: failed to send PQRequired");
        co_return false;
    }

    // Wait for KemPubkey from initiator
    auto kem_pk_raw = co_await recv_raw();
    if (!kem_pk_raw) {
        spdlog::warn("handshake: failed to receive KEM pubkey after PQRequired");
        co_return false;
    }

    // Process as standard PQ responder
    HandshakeResponder hs(identity_);
    auto [err, kem_ct_msg] = hs.receive_kem_pubkey(*kem_pk_raw);
    if (err != HandshakeError::Success) {
        spdlog::warn("handshake: KEM pubkey processing failed (fallback)");
        co_return false;
    }

    // Send KEM ciphertext
    if (!co_await send_raw(kem_ct_msg)) {
        spdlog::warn("handshake: failed to send KEM ciphertext (fallback)");
        co_return false;
    }

    session_keys_ = hs.take_session_keys();

    // Receive initiator's auth
    auto init_auth = co_await recv_encrypted();
    if (!init_auth) {
        spdlog::warn("handshake: failed to receive peer auth (fallback)");
        co_return false;
    }

    auto decoded = TransportCodec::decode(*init_auth);
    if (!decoded || decoded->type != wire::TransportMsgType_AuthSignature) {
        spdlog::warn("handshake: invalid auth message from peer (fallback)");
        co_return false;
    }

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
        spdlog::warn("handshake: peer auth signature invalid (fallback)");
        co_return false;
    }

    peer_pubkey_ = std::move(init_pk);

    // Send our auth
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

    auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
    if (!co_await send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth (fallback)");
        co_return false;
    }

    authenticated_ = true;
    spdlog::info("handshake complete (responder, PQ, trust mismatch)");
    co_return true;
}

// =============================================================================
// PQ handshake: responder path (first message already received)
// =============================================================================

asio::awaitable<bool> Connection::do_handshake_responder_pq(
    std::vector<uint8_t> first_msg) {

    HandshakeResponder hs(identity_);

    // Process the already-received KemPubkey
    auto [err, kem_ct_msg] = hs.receive_kem_pubkey(first_msg);
    if (err != HandshakeError::Success) {
        spdlog::warn("handshake: KEM pubkey processing failed");
        co_return false;
    }

    // Send KEM ciphertext
    if (!co_await send_raw(kem_ct_msg)) {
        spdlog::warn("handshake: failed to send KEM ciphertext");
        co_return false;
    }

    session_keys_ = hs.take_session_keys();

    // Receive initiator's auth
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

    // Send our auth
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

    auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
    if (!co_await send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth");
        co_return false;
    }

    authenticated_ = true;
    spdlog::info("handshake complete (responder, PQ)");
    co_return true;
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

    // Notify that handshake succeeded (before message loop).
    // This allows PeerManager to set up message routing and start sync.
    if (ready_cb_) ready_cb_(self);

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

} // namespace chromatindb::net
