#include "relay/core/uds_multiplexer.h"
#include "relay/core/subscription_tracker.h"
#include "relay/translate/translator.h"
#include "relay/translate/type_registry.h"
#include "relay/util/endian.h"
#include "relay/util/hex.h"
#include "relay/wire/aead.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"
#include "relay/ws/ws_frame.h"
#include "relay/ws/ws_session.h"

#include <oqs/oqs.h>
#include <oqs/sha3.h>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace chromatindb::relay::core {

using chromatindb::wire::TransportMsgType_TrustedHello;
using chromatindb::wire::TransportMsgType_AuthSignature;

static constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

/// Maximum frame payload size (110 MiB -- matches db/net/framing.h).
static constexpr uint32_t MAX_FRAME_SIZE = 110 * 1024 * 1024;

/// ML-DSA-87 key/signature sizes.
static constexpr size_t SIGNING_PK_SIZE = 2592;
static constexpr size_t SIGNATURE_SIZE = 4627;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UdsMultiplexer::UdsMultiplexer(asio::io_context& ioc,
                               std::string uds_path,
                               const identity::RelayIdentity& identity,
                               RequestRouter& router,
                               ws::SessionManager& sessions)
    : ioc_(ioc)
    , uds_path_(std::move(uds_path))
    , identity_(identity)
    , router_(router)
    , sessions_(sessions)
    , socket_(ioc) {}

void UdsMultiplexer::start() {
    auto self_ptr = this;
    asio::co_spawn(ioc_,
        [self_ptr]() -> asio::awaitable<void> {
            co_await self_ptr->connect_loop();
        }, asio::detached);
}

bool UdsMultiplexer::is_connected() const {
    return connected_;
}

void UdsMultiplexer::set_tracker(SubscriptionTracker* t) {
    tracker_ = t;
}

// ---------------------------------------------------------------------------
// Send (public API)
// ---------------------------------------------------------------------------

