#include "db/net/connection.h"

#include "db/crypto/signing.h"
#include "db/crypto/thread_pool.h"
#include "db/crypto/verify_helpers.h"
#include "db/net/auth_helpers.h"
#include "db/util/endian.h"

#include <sodium.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>

namespace chromatindb::net {

Connection::Connection(asio::generic::stream_protocol::socket socket,
                       const identity::NodeIdentity& identity,
                       bool is_initiator,
                       bool is_uds)
    : socket_(std::move(socket))
    , identity_(identity)
    , is_initiator_(is_initiator)
    , is_uds_(is_uds) {
    // remote_addr_ is set by the factory methods (before run())
}

Connection::~Connection() {
    close();
}

Connection::Ptr Connection::create_inbound(asio::ip::tcp::socket socket,
                                            const identity::NodeIdentity& identity) {
    // Capture remote address before moving the socket
    std::string addr;
    asio::error_code ec;
    auto ep = socket.remote_endpoint(ec);
    if (!ec) {
        addr = ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    // Convert tcp socket to generic stream socket
    auto proto = asio::generic::stream_protocol(
        ep.protocol().family(), ep.protocol().type());
    asio::generic::stream_protocol::socket generic(
        socket.get_executor(), proto, socket.release());

    auto conn = Ptr(new Connection(std::move(generic), identity, false, false));
    conn->remote_addr_ = std::move(addr);
    return conn;
}

Connection::Ptr Connection::create_outbound(asio::ip::tcp::socket socket,
                                             const identity::NodeIdentity& identity) {
    // Capture remote address before moving the socket
    std::string addr;
    asio::error_code ec;
    auto ep = socket.remote_endpoint(ec);
    if (!ec) {
        addr = ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    // Convert tcp socket to generic stream socket
    auto proto = asio::generic::stream_protocol(
        ep.protocol().family(), ep.protocol().type());
    asio::generic::stream_protocol::socket generic(
        socket.get_executor(), proto, socket.release());

    auto conn = Ptr(new Connection(std::move(generic), identity, true, false));
    conn->remote_addr_ = std::move(addr);
    return conn;
}

Connection::Ptr Connection::create_uds_inbound(asio::local::stream_protocol::socket socket,
                                                const identity::NodeIdentity& identity) {
    // Convert local socket to generic stream socket
    auto proto = asio::generic::stream_protocol(AF_UNIX, SOCK_STREAM);
    asio::generic::stream_protocol::socket generic(
        socket.get_executor(), proto, socket.release());

    auto conn = Ptr(new Connection(std::move(generic), identity, false, true));
    conn->remote_addr_ = "uds";
    return conn;
}

Connection::Ptr Connection::create_uds_outbound(asio::local::stream_protocol::socket socket,
                                                  const identity::NodeIdentity& identity) {
    // Convert local socket to generic stream socket
    auto proto = asio::generic::stream_protocol(AF_UNIX, SOCK_STREAM);
    asio::generic::stream_protocol::socket generic(
        socket.get_executor(), proto, socket.release());

    auto conn = Ptr(new Connection(std::move(generic), identity, true, true));
    conn->remote_addr_ = "uds";
    return conn;
}

void Connection::close() {
    if (closed_) return;
    closed_ = true;
    send_signal_.cancel();  // Wake drain coroutine so it exits
    asio::error_code ec;
    socket_.shutdown(asio::socket_base::shutdown_both, ec);
    socket_.close(ec);
}

// =============================================================================
// Raw (unencrypted) frame IO -- for KEM exchange
// =============================================================================

asio::awaitable<bool> Connection::send_raw(std::span<const uint8_t> data) {
    // Write 4-byte BE length prefix + data
    uint32_t len = static_cast<uint32_t>(data.size());
    std::array<uint8_t, 4> header;
    chromatindb::util::store_u32_be(header.data(), len);

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

    uint32_t len = chromatindb::util::read_u32_be(header.data());

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
    static constexpr uint64_t NONCE_LIMIT = 1ULL << 63;
    if (send_counter_ >= NONCE_LIMIT) {
        spdlog::error("nonce exhaustion on send (counter={}), killing connection {}",
                      send_counter_, remote_addr_);
        close();
        co_return false;
    }
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
    static constexpr uint64_t NONCE_LIMIT = 1ULL << 63;
    if (recv_counter_ >= NONCE_LIMIT) {
        spdlog::error("nonce exhaustion on recv (counter={}), killing connection {}",
                      recv_counter_, remote_addr_);
        close();
        co_return std::nullopt;
    }
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
    // UDS connections are always trusted (local transport, no PQ needed)
    bool peer_is_trusted;
    if (is_uds_) {
        peer_is_trusted = true;
    } else {
        // TCP: determine trust based on remote IP
        asio::error_code ec;
        auto gen_ep = socket_.remote_endpoint(ec);
        if (!ec && trust_check_) {
            // Extract IP address from generic endpoint's sockaddr
            const void* data = gen_ep.data();
            if (gen_ep.protocol().family() == AF_INET) {
                const auto* sa = static_cast<const sockaddr_in*>(data);
                asio::ip::address_v4::bytes_type bytes;
                std::memcpy(bytes.data(), &sa->sin_addr, 4);
                peer_is_trusted = trust_check_(asio::ip::address_v4(bytes));
            } else if (gen_ep.protocol().family() == AF_INET6) {
                const auto* sa6 = static_cast<const sockaddr_in6*>(data);
                asio::ip::address_v6::bytes_type bytes;
                std::memcpy(bytes.data(), &sa6->sin6_addr, 16);
                peer_is_trusted = trust_check_(asio::ip::address_v6(bytes));
            } else {
                peer_is_trusted = false;
            }
        } else {
            peer_is_trusted = false;
        }
    }

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

        // CRYPTO-03: Auth exchange over encrypted channel
        // Initiator sends first (matches PQ path order, prevents nonce desync)

        // Sign session fingerprint with our signing key
        auto sig = identity_.sign(session_keys_.session_fingerprint);
        auto auth_payload = chromatindb::net::encode_auth_payload(local_role_, identity_.public_key(), sig);
        auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
        if (!co_await send_encrypted(auth_msg)) {
            spdlog::warn("handshake: failed to send auth (lightweight initiator)");
            co_return false;
        }

        // Receive responder's auth
        auto resp_auth_raw = co_await recv_encrypted();
        if (!resp_auth_raw) {
            spdlog::warn("handshake: failed to receive peer auth (lightweight initiator)");
            co_return false;
        }

        auto resp_transport = TransportCodec::decode(*resp_auth_raw);
        if (!resp_transport || resp_transport->type != wire::TransportMsgType_AuthSignature) {
            spdlog::warn("handshake: invalid auth message from peer (lightweight initiator)");
            co_return false;
        }

        auto auth = chromatindb::net::decode_auth_payload(std::span{resp_transport->payload});
        if (!auth) {
            spdlog::warn("handshake: malformed auth payload (lightweight initiator)");
            co_return false;
        }

        // Verify pubkey matches TrustedHello (prevents MitM substitution)
        if (auth->pubkey.size() != resp_signing_pk.size() ||
            std::memcmp(auth->pubkey.data(), resp_signing_pk.data(), resp_signing_pk.size()) != 0) {
            spdlog::warn("handshake: auth pubkey mismatch (lightweight initiator)");
            co_return false;
        }

        bool valid = co_await chromatindb::crypto::verify_with_offload(
            pool_, session_keys_.session_fingerprint, auth->signature, auth->pubkey);
        if (!valid) {
            spdlog::warn("handshake: peer auth signature invalid (lightweight initiator)");
            co_return false;
        }

        peer_role_ = auth->role;
        peer_pubkey_ = std::move(auth->pubkey);
        authenticated_ = true;
        spdlog::info("handshake complete (initiator, lightweight, authenticated, peer_role={})",
                     role_name(peer_role_));
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
        auto auth_payload = chromatindb::net::encode_auth_payload(local_role_, identity_.public_key(), sig);

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

        auto auth = chromatindb::net::decode_auth_payload(std::span{resp_decoded->payload});
        if (!auth) co_return false;
        auto& resp_pk = auth->pubkey;
        auto& resp_sig = auth->signature;

        bool valid = co_await chromatindb::crypto::verify_with_offload(
            pool_, session_keys_.session_fingerprint, resp_sig, resp_pk);
        if (!valid) {
            spdlog::warn("handshake: peer auth signature invalid (fallback)");
            co_return false;
        }

        peer_role_ = auth->role;
        peer_pubkey_ = std::move(resp_pk);
        authenticated_ = true;
        spdlog::info("handshake complete (initiator, PQ, trust mismatch, peer_role={})",
                     role_name(peer_role_));
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
    auto auth_payload = chromatindb::net::encode_auth_payload(local_role_, identity_.public_key(), sig);

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

    auto auth = chromatindb::net::decode_auth_payload(std::span{decoded->payload});
    if (!auth) co_return false;
    auto& resp_pk = auth->pubkey;
    auto& resp_sig = auth->signature;

    bool valid = co_await chromatindb::crypto::verify_with_offload(
        pool_, session_keys_.session_fingerprint, resp_sig, resp_pk);
    if (!valid) {
        spdlog::warn("handshake: peer auth signature invalid");
        co_return false;
    }

    peer_role_ = auth->role;
    peer_pubkey_ = std::move(resp_pk);
    authenticated_ = true;
    spdlog::info("handshake complete (initiator, PQ, peer_role={})",
                 role_name(peer_role_));
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

    // CRYPTO-03: Auth exchange over encrypted channel
    // Responder receives first, then sends (initiator sends first)

    // Receive initiator's auth
    auto init_auth_raw = co_await recv_encrypted();
    if (!init_auth_raw) {
        spdlog::warn("handshake: failed to receive peer auth (lightweight responder)");
        co_return false;
    }

    auto init_transport = TransportCodec::decode(*init_auth_raw);
    if (!init_transport || init_transport->type != wire::TransportMsgType_AuthSignature) {
        spdlog::warn("handshake: invalid auth message from peer (lightweight responder)");
        co_return false;
    }

    auto auth = chromatindb::net::decode_auth_payload(std::span{init_transport->payload});
    if (!auth) {
        spdlog::warn("handshake: malformed auth payload (lightweight responder)");
        co_return false;
    }

    // Verify pubkey matches TrustedHello (prevents MitM substitution)
    if (auth->pubkey.size() != init_signing_pk.size() ||
        std::memcmp(auth->pubkey.data(), init_signing_pk.data(), init_signing_pk.size()) != 0) {
        spdlog::warn("handshake: auth pubkey mismatch (lightweight responder)");
        co_return false;
    }

    bool valid = co_await chromatindb::crypto::verify_with_offload(
        pool_, session_keys_.session_fingerprint, auth->signature, auth->pubkey);
    if (!valid) {
        spdlog::warn("handshake: peer auth signature invalid (lightweight responder)");
        co_return false;
    }

    // Send our auth
    auto sig = identity_.sign(session_keys_.session_fingerprint);
    auto auth_payload = chromatindb::net::encode_auth_payload(local_role_, identity_.public_key(), sig);
    auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
    if (!co_await send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth (lightweight responder)");
        co_return false;
    }

    peer_role_ = auth->role;
    peer_pubkey_ = std::move(auth->pubkey);
    authenticated_ = true;
    spdlog::info("handshake complete (responder, lightweight, authenticated, peer_role={})",
                 role_name(peer_role_));
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

    auto auth = chromatindb::net::decode_auth_payload(std::span{decoded->payload});
    if (!auth) co_return false;
    auto& init_pk = auth->pubkey;
    auto& init_sig = auth->signature;

    bool valid = co_await chromatindb::crypto::verify_with_offload(
        pool_, session_keys_.session_fingerprint, init_sig, init_pk);
    if (!valid) {
        spdlog::warn("handshake: peer auth signature invalid (fallback)");
        co_return false;
    }

    peer_role_ = auth->role;
    peer_pubkey_ = std::move(init_pk);

    // Send our auth
    auto sig = identity_.sign(session_keys_.session_fingerprint);
    auto auth_payload = chromatindb::net::encode_auth_payload(local_role_, identity_.public_key(), sig);

    auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
    if (!co_await send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth (fallback)");
        co_return false;
    }

    authenticated_ = true;
    spdlog::info("handshake complete (responder, PQ, trust mismatch, peer_role={})",
                 role_name(peer_role_));
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

    auto auth = chromatindb::net::decode_auth_payload(std::span{decoded->payload});
    if (!auth) co_return false;
    auto& init_pk = auth->pubkey;
    auto& init_sig = auth->signature;

    bool valid = co_await chromatindb::crypto::verify_with_offload(
        pool_, session_keys_.session_fingerprint, init_sig, init_pk);
    if (!valid) {
        spdlog::warn("handshake: peer auth signature invalid");
        co_return false;
    }

    peer_role_ = auth->role;
    peer_pubkey_ = std::move(init_pk);

    // Send our auth
    auto sig = identity_.sign(session_keys_.session_fingerprint);
    auto auth_payload = chromatindb::net::encode_auth_payload(local_role_, identity_.public_key(), sig);

    auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
    if (!co_await send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth");
        co_return false;
    }

    authenticated_ = true;
    spdlog::info("handshake complete (responder, PQ, peer_role={})",
                 role_name(peer_role_));
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

        // Update last-recv time for keepalive (any decoded message resets silence)
        last_recv_time_ = std::chrono::steady_clock::now();

        // Check for chunked sub-frame mode (first byte == 0x01).
        // FlatBuffer messages never start with 0x01 (they start with a 4-byte
        // size prefix followed by the root table offset), so this is unambiguous.
        if (!msg->empty() && (*msg)[0] == 0x01) {
            auto reassembled = co_await recv_chunked(*msg);
            if (!reassembled) {
                spdlog::warn("chunked reassembly failed");
                break;
            }
            if (!authenticated_) {
                spdlog::error("received chunked message before auth from {}", remote_addr_);
                break;
            }
            if (message_cb_) {
                message_cb_(shared_from_this(),
                            static_cast<wire::TransportMsgType>(reassembled->type),
                            std::move(reassembled->payload),
                            reassembled->request_id);
            }
            continue;
        }

        auto decoded = TransportCodec::decode(*msg);
        if (!decoded) {
            spdlog::warn("received invalid transport message");
            continue;
        }

        if (!authenticated_) {
            spdlog::error("received message before authentication from {}", remote_addr_);
            break;
        }

        switch (decoded->type) {
            case wire::TransportMsgType_Ping: {
                // Reply with Pong -- through send queue for AEAD nonce ordering
                std::span<const uint8_t> empty{};
                co_await send_message(wire::TransportMsgType_Pong, empty);
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
                                std::move(decoded->payload), decoded->request_id);
                }
                break;
        }
    }
}

// =============================================================================
// Chunked sub-frame support
// =============================================================================

asio::awaitable<std::optional<Connection::ReassembledChunked>> Connection::recv_chunked(
    const std::vector<uint8_t>& first_frame) {
    // Parse chunked header: [flags:1][type:1][request_id:4BE][total_size:8BE][extra...]
    constexpr size_t CHUNKED_HEADER_SIZE = 14;
    if (first_frame.size() < CHUNKED_HEADER_SIZE) {
        spdlog::warn("chunked header too short: {} bytes", first_frame.size());
        co_return std::nullopt;
    }
    if (first_frame[0] != 0x01) {
        spdlog::warn("chunked header: invalid flags byte 0x{:02x}", first_frame[0]);
        co_return std::nullopt;
    }

    uint8_t type = first_frame[1];
    uint32_t request_id = chromatindb::util::read_u32_be(first_frame.data() + 2);
    uint64_t total_payload_size = chromatindb::util::read_u64_be(first_frame.data() + 6);

    // Validate total size against MAX_BLOB_DATA_SIZE
    if (total_payload_size > MAX_BLOB_DATA_SIZE) {
        spdlog::error("chunked reassembly: declared size {} exceeds MAX_BLOB_DATA_SIZE {}",
                      total_payload_size, MAX_BLOB_DATA_SIZE);
        co_return std::nullopt;
    }

    // Pre-allocate payload and copy any extra metadata from the header
    std::vector<uint8_t> payload;
    payload.reserve(static_cast<size_t>(total_payload_size));
    if (first_frame.size() > CHUNKED_HEADER_SIZE) {
        payload.insert(payload.end(),
                       first_frame.begin() + CHUNKED_HEADER_SIZE,
                       first_frame.end());
    }

    // Read data sub-frames until zero-length sentinel
    while (true) {
        auto chunk = co_await recv_encrypted();
        if (!chunk) {
            spdlog::warn("chunked reassembly: connection lost mid-stream");
            co_return std::nullopt;
        }
        last_recv_time_ = std::chrono::steady_clock::now();

        // Zero-length sentinel = end of chunked sequence
        if (chunk->empty()) break;

        payload.insert(payload.end(), chunk->begin(), chunk->end());

        // Safety check: accumulated > total_payload_size (with small tolerance for metadata)
        if (payload.size() > static_cast<size_t>(total_payload_size) + 64) {
            spdlog::error("chunked reassembly: exceeded declared size ({} > {})",
                          payload.size(), total_payload_size);
            co_return std::nullopt;
        }
    }

    co_return ReassembledChunked{type, request_id, std::move(payload)};
}

asio::awaitable<bool> Connection::send_message_chunked(
    wire::TransportMsgType type,
    std::span<const uint8_t> payload,
    uint32_t request_id,
    std::span<const uint8_t> extra_metadata) {
    if (closed_ || closing_) co_return false;
    if (send_queue_.size() >= MAX_SEND_QUEUE) {
        spdlog::warn("send queue full ({} messages), disconnecting {}",
                     MAX_SEND_QUEUE, remote_addr_);
        close();
        co_return false;
    }

    // Build 14-byte chunked header: [0x01][type][request_id 4BE][total_size 8BE] + extra_metadata
    constexpr size_t CHUNKED_HEADER_SIZE = 14;
    std::vector<uint8_t> header(CHUNKED_HEADER_SIZE + extra_metadata.size());
    header[0] = 0x01;  // CHUNKED_BEGIN
    header[1] = static_cast<uint8_t>(type);
    chromatindb::util::store_u32_be(header.data() + 2, request_id);
    chromatindb::util::store_u64_be(header.data() + 6, payload.size());
    if (!extra_metadata.empty()) {
        std::copy(extra_metadata.begin(), extra_metadata.end(),
                  header.begin() + CHUNKED_HEADER_SIZE);
    }

    // Enqueue as a single PendingMessage with is_chunked=true.
    // drain_send_queue will atomically send: header + chunks + sentinel.
    // This prevents interleaving with other messages (Pitfall 1 from research).
    bool result = false;
    asio::steady_timer completion(socket_.get_executor());
    completion.expires_after(std::chrono::hours(24));

    PendingMessage pm;
    pm.encoded = std::move(header);
    pm.completion = &completion;
    pm.result_ptr = &result;
    pm.is_chunked = true;
    pm.chunked_payload.assign(payload.begin(), payload.end());

    send_queue_.push_back(std::move(pm));
    send_signal_.cancel();

    auto [ec] = co_await completion.async_wait(use_nothrow);
    co_return result;
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

    // Run message_loop and drain_send_queue concurrently.
    // When message_loop exits (socket closed/error), close() sets closed_=true
    // and cancels send_signal_, causing drain_send_queue to exit.
    using namespace asio::experimental::awaitable_operators;
    co_await (message_loop() && drain_send_queue());

    if (close_cb_) close_cb_(self, received_goodbye_);
    co_return true;
}

asio::awaitable<bool> Connection::send_message(wire::TransportMsgType type,
                                                std::span<const uint8_t> payload,
                                                uint32_t request_id) {
    // Automatically use chunked mode for large payloads
    if (payload.size() >= STREAMING_THRESHOLD) {
        co_return co_await send_message_chunked(type, payload, request_id);
    }
    auto msg = TransportCodec::encode(type, payload, request_id);
    co_return co_await enqueue_send(std::move(msg));
}

asio::awaitable<bool> Connection::enqueue_send(std::vector<uint8_t> encoded) {
    if (closed_ || closing_) co_return false;
    if (send_queue_.size() >= MAX_SEND_QUEUE) {
        spdlog::warn("send queue full ({} messages), disconnecting {}",
                     MAX_SEND_QUEUE, remote_addr_);
        close();
        co_return false;
    }
    bool result = false;
    asio::steady_timer completion(socket_.get_executor());
    completion.expires_after(std::chrono::hours(24));

    send_queue_.push_back({std::move(encoded), &completion, &result});
    send_signal_.cancel();  // Wake drain coroutine

    auto [ec] = co_await completion.async_wait(use_nothrow);
    // ec == operation_aborted means drain coroutine processed our message
    co_return result;
}

asio::awaitable<void> Connection::drain_send_queue() {
    auto self = shared_from_this();
    drain_running_ = true;

    while (!closed_) {
        while (!send_queue_.empty() && !closed_) {
            auto msg = std::move(send_queue_.front());
            send_queue_.pop_front();

            bool ok;
            if (msg.is_chunked) {
                // Atomic chunked send — header + data chunks + sentinel.
                // All sub-frames are sent sequentially without interleaving other
                // messages (prevents AEAD nonce desync per Pitfall 1).
                ok = co_await send_encrypted(msg.encoded);  // Chunked header
                if (ok) {
                    // Send data sub-frames in STREAMING_THRESHOLD-sized chunks
                    size_t offset = 0;
                    while (ok && offset < msg.chunked_payload.size()) {
                        size_t len = std::min(static_cast<size_t>(STREAMING_THRESHOLD),
                                              msg.chunked_payload.size() - offset);
                        std::span<const uint8_t> chunk(
                            msg.chunked_payload.data() + offset, len);
                        ok = co_await send_encrypted(chunk);
                        offset += len;
                    }
                    if (ok) {
                        // Send zero-length sentinel
                        std::span<const uint8_t> empty{};
                        ok = co_await send_encrypted(empty);
                    }
                }
            } else {
                ok = co_await send_encrypted(msg.encoded);
            }

            // Signal the waiting caller
            if (msg.result_ptr) *msg.result_ptr = ok;
            if (msg.completion) msg.completion->cancel();

            if (!ok) {
                // Write failed -- drain remaining messages as failures
                while (!send_queue_.empty()) {
                    auto& m = send_queue_.front();
                    if (m.result_ptr) *m.result_ptr = false;
                    if (m.completion) m.completion->cancel();
                    send_queue_.pop_front();
                }
                break;
            }
        }

        if (closed_) break;

        // Wait for new messages (timer-cancel wakeup pattern)
        send_signal_.expires_after(std::chrono::hours(24));
        auto [ec] = co_await send_signal_.async_wait(use_nothrow);
        // ec == operation_aborted means new message was enqueued (cancel woke us)
    }

    // Drain remaining on close: signal all waiters as failed
    while (!send_queue_.empty()) {
        auto& m = send_queue_.front();
        if (m.result_ptr) *m.result_ptr = false;
        if (m.completion) m.completion->cancel();
        send_queue_.pop_front();
    }

    drain_running_ = false;
}

asio::awaitable<void> Connection::close_gracefully() {
    if (closed_ || !authenticated_) {
        close();
        co_return;
    }

    // Enqueue Goodbye through the queue. We encode it manually and enqueue
    // directly so we can set closing_ AFTER the message enters the queue
    // but BEFORE we await completion. This ensures Goodbye is accepted
    // while new sends from other coroutines are rejected.
    std::span<const uint8_t> empty{};
    auto goodbye = TransportCodec::encode(wire::TransportMsgType_Goodbye, empty);

    // Enqueue without going through send_message() so we can control
    // the closing_ flag timing precisely.
    if (send_queue_.size() >= MAX_SEND_QUEUE) {
        close();
        co_return;
    }
    bool result = false;
    asio::steady_timer completion(socket_.get_executor());
    completion.expires_after(std::chrono::hours(24));
    send_queue_.push_back({std::move(goodbye), &completion, &result});
    closing_ = true;  // Now reject all subsequent sends
    send_signal_.cancel();
    auto [ec] = co_await completion.async_wait(use_nothrow);

    close();
}

} // namespace chromatindb::net
