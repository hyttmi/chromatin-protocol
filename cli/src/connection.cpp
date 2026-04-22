#include "cli/src/connection.h"
#include "cli/src/pipeline_pump.h"
#include "cli/src/wire.h"

#include <oqs/oqs.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>

namespace chromatindb::cli {

// =============================================================================
// Handshake-only message types (not in MsgType enum — only used here)
// =============================================================================

static constexpr uint8_t TYPE_KEM_PUBKEY     = 1;
static constexpr uint8_t TYPE_KEM_CIPHERTEXT = 2;
static constexpr uint8_t TYPE_AUTH_SIGNATURE  = 3;
static constexpr uint8_t TYPE_TRUSTED_HELLO   = 23;

// =============================================================================
// Connection role (wire value, must match db/net/role.h)
// cdb is always a CLIENT initiator; it doesn't act on the remote's declared
// role, so only the value we send is named here.
// =============================================================================

static constexpr uint8_t ROLE_CLIENT = 0x01;

// =============================================================================
// Constants
// =============================================================================

static constexpr uint32_t MAX_FRAME_SIZE = 110 * 1024 * 1024;  // 110 MiB
static constexpr size_t STREAMING_THRESHOLD = 1048576;  // 1 MiB plaintext sub-frame size
static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD,
    "MAX_FRAME_SIZE must admit one full streaming sub-frame plus headroom "
    "for AEAD tag (16B), length prefix (4B), and transport envelope. "
    "Shrinking either constant without re-checking the other breaks the "
    "invariant.");
static constexpr size_t SIGNING_PK_SIZE = Identity::SIGNING_PK_SIZE;  // 2592
static constexpr size_t KEM_CT_SIZE     = 1568;
static constexpr size_t NONCE_SIZE      = 32;

// PIPE-03 / D-07: pipeline depth now lives on Connection::kPipelineDepth
// (cli/src/connection.h). That single definition is shared by send_async
// below and by the per-batch rid_to_index maps in cmd::get / cmd::put.

// =============================================================================
// SHA3-256 helper
// =============================================================================



// =============================================================================
// Auth payload encode/decode (matching db/net/auth_helpers.h)
// =============================================================================

static std::vector<uint8_t> encode_auth_payload(
    uint8_t role,
    std::span<const uint8_t> signing_pubkey,
    std::span<const uint8_t> signature) {

    std::vector<uint8_t> payload;
    payload.reserve(1 + 4 + signing_pubkey.size() + signature.size());

    // Role byte (cdb always sends ROLE_CLIENT)
    payload.push_back(role);

    // 4-byte BE pubkey size
    uint8_t pk_size_be[4];
    store_u32_be(pk_size_be, static_cast<uint32_t>(signing_pubkey.size()));
    payload.insert(payload.end(), pk_size_be, pk_size_be + 4);

    payload.insert(payload.end(), signing_pubkey.begin(), signing_pubkey.end());
    payload.insert(payload.end(), signature.begin(), signature.end());

    return payload;
}

struct AuthPayload {
    uint8_t role;
    std::vector<uint8_t> pubkey;
    std::vector<uint8_t> signature;
};

static std::optional<AuthPayload> decode_auth_payload(std::span<const uint8_t> data) {
    // Need at least role(1) + pk_size(4)
    if (data.size() < 5) return std::nullopt;

    // cdb doesn't route on the remote's declared role -- it trusts whatever
    // the node reports and relies on the signature check for integrity.
    uint8_t role = data[0];

    uint32_t pk_size = load_u32_be(data.data() + 1);
    if (pk_size != SIGNING_PK_SIZE) return std::nullopt;
    if (pk_size > data.size() - 5) return std::nullopt;

    AuthPayload result;
    result.role = role;
    result.pubkey.assign(data.begin() + 5, data.begin() + 5 + pk_size);
    result.signature.assign(data.begin() + 5 + pk_size, data.end());
    return result;
}

// =============================================================================
// ML-DSA-87 verify helper
// =============================================================================