bool UdsMultiplexer::send(std::vector<uint8_t> transport_msg) {
    if (!connected_) return false;

    send_queue_.push_back(std::move(transport_msg));

    if (!draining_) {
        draining_ = true;
        auto self_ptr = this;
        asio::co_spawn(ioc_,
            [self_ptr]() -> asio::awaitable<void> {
                co_await self_ptr->drain_send_queue();
            }, asio::detached);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Send queue drain
// ---------------------------------------------------------------------------

asio::awaitable<void> UdsMultiplexer::drain_send_queue() {
    while (!send_queue_.empty() && connected_) {
        auto msg = std::move(send_queue_.front());
        send_queue_.pop_front();

        if (!co_await send_encrypted(msg)) {
            spdlog::error("UDS send_encrypted failed -- disconnecting");
            connected_ = false;
            break;
        }
    }
    draining_ = false;
}

// ---------------------------------------------------------------------------
// Connect loop with jittered backoff
// ---------------------------------------------------------------------------

asio::awaitable<void> UdsMultiplexer::connect_loop() {
    static thread_local std::mt19937 rng(std::random_device{}());

    int attempt = 0;

    while (true) {
        try {
            // Close any previously-open socket.
            asio::error_code ignore_ec;
            socket_.close(ignore_ec);
            socket_ = asio::local::stream_protocol::socket(ioc_);

            asio::local::stream_protocol::endpoint ep(uds_path_);
            auto [ec] = co_await socket_.async_connect(ep, use_nothrow);
            if (ec) {
                spdlog::warn("UDS connect to '{}' failed: {}", uds_path_, ec.message());
                goto retry;
            }

            spdlog::info("UDS connected to '{}'", uds_path_);

            if (!co_await do_handshake()) {
                spdlog::error("UDS handshake failed");
                socket_.close(ignore_ec);
                goto retry;
            }

            connected_ = true;
            attempt = 0;
            spdlog::info("UDS handshake complete -- relay connected to node");

            // Spawn read loop and cleanup loop
            {
                auto self_ptr = this;
                asio::co_spawn(ioc_,
                    [self_ptr]() -> asio::awaitable<void> {
                        co_await self_ptr->read_loop();
                    }, asio::detached);
                asio::co_spawn(ioc_,
                    [self_ptr]() -> asio::awaitable<void> {
                        co_await self_ptr->cleanup_loop();
                    }, asio::detached);
            }
            co_return;

        } catch (const std::exception& e) {
            spdlog::error("UDS connect exception: {}", e.what());
        }

retry:
        // Jittered exponential backoff: delay = min(1s * 2^attempt, 30s)
        double base_delay = std::min(1.0 * (1 << std::min(attempt, 15)), 30.0);
        std::uniform_real_distribution<double> dist(0.5, 1.0);
        double jittered = base_delay * dist(rng);

        spdlog::info("UDS reconnecting in {:.1f}s (attempt {})", jittered, attempt + 1);

        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::milliseconds(
            static_cast<int64_t>(jittered * 1000)));
        co_await timer.async_wait(asio::use_awaitable);

        ++attempt;
    }
}

// ---------------------------------------------------------------------------
// TrustedHello + HKDF + Auth handshake
// ---------------------------------------------------------------------------

asio::awaitable<bool> UdsMultiplexer::do_handshake() {
    // Step 1: Generate 32-byte nonce
    std::array<uint8_t, 32> nonce_i{};
    if (RAND_bytes(nonce_i.data(), 32) != 1) {
        spdlog::error("handshake: RAND_bytes failed");
        co_return false;
    }

    // Step 2: Build TrustedHello payload: [nonce:32][signing_pubkey:2592]
    auto signing_pk = identity_.public_key();
    std::vector<uint8_t> hello_payload;
    hello_payload.reserve(32 + signing_pk.size());
    hello_payload.insert(hello_payload.end(), nonce_i.begin(), nonce_i.end());
    hello_payload.insert(hello_payload.end(), signing_pk.begin(), signing_pk.end());

    // Step 3: Encode with TransportCodec
    auto hello_msg = wire::TransportCodec::encode(TransportMsgType_TrustedHello, hello_payload);

    // Step 4: send_raw
    if (!co_await send_raw(hello_msg)) {
        spdlog::warn("handshake: failed to send TrustedHello");
        co_return false;
    }

    // Step 5: recv_raw
    auto response = co_await recv_raw();
    if (!response) {
        spdlog::warn("handshake: failed to receive TrustedHello response");
        co_return false;
    }

    // Step 6: Decode response
    auto decoded = wire::TransportCodec::decode(*response);
    if (!decoded) {
        spdlog::warn("handshake: invalid TrustedHello response");
        co_return false;
    }

    if (decoded->type != TransportMsgType_TrustedHello) {
        spdlog::warn("handshake: unexpected response type {}", static_cast<int>(decoded->type));
        co_return false;
    }

    // Step 7: Parse response nonce + signing_pk
    constexpr size_t NONCE_SIZE = 32;
    size_t expected_size = NONCE_SIZE + SIGNING_PK_SIZE;
    if (decoded->payload.size() != expected_size) {
        spdlog::warn("handshake: invalid TrustedHello payload size (got {}, expected {})",
                     decoded->payload.size(), expected_size);
        co_return false;
    }

    auto nonce_r = std::span<const uint8_t>(decoded->payload.data(), NONCE_SIZE);
    auto resp_signing_pk = std::span<const uint8_t>(
        decoded->payload.data() + NONCE_SIZE, SIGNING_PK_SIZE);

    // Step 8: Derive session keys via HKDF-SHA256
    // IKM = nonce_i || nonce_r (64 bytes)
    std::vector<uint8_t> ikm;
    ikm.reserve(64);
    ikm.insert(ikm.end(), nonce_i.begin(), nonce_i.end());
    ikm.insert(ikm.end(), nonce_r.begin(), nonce_r.end());

    // Salt = signing_pk_i || signing_pk_r (5184 bytes) -- relay is initiator
    std::vector<uint8_t> salt;
    salt.reserve(signing_pk.size() + SIGNING_PK_SIZE);
    salt.insert(salt.end(), signing_pk.begin(), signing_pk.end());
    salt.insert(salt.end(), resp_signing_pk.begin(), resp_signing_pk.end());

    auto prk = wire::hkdf_extract(salt, ikm);
    auto init_to_resp_key = wire::hkdf_expand(prk, "chromatin-init-to-resp-v1", 32);
    auto resp_to_init_key = wire::hkdf_expand(prk, "chromatin-resp-to-init-v1", 32);

    // Session fingerprint: SHA3-256(IKM || Salt)
    std::vector<uint8_t> fp_input;
    fp_input.reserve(ikm.size() + salt.size());
    fp_input.insert(fp_input.end(), ikm.begin(), ikm.end());
    fp_input.insert(fp_input.end(), salt.begin(), salt.end());

    std::array<uint8_t, 32> session_fingerprint{};
    OQS_SHA3_sha3_256(session_fingerprint.data(), fp_input.data(), fp_input.size());

    // Step 9: Set keys -- relay is initiator
    send_key_.assign(init_to_resp_key.begin(), init_to_resp_key.end());
    recv_key_.assign(resp_to_init_key.begin(), resp_to_init_key.end());
    send_counter_ = 0;
    recv_counter_ = 0;

    // Step 10: Auth exchange (encrypted, consumes counter 0)
    // Sign session fingerprint
    auto sig = identity_.sign(session_fingerprint);

    // Build auth payload: [pubkey_size:4B LE][pubkey:2592][signature:4627]
    // CRITICAL: pubkey_size is LITTLE-endian (protocol exception per project memory)
    std::vector<uint8_t> auth_payload;
    auth_payload.reserve(4 + signing_pk.size() + sig.size());
    uint32_t pk_size = static_cast<uint32_t>(signing_pk.size());
    auth_payload.push_back(static_cast<uint8_t>(pk_size & 0xFF));
    auth_payload.push_back(static_cast<uint8_t>((pk_size >> 8) & 0xFF));
    auth_payload.push_back(static_cast<uint8_t>((pk_size >> 16) & 0xFF));
    auth_payload.push_back(static_cast<uint8_t>((pk_size >> 24) & 0xFF));
    auth_payload.insert(auth_payload.end(), signing_pk.begin(), signing_pk.end());
    auth_payload.insert(auth_payload.end(), sig.begin(), sig.end());

    // Encode and send encrypted (uses counter 0)
    auto auth_msg = wire::TransportCodec::encode(TransportMsgType_AuthSignature, auth_payload);
    if (!co_await send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth");
        co_return false;
    }

    // Receive responder's auth (uses counter 0)
    auto resp_auth_raw = co_await recv_encrypted();
    if (!resp_auth_raw) {
        spdlog::warn("handshake: failed to receive peer auth");
        co_return false;
    }

    auto resp_transport = wire::TransportCodec::decode(*resp_auth_raw);
    if (!resp_transport || resp_transport->type != TransportMsgType_AuthSignature) {
        spdlog::warn("handshake: invalid auth message from node");
        co_return false;
    }

    // Parse auth payload: [pubkey_size:4B LE][pubkey:2592][signature:4627]
    if (resp_transport->payload.size() < 4) {
        spdlog::warn("handshake: auth payload too short");
        co_return false;
    }
    uint32_t resp_pk_size =
        static_cast<uint32_t>(resp_transport->payload[0]) |
        (static_cast<uint32_t>(resp_transport->payload[1]) << 8) |
        (static_cast<uint32_t>(resp_transport->payload[2]) << 16) |
        (static_cast<uint32_t>(resp_transport->payload[3]) << 24);

    if (resp_pk_size != SIGNING_PK_SIZE) {
        spdlog::warn("handshake: unexpected pubkey size {} in auth", resp_pk_size);
        co_return false;
    }
    if (resp_transport->payload.size() < 4 + resp_pk_size) {
        spdlog::warn("handshake: auth payload truncated");
        co_return false;
    }

    auto resp_auth_pk = std::span<const uint8_t>(
        resp_transport->payload.data() + 4, resp_pk_size);
    auto resp_auth_sig = std::span<const uint8_t>(
        resp_transport->payload.data() + 4 + resp_pk_size,
        resp_transport->payload.size() - 4 - resp_pk_size);

    // Verify pubkey matches TrustedHello (prevents MitM substitution)
    if (resp_auth_pk.size() != resp_signing_pk.size() ||
        std::memcmp(resp_auth_pk.data(), resp_signing_pk.data(), resp_signing_pk.size()) != 0) {
        spdlog::warn("handshake: auth pubkey mismatch");
        co_return false;
    }

    // Verify signature on session fingerprint using OQS_SIG_verify (ML-DSA-87)
    OQS_SIG* verifier = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!verifier) {
        spdlog::error("handshake: failed to create ML-DSA-87 verifier");
        co_return false;
    }

    OQS_STATUS verify_result = OQS_SIG_verify(
        verifier,
        session_fingerprint.data(), session_fingerprint.size(),
        resp_auth_sig.data(), resp_auth_sig.size(),
        resp_auth_pk.data());
    OQS_SIG_free(verifier);

    if (verify_result != OQS_SUCCESS) {
        spdlog::warn("handshake: peer auth signature invalid");
        co_return false;
    }

    // Step 11: Post-handshake -- counters are now both 1 (auth consumed 0)
    spdlog::debug("handshake: auth verified, send_counter={} recv_counter={}",
                  send_counter_, recv_counter_);
    co_return true;
}