static bool verify_signature(std::span<const uint8_t> message,
                             std::span<const uint8_t> signature,
                             std::span<const uint8_t> pubkey) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) return false;

    OQS_STATUS rc = OQS_SIG_verify(sig, message.data(), message.size(),
                                    signature.data(), signature.size(),
                                    pubkey.data());
    OQS_SIG_free(sig);
    return rc == OQS_SUCCESS;
}

// =============================================================================
// Connection
// =============================================================================

Connection::Connection(Identity& identity)
    : identity_(identity) {}

// =============================================================================
// connect()
// =============================================================================

bool Connection::connect(const std::string& host, uint16_t port,
                         const std::string& uds_path) {
    // Try UDS first if socket file exists. Use the error_code overload so a
    // permission-denied stat (e.g. /run/chromatindb/node.sock owned by the
    // chromatindb group) returns false instead of throwing -- we want to
    // fall through to TCP, not abort the whole command.
    std::error_code ec;
    if (!uds_path.empty() && std::filesystem::exists(uds_path, ec) && !ec) {
        if (connect_uds(uds_path)) {
            spdlog::debug("connected via UDS: {}", uds_path);
            if (handshake_trusted()) {
                connected_ = true;
                return true;
            }
            close();
        }
    }

    // Fall back to TCP with full PQ handshake
    if (connect_tcp(host, port)) {
        spdlog::debug("connected via TCP: {}:{}", host, port);
        if (handshake_pq()) {
            connected_ = true;
            return true;
        }
        close();
    }

    return false;
}

// =============================================================================
// Socket connect
// =============================================================================

bool Connection::connect_uds(const std::string& uds_path) {
    try {
        uds_socket_.emplace(ioc_);
        asio::local::stream_protocol::endpoint ep(uds_path);
        uds_socket_->connect(ep);
        is_uds_ = true;
        return true;
    } catch (const std::exception& e) {
        spdlog::debug("UDS connect failed: {}", e.what());
        uds_socket_.reset();
        is_uds_ = false;
        return false;
    }
}

bool Connection::connect_tcp(const std::string& host, uint16_t port) {
    // Bounded connect: race an async_connect against a 5-second timer so a
    // dead/unreachable host fails fast with a clear error instead of blocking
    // on the kernel's default TCP SYN-retry ladder (30+ seconds).
    static constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(5);

    try {
        tcp_socket_.emplace(ioc_);
        asio::ip::tcp::resolver resolver(ioc_);
        auto endpoints = resolver.resolve(host, std::to_string(port));

        ioc_.restart();
        std::error_code connect_ec = asio::error::would_block;
        asio::steady_timer timer(ioc_);
        timer.expires_after(CONNECT_TIMEOUT);
        timer.async_wait([&](const std::error_code& ec) {
            if (!ec && connect_ec == asio::error::would_block) {
                // Timer fired before connect completed — cancel the socket.
                std::error_code ignore;
                tcp_socket_->close(ignore);
            }
        });
        asio::async_connect(*tcp_socket_, endpoints,
            [&](const std::error_code& ec, const asio::ip::tcp::endpoint&) {
                connect_ec = ec;
                timer.cancel();
            });

        ioc_.run();

        if (connect_ec) {
            if (connect_ec == asio::error::operation_aborted ||
                connect_ec == asio::error::timed_out) {
                spdlog::debug("TCP connect timeout: {}:{}", host, port);
            } else {
                spdlog::debug("TCP connect failed: {}", connect_ec.message());
            }
            tcp_socket_.reset();
            return false;
        }

        is_uds_ = false;
        return true;
    } catch (const std::exception& e) {
        spdlog::debug("TCP connect failed: {}", e.what());
        tcp_socket_.reset();
        return false;
    }
}

// =============================================================================
// Raw I/O helpers
// =============================================================================