// ---------------------------------------------------------------------------
// Raw framing: [4B BE length][data]
// ---------------------------------------------------------------------------

asio::awaitable<bool> UdsMultiplexer::send_raw(std::span<const uint8_t> data) {
    uint8_t header[4];
    util::store_u32_be(header, static_cast<uint32_t>(data.size()));

    auto [ec1, n1] = co_await asio::async_write(
        socket_, asio::buffer(header, 4), use_nothrow);
    if (ec1 || n1 != 4) co_return false;

    auto [ec2, n2] = co_await asio::async_write(
        socket_, asio::buffer(data.data(), data.size()), use_nothrow);
    if (ec2 || n2 != data.size()) co_return false;

    co_return true;
}

asio::awaitable<std::optional<std::vector<uint8_t>>> UdsMultiplexer::recv_raw() {
    uint8_t header[4];
    auto [ec1, n1] = co_await asio::async_read(
        socket_, asio::buffer(header, 4), use_nothrow);
    if (ec1 || n1 != 4) co_return std::nullopt;

    uint32_t length = util::read_u32_be(header);
    if (length > MAX_FRAME_SIZE) {
        spdlog::error("UDS frame too large: {} bytes", length);
        co_return std::nullopt;
    }

    std::vector<uint8_t> buf(length);
    auto [ec2, n2] = co_await asio::async_read(
        socket_, asio::buffer(buf.data(), length), use_nothrow);
    if (ec2 || n2 != length) co_return std::nullopt;

    co_return buf;
}