bool Connection::write_bytes(const uint8_t* data, size_t len) {
    try {
        if (is_uds_ && uds_socket_) {
            asio::write(*uds_socket_, asio::buffer(data, len));
        } else if (tcp_socket_) {
            asio::write(*tcp_socket_, asio::buffer(data, len));
        } else {
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Connection::read_bytes(uint8_t* buf, size_t len) {
    try {
        if (is_uds_ && uds_socket_) {
            asio::read(*uds_socket_, asio::buffer(buf, len));
        } else if (tcp_socket_) {
            asio::read(*tcp_socket_, asio::buffer(buf, len));
        } else {
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Connection::send_raw(std::span<const uint8_t> data) {
    // Wire format: [4-byte BE length][data]
    uint8_t header[4];
    store_u32_be(header, static_cast<uint32_t>(data.size()));

    if (!write_bytes(header, 4)) return false;
    if (!data.empty() && !write_bytes(data.data(), data.size())) return false;
    return true;
}

std::optional<std::vector<uint8_t>> Connection::recv_raw() {
    // Read 4-byte BE length prefix
    uint8_t header[4];
    if (!read_bytes(header, 4)) return std::nullopt;

    uint32_t len = load_u32_be(header);
    if (len > MAX_FRAME_SIZE) {
        spdlog::warn("frame exceeds max size: {}", len);
        return std::nullopt;
    }

    std::vector<uint8_t> payload(len);
    if (len > 0 && !read_bytes(payload.data(), len)) return std::nullopt;
    return payload;
}

// =============================================================================
// Encrypted I/O
// =============================================================================

bool Connection::send_encrypted(std::span<const uint8_t> plaintext) {
    auto key_span = std::span<const uint8_t, 32>(send_key_.data(), 32);
    auto ct = encrypt_frame(plaintext, key_span, send_counter_);
    send_counter_++;
    return send_raw(ct);
}

std::optional<std::vector<uint8_t>> Connection::recv_encrypted() {
    auto raw = recv_raw();
    if (!raw) return std::nullopt;

    auto key_span = std::span<const uint8_t, 32>(recv_key_.data(), 32);
    auto pt = decrypt_frame(*raw, key_span, recv_counter_);
    if (!pt) {
        spdlog::warn("AEAD decrypt failed (counter={})", recv_counter_);
        return std::nullopt;
    }
    recv_counter_++;
    return pt;
}

// =============================================================================
// PQ Handshake (TCP) — initiator side
// =============================================================================

bool Connection::handshake_pq() {
    // Step 1: Generate ephemeral ML-KEM-1024 keypair
    OQS_KEM* kem = OQS_KEM_new(OQS_KEM_alg_ml_kem_1024);
    if (!kem) {
        spdlog::error("failed to create ML-KEM-1024 context");
        return false;
    }

    std::vector<uint8_t> kem_pk(kem->length_public_key);
    std::vector<uint8_t> kem_sk(kem->length_secret_key);
    if (OQS_KEM_keypair(kem, kem_pk.data(), kem_sk.data()) != OQS_SUCCESS) {
        OQS_KEM_free(kem);
        spdlog::error("ML-KEM-1024 keypair generation failed");
        return false;
    }

    // Step 2: Send KemPubkey (type=1): payload = [kem_pk:1568][signing_pk:2592]
    auto signing_pk = identity_.signing_pubkey();
    std::vector<uint8_t> kem_payload;
    kem_payload.reserve(kem_pk.size() + signing_pk.size());
    kem_payload.insert(kem_payload.end(), kem_pk.begin(), kem_pk.end());
    kem_payload.insert(kem_payload.end(), signing_pk.begin(), signing_pk.end());

    auto kem_msg = encode_transport(TYPE_KEM_PUBKEY, kem_payload, 0);
    if (!send_raw(kem_msg)) {
        OQS_KEM_free(kem);
        spdlog::warn("handshake: failed to send KemPubkey");
        return false;
    }

    // Step 3: Receive KemCiphertext (type=2): payload = [ct:1568][signing_pk:2592]
    auto resp_raw = recv_raw();
    if (!resp_raw) {
        OQS_KEM_free(kem);
        spdlog::warn("handshake: failed to receive KemCiphertext");
        return false;
    }

    auto resp = decode_transport(*resp_raw);
    if (!resp || resp->type != TYPE_KEM_CIPHERTEXT) {
        OQS_KEM_free(kem);
        spdlog::warn("handshake: unexpected response type {}", resp ? resp->type : 0);
        return false;
    }

    size_t expected_ct_size = KEM_CT_SIZE + SIGNING_PK_SIZE;
    if (resp->payload.size() != expected_ct_size) {
        OQS_KEM_free(kem);
        spdlog::warn("handshake: invalid KemCiphertext payload size: {}",
                      resp->payload.size());
        return false;
    }

    auto ct_span = std::span<const uint8_t>(resp->payload.data(), KEM_CT_SIZE);
    auto resp_signing_pk = std::span<const uint8_t>(
        resp->payload.data() + KEM_CT_SIZE, SIGNING_PK_SIZE);

    // Step 4: Decapsulate to get shared secret
    std::vector<uint8_t> shared_secret(kem->length_shared_secret);
    if (OQS_KEM_decaps(kem, shared_secret.data(), ct_span.data(), kem_sk.data()) != OQS_SUCCESS) {
        OQS_KEM_free(kem);
        spdlog::error("KEM decapsulation failed");
        return false;
    }
    OQS_KEM_free(kem);

    // Step 5: HKDF-SHA256 key derivation (empty salt)
    uint8_t prk[32];
    crypto_kdf_hkdf_sha256_extract(prk, nullptr, 0,
                                    shared_secret.data(), shared_secret.size());

    uint8_t init_to_resp[32];
    crypto_kdf_hkdf_sha256_expand(init_to_resp, 32,
                                   "chromatin-init-to-resp-v1", 25, prk);

    uint8_t resp_to_init[32];
    crypto_kdf_hkdf_sha256_expand(resp_to_init, 32,
                                   "chromatin-resp-to-init-v1", 25, prk);

    // As initiator: send = init_to_resp, recv = resp_to_init
    send_key_.assign(init_to_resp, init_to_resp + 32);
    recv_key_.assign(resp_to_init, resp_to_init + 32);

    // Step 6: Session fingerprint = SHA3-256(shared_secret || initiator_signing_pk || responder_signing_pk)
    std::vector<uint8_t> fp_input;
    fp_input.reserve(shared_secret.size() + signing_pk.size() + resp_signing_pk.size());
    fp_input.insert(fp_input.end(), shared_secret.begin(), shared_secret.end());
    fp_input.insert(fp_input.end(), signing_pk.begin(), signing_pk.end());
    fp_input.insert(fp_input.end(), resp_signing_pk.begin(), resp_signing_pk.end());
    auto fingerprint = sha3_256(fp_input);

    // Securely erase ephemeral secrets
    sodium_memzero(shared_secret.data(), shared_secret.size());
    sodium_memzero(kem_sk.data(), kem_sk.size());
    sodium_memzero(prk, sizeof(prk));

    // Step 7: Auth exchange (encrypted — counters start at 0)

    // 7a: Send our AuthSignature (type=3)
    auto sig = identity_.sign(fingerprint);
    auto auth_payload = encode_auth_payload(ROLE_CLIENT, signing_pk, sig);
    auto auth_msg = encode_transport(TYPE_AUTH_SIGNATURE, auth_payload, 0);
    if (!send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth");
        return false;
    }

    // 7b: Receive responder's AuthSignature
    auto resp_auth_pt = recv_encrypted();
    if (!resp_auth_pt) {
        spdlog::warn("handshake: failed to receive peer auth");
        return false;
    }

    auto resp_auth = decode_transport(*resp_auth_pt);
    if (!resp_auth || resp_auth->type != TYPE_AUTH_SIGNATURE) {
        spdlog::warn("handshake: invalid auth response");
        return false;
    }

    auto auth = decode_auth_payload(resp_auth->payload);
    if (!auth) {
        spdlog::warn("handshake: malformed auth payload");
        return false;
    }

    // Verify responder's signature over session fingerprint
    if (!verify_signature(fingerprint, auth->signature, auth->pubkey)) {
        spdlog::warn("handshake: peer auth signature invalid");
        return false;
    }

    spdlog::info("PQ handshake complete (send_counter={}, recv_counter={})",
                 send_counter_, recv_counter_);
    return true;
}

// =============================================================================
// TrustedHello Handshake (UDS) — initiator side
// =============================================================================

bool Connection::handshake_trusted() {
    // Step 1: Generate 32-byte random nonce
    std::array<uint8_t, 32> nonce_i{};
    randombytes_buf(nonce_i.data(), nonce_i.size());

    // Step 2: Send TrustedHello (type=23): payload = [nonce:32][signing_pk:2592]
    auto signing_pk = identity_.signing_pubkey();
    std::vector<uint8_t> hello_payload;
    hello_payload.reserve(NONCE_SIZE + signing_pk.size());
    hello_payload.insert(hello_payload.end(), nonce_i.begin(), nonce_i.end());
    hello_payload.insert(hello_payload.end(), signing_pk.begin(), signing_pk.end());

    auto hello_msg = encode_transport(TYPE_TRUSTED_HELLO, hello_payload, 0);
    if (!send_raw(hello_msg)) {
        spdlog::warn("handshake: failed to send TrustedHello");
        return false;
    }

    // Step 3: Receive response
    auto resp_raw = recv_raw();
    if (!resp_raw) {
        spdlog::warn("handshake: failed to receive TrustedHello response");
        return false;
    }

    auto resp = decode_transport(*resp_raw);
    if (!resp) {
        spdlog::warn("handshake: invalid TrustedHello response");
        return false;
    }

    if (resp->type != TYPE_TRUSTED_HELLO) {
        spdlog::warn("handshake: unexpected response type {} to TrustedHello",
                      resp->type);
        return false;
    }

    // Parse responder's TrustedHello: [nonce:32][signing_pk:2592]
    size_t expected = NONCE_SIZE + SIGNING_PK_SIZE;
    if (resp->payload.size() != expected) {
        spdlog::warn("handshake: invalid TrustedHello payload size: {}",
                      resp->payload.size());
        return false;
    }

    auto nonce_r = std::span<const uint8_t>(resp->payload.data(), NONCE_SIZE);
    auto resp_signing_pk = std::span<const uint8_t>(
        resp->payload.data() + NONCE_SIZE, SIGNING_PK_SIZE);

    // Step 4: Derive session keys via HKDF-SHA256
    // IKM = our_nonce || their_nonce (64 bytes)
    std::vector<uint8_t> ikm;
    ikm.reserve(NONCE_SIZE * 2);
    ikm.insert(ikm.end(), nonce_i.begin(), nonce_i.end());
    ikm.insert(ikm.end(), nonce_r.begin(), nonce_r.end());

    // Salt = our_signing_pk || their_signing_pk (5184 bytes)
    std::vector<uint8_t> salt;
    salt.reserve(signing_pk.size() + resp_signing_pk.size());
    salt.insert(salt.end(), signing_pk.begin(), signing_pk.end());
    salt.insert(salt.end(), resp_signing_pk.begin(), resp_signing_pk.end());

    uint8_t prk[32];
    crypto_kdf_hkdf_sha256_extract(prk, salt.data(), salt.size(),
                                    ikm.data(), ikm.size());

    uint8_t init_to_resp[32];
    crypto_kdf_hkdf_sha256_expand(init_to_resp, 32,
                                   "chromatin-init-to-resp-v1", 25, prk);

    uint8_t resp_to_init[32];
    crypto_kdf_hkdf_sha256_expand(resp_to_init, 32,
                                   "chromatin-resp-to-init-v1", 25, prk);

    send_key_.assign(init_to_resp, init_to_resp + 32);
    recv_key_.assign(resp_to_init, resp_to_init + 32);

    // Step 5: Session fingerprint = SHA3-256(IKM || Salt)
    std::vector<uint8_t> fp_input;
    fp_input.reserve(ikm.size() + salt.size());
    fp_input.insert(fp_input.end(), ikm.begin(), ikm.end());
    fp_input.insert(fp_input.end(), salt.begin(), salt.end());
    auto fingerprint = sha3_256(fp_input);

    // Securely erase PRK
    sodium_memzero(prk, sizeof(prk));

    // Step 6: Auth exchange (encrypted — counters start at 0)
    // Initiator sends first (matching PQ path order)

    // 6a: Send our AuthSignature
    auto sig = identity_.sign(fingerprint);
    auto auth_payload = encode_auth_payload(ROLE_CLIENT, signing_pk, sig);
    auto auth_msg = encode_transport(TYPE_AUTH_SIGNATURE, auth_payload, 0);
    if (!send_encrypted(auth_msg)) {
        spdlog::warn("handshake: failed to send auth (trusted)");
        return false;
    }

    // 6b: Receive responder's AuthSignature
    auto resp_auth_pt = recv_encrypted();
    if (!resp_auth_pt) {
        spdlog::warn("handshake: failed to receive peer auth (trusted)");
        return false;
    }

    auto resp_auth = decode_transport(*resp_auth_pt);
    if (!resp_auth || resp_auth->type != TYPE_AUTH_SIGNATURE) {
        spdlog::warn("handshake: invalid auth response (trusted)");
        return false;
    }

    auto auth = decode_auth_payload(resp_auth->payload);
    if (!auth) {
        spdlog::warn("handshake: malformed auth payload (trusted)");
        return false;
    }

    // Verify pubkey matches TrustedHello (prevents MitM substitution)
    if (auth->pubkey.size() != resp_signing_pk.size() ||
        std::memcmp(auth->pubkey.data(), resp_signing_pk.data(),
                     resp_signing_pk.size()) != 0) {
        spdlog::warn("handshake: auth pubkey mismatch (trusted)");
        return false;
    }

    // Verify signature over session fingerprint
    if (!verify_signature(fingerprint, auth->signature, auth->pubkey)) {
        spdlog::warn("handshake: peer auth signature invalid (trusted)");
        return false;
    }

    spdlog::info("TrustedHello handshake complete (send_counter={}, recv_counter={})",
                 send_counter_, recv_counter_);
    return true;
}

// =============================================================================
// drain_announce() — absorb the SyncNamespaceAnnounce node sends after handshake
// =============================================================================

bool Connection::drain_announce() {
    auto pt = recv_encrypted();
    if (!pt) {
        spdlog::warn("drain_announce: failed to receive post-handshake message");
        return false;
    }

    auto msg = decode_transport(*pt);
    if (!msg) {
        spdlog::warn("drain_announce: failed to decode transport message");
        return false;
    }

    if (msg->type == static_cast<uint8_t>(MsgType::SyncNamespaceAnnounce)) {
        spdlog::debug("drain_announce: consumed SyncNamespaceAnnounce ({} bytes)",
                       msg->payload.size());
    } else {
        spdlog::warn("drain_announce: unexpected type {} (expected 62)",
                      msg->type);
    }

    return true;
}

// =============================================================================
// Public send/recv
// =============================================================================

bool Connection::send(MsgType type, std::span<const uint8_t> payload,
                      uint32_t request_id) {
    if (!connected_) return false;

    // Use chunked framing for large payloads (see file-scope STREAMING_THRESHOLD)
    if (payload.size() >= STREAMING_THRESHOLD) {
        return send_chunked(type, payload, request_id);
    }

    auto transport_bytes = encode_transport(static_cast<uint8_t>(type),
                                            payload, request_id);
    return send_encrypted(transport_bytes);
}

bool Connection::send_chunked(MsgType type, std::span<const uint8_t> payload,
                               uint32_t request_id) {
    static constexpr size_t CHUNK_SIZE = 1048576;  // 1 MiB

    // 1. Build and send chunked header: [0x01][type:1][request_id:4BE][total_size:8BE]
    std::vector<uint8_t> header(14);
    header[0] = 0x01;  // CHUNKED_BEGIN
    header[1] = static_cast<uint8_t>(type);
    store_u32_be(header.data() + 2, request_id);
    store_u64_be(header.data() + 6, payload.size());

    if (!send_encrypted(header)) return false;

    // 2. Send data sub-frames (1 MiB each)
    size_t offset = 0;
    while (offset < payload.size()) {
        size_t chunk_len = std::min(CHUNK_SIZE, payload.size() - offset);
        auto chunk = payload.subspan(offset, chunk_len);
        if (!send_encrypted(chunk)) return false;
        offset += chunk_len;
    }

    // 3. Send zero-length sentinel
    if (!send_encrypted(std::span<const uint8_t>{})) return false;

    return true;
}

std::optional<DecodedTransport> Connection::recv() {
    if (!connected_) return std::nullopt;

    auto pt = recv_encrypted();
    if (!pt) return std::nullopt;

    // Check for chunked framing (first byte == 0x01)
    if (!pt->empty() && (*pt)[0] == 0x01 && pt->size() >= 14) {
        // Parse chunked header
        uint8_t type = (*pt)[1];
        uint32_t request_id = load_u32_be(pt->data() + 2);
        uint64_t total_size = load_u64_be(pt->data() + 6);

        // P-119-06 / T-119-06: clamp total_size before the
        // reserve(). Without this an attacker-forged chunked-framing header
        // (total_size = 2^64 - 1) would trigger a multi-EiB vector::reserve
        // and crash or OOM the client. MAX_FRAME_SIZE (110 MiB) is the same
        // cap applied to the single-frame path at line 282.
        if (total_size > MAX_FRAME_SIZE) {
            spdlog::warn(
                "recv: chunked total_size {} exceeds MAX_FRAME_SIZE ({}); aborting",
                total_size, MAX_FRAME_SIZE);
            return std::nullopt;
        }

        // Collect extra metadata from header (bytes after 14)
        std::vector<uint8_t> payload;
        payload.reserve(static_cast<size_t>(total_size));
        if (pt->size() > 14) {
            payload.insert(payload.end(), pt->begin() + 14, pt->end());
        }

        // Read data sub-frames until zero-length sentinel
        while (true) {
            auto chunk = recv_encrypted();
            if (!chunk) return std::nullopt;
            if (chunk->empty()) break;  // Sentinel
            payload.insert(payload.end(), chunk->begin(), chunk->end());
        }

        DecodedTransport msg;
        msg.type = type;
        msg.request_id = request_id;
        msg.payload = std::move(payload);
        return msg;
    }

    return decode_transport(*pt);
}

// =============================================================================
// send_async() / recv_for() — pipelining primitives
// =============================================================================

bool Connection::send_async(MsgType type, std::span<const uint8_t> payload,
                            uint32_t request_id) {
    if (!connected_) return false;

    // Backpressure (D-06): drain replies until a slot frees. The pump only
    // drives recv(), never touches the send path, so single-sender invariant
    // (D-09, PIPE-02) for AEAD send_counter_ is preserved by construction.
    while (in_flight_ >= Connection::kPipelineDepth) {
        if (!pipeline::pump_one_for_backpressure(
                [this] { return recv(); }, pending_replies_)) {
            return false;
        }
    }

    if (!send(type, payload, request_id)) return false;
    ++in_flight_;
    return true;
}

std::optional<DecodedTransport> Connection::recv_for(uint32_t request_id) {
    if (!connected_) return std::nullopt;

    return pipeline::pump_recv_for(
        request_id,
        [this] { return recv(); },
        pending_replies_,
        in_flight_);
}

std::optional<DecodedTransport> Connection::recv_next() {
    if (!connected_) return std::nullopt;

    return pipeline::pump_recv_any(
        [this] { return recv(); },
        pending_replies_,
        in_flight_);
}

// =============================================================================
// close()
// =============================================================================

void Connection::close() {
    connected_ = false;

    if (uds_socket_ && uds_socket_->is_open()) {
        asio::error_code ec;
        uds_socket_->shutdown(asio::local::stream_protocol::socket::shutdown_both, ec);
        uds_socket_->close(ec);
    }
    uds_socket_.reset();

    if (tcp_socket_ && tcp_socket_->is_open()) {
        asio::error_code ec;
        tcp_socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        tcp_socket_->close(ec);
    }
    tcp_socket_.reset();

    // Securely erase session keys
    if (!send_key_.empty()) sodium_memzero(send_key_.data(), send_key_.size());
    if (!recv_key_.empty()) sodium_memzero(recv_key_.data(), recv_key_.size());
    send_key_.clear();
    recv_key_.clear();
    send_counter_ = 0;
    recv_counter_ = 0;
    pending_replies_.clear();
    in_flight_ = 0;
}

} // namespace chromatindb::cli