// ---------------------------------------------------------------------------
// Encrypted framing: AEAD encrypt/decrypt then raw send/recv
// ---------------------------------------------------------------------------

asio::awaitable<bool> UdsMultiplexer::send_encrypted(std::span<const uint8_t> plaintext) {
    auto ct = wire::aead_encrypt(plaintext, send_key_, send_counter_++);
    co_return co_await send_raw(ct);
}

asio::awaitable<std::optional<std::vector<uint8_t>>> UdsMultiplexer::recv_encrypted() {
    auto raw = co_await recv_raw();
    if (!raw) co_return std::nullopt;

    auto pt = wire::aead_decrypt(*raw, recv_key_, recv_counter_++);
    if (!pt) {
        spdlog::error("UDS AEAD decrypt failed at recv_counter={}", recv_counter_ - 1);
        co_return std::nullopt;
    }
    co_return pt;
}

// ---------------------------------------------------------------------------
// Read loop: receive + decode + route
// ---------------------------------------------------------------------------

asio::awaitable<void> UdsMultiplexer::read_loop() {
    while (connected_) {
        auto msg = co_await recv_encrypted();
        if (!msg) {
            spdlog::warn("UDS read_loop: recv failed -- disconnecting");
            connected_ = false;

            // Close socket and restart connection.
            asio::error_code ec;
            socket_.close(ec);

            auto self_ptr = this;
            asio::co_spawn(ioc_,
                [self_ptr]() -> asio::awaitable<void> {
                    co_await self_ptr->connect_loop();
                }, asio::detached);
            co_return;
        }

        auto decoded = wire::TransportCodec::decode(*msg);
        if (!decoded) {
            spdlog::warn("UDS: invalid TransportMessage from node");
            continue;
        }

        route_response(static_cast<uint8_t>(decoded->type),
                       std::move(decoded->payload),
                       decoded->request_id);
    }
}

// ---------------------------------------------------------------------------
// Response routing
// ---------------------------------------------------------------------------

void UdsMultiplexer::route_response(uint8_t type, std::vector<uint8_t> payload,
                                     uint32_t request_id) {
    if (request_id == 0) {
        // Server-initiated messages (Phase 104)
        if (type == 21) {
            // Notification: fan-out to subscribed sessions (D-06)
            handle_notification(type, payload);
            return;
        }

        if (type == 22 || type == 25) {
            // StorageFull(22) / QuotaExceeded(25): broadcast to ALL sessions (D-08)
            auto json_opt = translate::binary_to_json(type, payload);
            if (!json_opt) {
                spdlog::warn("UDS: binary_to_json failed for broadcast type={}", type);
                return;
            }
            spdlog::debug("UDS: broadcasting type={} to all sessions", type);
            sessions_.for_each([&json_opt](uint64_t /*id*/, const auto& session) {
                if (session) {
                    session->send_json(*json_opt);
                }
            });
            return;
        }

        auto type_name = translate::type_to_string(type);
        spdlog::debug("UDS: unhandled server-initiated message type={} ({})",
                      type, type_name.value_or("unknown"));
        return;
    }

    // Look up which client this response belongs to.
    auto pending = router_.resolve_response(request_id);
    if (!pending) {
        spdlog::debug("UDS: no pending request for relay_rid={}", request_id);
        return;
    }

    auto session = sessions_.get_session(pending->client_session_id);
    if (!session) {
        spdlog::debug("UDS: session {} gone for relay_rid={}",
                      pending->client_session_id, request_id);
        return;
    }

    // Translate binary -> JSON
    auto json_opt = translate::binary_to_json(type, payload);
    if (!json_opt) {
        spdlog::warn("UDS: binary_to_json failed for type={}", type);
        return;
    }

    // Restore client's original request_id
    if (pending->client_request_id != 0) {
        (*json_opt)["request_id"] = pending->client_request_id;
    }

    // Send to client: binary WS frame for ReadResponse/BatchReadResponse,
    // text WS frame for everything else.
    if (translate::is_binary_response(type)) {
        session->send_binary(json_opt->dump());
    } else {
        session->send_json(*json_opt);
    }
}

// ---------------------------------------------------------------------------
// Notification fan-out (Phase 104 D-06)
// ---------------------------------------------------------------------------

void UdsMultiplexer::handle_notification(uint8_t type, std::span<const uint8_t> payload) {
    if (payload.size() < 32) return;  // Need at least namespace_id

    // Extract namespace from first 32 bytes
    std::array<uint8_t, 32> ns{};
    std::memcpy(ns.data(), payload.data(), 32);

    if (!tracker_) return;

    // Get subscribed sessions
    auto session_ids = tracker_->get_subscribers(ns);
    if (session_ids.empty()) return;

    // Translate ONCE (D-06 optimization)
    auto json_opt = translate::binary_to_json(type, payload);
    if (!json_opt) return;

    spdlog::debug("notification: fan-out to {} subscribers for namespace {}",
                  session_ids.size(), util::to_hex(ns));

    // Fan-out to all subscribers (D-07: text frames via send_json)
    for (uint64_t sid : session_ids) {
        auto session = sessions_.get_session(sid);
        if (session) {
            session->send_json(*json_opt);
        }
    }
}

// ---------------------------------------------------------------------------
// Stale request cleanup
// ---------------------------------------------------------------------------

asio::awaitable<void> UdsMultiplexer::cleanup_loop() {
    while (connected_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(use_nothrow);
        if (ec || !connected_) break;

        auto purged = router_.purge_stale();
        if (purged > 0) {
            spdlog::debug("UDS cleanup: purged {} stale requests", purged);
        }
    }
}

} // namespace chromatindb::relay::core
