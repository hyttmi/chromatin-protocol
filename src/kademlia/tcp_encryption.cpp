#include "kademlia/tcp_encryption.h"

#include <algorithm>
#include <cstring>

#include <oqs/oqs.h>

#include "crypto/aead.h"
#include "crypto/kem.h"

namespace chromatin::kademlia::tcp_encryption {

// ---------------------------------------------------------------------------
// SessionKeys: secure zeroing
// ---------------------------------------------------------------------------

SessionKeys::~SessionKeys() {
    OQS_MEM_cleanse(send_key.data(), send_key.size());
    OQS_MEM_cleanse(recv_key.data(), recv_key.size());
}

SessionKeys::SessionKeys(SessionKeys&& other) noexcept
    : send_key(other.send_key)
    , recv_key(other.recv_key)
    , send_nonce(other.send_nonce)
    , recv_nonce(other.recv_nonce) {
    OQS_MEM_cleanse(other.send_key.data(), other.send_key.size());
    OQS_MEM_cleanse(other.recv_key.data(), other.recv_key.size());
}

SessionKeys& SessionKeys::operator=(SessionKeys&& other) noexcept {
    if (this != &other) {
        OQS_MEM_cleanse(send_key.data(), send_key.size());
        OQS_MEM_cleanse(recv_key.data(), recv_key.size());
        send_key = other.send_key;
        recv_key = other.recv_key;
        send_nonce = other.send_nonce;
        recv_nonce = other.recv_nonce;
        OQS_MEM_cleanse(other.send_key.data(), other.send_key.size());
        OQS_MEM_cleanse(other.recv_key.data(), other.recv_key.size());
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

void fill_random(uint8_t* buf, size_t len) {
    OQS_randombytes(buf, len);
}

// Derive directional keys from shared secret + randoms.
// i2r_key = SHA3-256("chromatin:tcp:i2r:" || ss || hello_random || accept_random)
// r2i_key = SHA3-256("chromatin:tcp:r2i:" || ss || hello_random || accept_random)
void derive_keys(std::span<const uint8_t> shared_secret,
                 const std::array<uint8_t, 32>& hello_random,
                 const std::array<uint8_t, 32>& accept_random,
                 std::array<uint8_t, 32>& i2r_key,
                 std::array<uint8_t, 32>& r2i_key) {
    // Build common suffix: ss || hello_random || accept_random
    std::vector<uint8_t> material;
    material.reserve(shared_secret.size() + 64);
    material.insert(material.end(), shared_secret.begin(), shared_secret.end());
    material.insert(material.end(), hello_random.begin(), hello_random.end());
    material.insert(material.end(), accept_random.begin(), accept_random.end());

    auto i2r = crypto::sha3_256_prefixed("chromatin:tcp:i2r:", material);
    auto r2i = crypto::sha3_256_prefixed("chromatin:tcp:r2i:", material);

    std::copy(i2r.begin(), i2r.end(), i2r_key.begin());
    std::copy(r2i.begin(), r2i.end(), r2i_key.begin());

    // Zero shared secret from the material buffer
    OQS_MEM_cleanse(material.data(), material.size());
}

// Build ACCEPT signature data:
// hello_random || initiator_node_id || responder_node_id || accept_random || kem_ciphertext
std::vector<uint8_t> build_accept_sig_data(
    const std::array<uint8_t, 32>& hello_random,
    const NodeId& initiator_id,
    const NodeId& responder_id,
    const std::array<uint8_t, 32>& accept_random,
    std::span<const uint8_t> kem_ciphertext) {
    std::vector<uint8_t> data;
    data.reserve(32 + 32 + 32 + 32 + kem_ciphertext.size());
    data.insert(data.end(), hello_random.begin(), hello_random.end());
    data.insert(data.end(), initiator_id.id.begin(), initiator_id.id.end());
    data.insert(data.end(), responder_id.id.begin(), responder_id.id.end());
    data.insert(data.end(), accept_random.begin(), accept_random.end());
    data.insert(data.end(), kem_ciphertext.begin(), kem_ciphertext.end());
    return data;
}

// Build CONFIRM signature data:
// hello_random || accept_random || kem_ciphertext || responder_node_id
std::vector<uint8_t> build_confirm_sig_data(
    const std::array<uint8_t, 32>& hello_random,
    const std::array<uint8_t, 32>& accept_random,
    std::span<const uint8_t> kem_ciphertext,
    const NodeId& responder_id) {
    std::vector<uint8_t> data;
    data.reserve(32 + 32 + kem_ciphertext.size() + 32);
    data.insert(data.end(), hello_random.begin(), hello_random.end());
    data.insert(data.end(), accept_random.begin(), accept_random.end());
    data.insert(data.end(), kem_ciphertext.begin(), kem_ciphertext.end());
    data.insert(data.end(), responder_id.id.begin(), responder_id.id.end());
    return data;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HandshakeInitiator
// ---------------------------------------------------------------------------

HandshakeInitiator::HandshakeInitiator(const NodeId& local_id)
    : local_id_(local_id) {}

std::vector<uint8_t> HandshakeInitiator::generate_hello(
    std::span<const uint8_t> signing_pubkey) {
    // Generate ephemeral KEM keypair
    auto kem_kp = crypto::generate_kem_keypair();
    kem_public_key_ = std::move(kem_kp.public_key);
    kem_secret_key_ = std::move(kem_kp.secret_key);

    // Generate random nonce
    fill_random(hello_random_.data(), hello_random_.size());

    // HELLO format:
    // [1B probe] [1B version] [1B cipher]
    // [32B initiator_node_id] [2B BE kem_pk_len] [kem_pk] [32B hello_random]
    // [2B BE sig_pk_len] [signing_pk]
    uint16_t pk_len = static_cast<uint16_t>(kem_public_key_.size());
    uint16_t sig_pk_len = static_cast<uint16_t>(signing_pubkey.size());

    std::vector<uint8_t> hello;
    hello.reserve(3 + 32 + 2 + kem_public_key_.size() + 32 + 2 + signing_pubkey.size());

    hello.push_back(PROBE_BYTE);
    hello.push_back(HANDSHAKE_VERSION);
    hello.push_back(CIPHER_CHACHA20_POLY1305);

    hello.insert(hello.end(), local_id_.id.begin(), local_id_.id.end());

    hello.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    hello.push_back(static_cast<uint8_t>(pk_len & 0xFF));

    hello.insert(hello.end(), kem_public_key_.begin(), kem_public_key_.end());

    hello.insert(hello.end(), hello_random_.begin(), hello_random_.end());

    hello.push_back(static_cast<uint8_t>((sig_pk_len >> 8) & 0xFF));
    hello.push_back(static_cast<uint8_t>(sig_pk_len & 0xFF));
    hello.insert(hello.end(), signing_pubkey.begin(), signing_pubkey.end());

    return hello;
}

std::optional<std::vector<uint8_t>> HandshakeInitiator::process_accept(
    std::span<const uint8_t> accept_bytes,
    std::span<const uint8_t> own_signing_key) {

    // ACCEPT format:
    // [1B version] [1B cipher] [32B responder_node_id]
    // [2B BE ct_len] [ct] [32B accept_random]
    // [2B BE sig_pk_len] [signing_pk]
    // [2B BE sig_len] [sig]
    size_t offset = 0;

    if (accept_bytes.size() < 2 + 32 + 2) return std::nullopt;

    uint8_t version = accept_bytes[offset++];
    uint8_t cipher = accept_bytes[offset++];
    if (version != HANDSHAKE_VERSION || cipher != CIPHER_CHACHA20_POLY1305)
        return std::nullopt;

    NodeId responder_id;
    std::copy_n(accept_bytes.data() + offset, 32, responder_id.id.begin());
    offset += 32;

    if (accept_bytes.size() < offset + 2) return std::nullopt;
    uint16_t ct_len = (static_cast<uint16_t>(accept_bytes[offset]) << 8) |
                       accept_bytes[offset + 1];
    offset += 2;

    if (accept_bytes.size() < offset + ct_len + 32 + 2) return std::nullopt;
    auto kem_ct = accept_bytes.subspan(offset, ct_len);
    offset += ct_len;

    std::array<uint8_t, 32> accept_random;
    std::copy_n(accept_bytes.data() + offset, 32, accept_random.begin());
    offset += 32;

    // Extract responder's signing pubkey
    if (accept_bytes.size() < offset + 2) return std::nullopt;
    uint16_t sig_pk_len = (static_cast<uint16_t>(accept_bytes[offset]) << 8) |
                           accept_bytes[offset + 1];
    offset += 2;

    if (accept_bytes.size() < offset + sig_pk_len + 2) return std::nullopt;
    auto responder_pubkey = accept_bytes.subspan(offset, sig_pk_len);
    offset += sig_pk_len;

    // Verify identity: SHA3-256(signing_pubkey) must equal responder's node ID
    auto expected_id = crypto::sha3_256(responder_pubkey);
    if (expected_id != responder_id.id) {
        return std::nullopt;
    }

    if (accept_bytes.size() < offset + 2) return std::nullopt;
    uint16_t sig_len = (static_cast<uint16_t>(accept_bytes[offset]) << 8) |
                        accept_bytes[offset + 1];
    offset += 2;

    if (accept_bytes.size() < offset + sig_len) return std::nullopt;
    auto sig = accept_bytes.subspan(offset, sig_len);

    // Verify responder's signature
    auto sig_data = build_accept_sig_data(hello_random_, local_id_, responder_id,
                                          accept_random, kem_ct);
    if (!crypto::verify(sig_data, sig, responder_pubkey)) {
        return std::nullopt;
    }

    // Decapsulate to get shared secret
    auto shared_secret = crypto::kem_decapsulate(kem_ct, kem_secret_key_);
    if (!shared_secret) return std::nullopt;

    // Zero ephemeral secret key — no longer needed
    OQS_MEM_cleanse(kem_secret_key_.data(), kem_secret_key_.size());
    kem_secret_key_.clear();

    // Derive session keys
    SessionKeys keys;
    std::array<uint8_t, 32> i2r_key, r2i_key;
    derive_keys(*shared_secret, hello_random_, accept_random, i2r_key, r2i_key);

    // Zero shared secret
    OQS_MEM_cleanse(shared_secret->data(), shared_secret->size());

    // Initiator sends with i2r, receives with r2i
    keys.send_key = i2r_key;
    keys.recv_key = r2i_key;
    OQS_MEM_cleanse(i2r_key.data(), i2r_key.size());
    OQS_MEM_cleanse(r2i_key.data(), r2i_key.size());

    keys_ = std::move(keys);

    // Build CONFIRM: [2B BE sig_len] [sig]
    auto confirm_data = build_confirm_sig_data(hello_random_, accept_random,
                                               kem_ct, responder_id);
    auto confirm_sig = crypto::sign(confirm_data, own_signing_key);

    std::vector<uint8_t> confirm;
    uint16_t csig_len = static_cast<uint16_t>(confirm_sig.size());
    confirm.reserve(2 + confirm_sig.size());
    confirm.push_back(static_cast<uint8_t>((csig_len >> 8) & 0xFF));
    confirm.push_back(static_cast<uint8_t>(csig_len & 0xFF));
    confirm.insert(confirm.end(), confirm_sig.begin(), confirm_sig.end());

    return confirm;
}

std::optional<SessionKeys> HandshakeInitiator::session_keys() const {
    if (!keys_) return std::nullopt;
    // Copy keys (must be done carefully since SessionKeys is move-only)
    SessionKeys copy;
    copy.send_key = keys_->send_key;
    copy.recv_key = keys_->recv_key;
    copy.send_nonce = keys_->send_nonce;
    copy.recv_nonce = keys_->recv_nonce;
    return copy;
}

// ---------------------------------------------------------------------------
// HandshakeResponder
// ---------------------------------------------------------------------------

HandshakeResponder::HandshakeResponder(const NodeId& local_id)
    : local_id_(local_id) {}

std::optional<NodeId> HandshakeResponder::process_hello(std::span<const uint8_t> hello_bytes) {
    // HELLO format (probe byte already consumed by caller):
    // [1B probe] [1B version] [1B cipher]
    // [32B initiator_node_id] [2B BE kem_pk_len] [kem_pk] [32B hello_random]
    size_t offset = 0;

    if (hello_bytes.size() < 3 + 32 + 2) return std::nullopt;

    if (hello_bytes[offset++] != PROBE_BYTE) return std::nullopt;

    uint8_t version = hello_bytes[offset++];
    uint8_t cipher = hello_bytes[offset++];
    if (version != HANDSHAKE_VERSION || cipher != CIPHER_CHACHA20_POLY1305)
        return std::nullopt;

    std::copy_n(hello_bytes.data() + offset, 32, initiator_id_.id.begin());
    offset += 32;

    uint16_t pk_len = (static_cast<uint16_t>(hello_bytes[offset]) << 8) |
                       hello_bytes[offset + 1];
    offset += 2;

    if (hello_bytes.size() < offset + pk_len + 32) return std::nullopt;

    auto kem_pk = hello_bytes.subspan(offset, pk_len);
    offset += pk_len;

    std::copy_n(hello_bytes.data() + offset, 32, hello_random_.begin());
    offset += 32;

    // Extract initiator's signing pubkey
    if (hello_bytes.size() < offset + 2) return std::nullopt;
    uint16_t sig_pk_len = (static_cast<uint16_t>(hello_bytes[offset]) << 8) |
                           hello_bytes[offset + 1];
    offset += 2;

    if (hello_bytes.size() < offset + sig_pk_len) return std::nullopt;
    initiator_signing_pubkey_.assign(
        hello_bytes.data() + offset, hello_bytes.data() + offset + sig_pk_len);
    offset += sig_pk_len;

    // Verify identity: SHA3-256(signing_pubkey) must equal initiator's node ID
    auto expected_id = crypto::sha3_256(initiator_signing_pubkey_);
    if (expected_id != initiator_id_.id) {
        initiator_signing_pubkey_.clear();
        return std::nullopt;
    }

    // Encapsulate to get shared secret + ciphertext
    auto encap = crypto::kem_encapsulate(kem_pk);
    kem_ciphertext_ = std::move(encap.ciphertext);

    // Generate accept random
    fill_random(accept_random_.data(), accept_random_.size());

    // Derive session keys
    SessionKeys keys;
    std::array<uint8_t, 32> i2r_key, r2i_key;
    derive_keys(encap.shared_secret, hello_random_, accept_random_, i2r_key, r2i_key);

    // Zero shared secret
    OQS_MEM_cleanse(encap.shared_secret.data(), encap.shared_secret.size());

    // Responder receives with i2r, sends with r2i
    keys.recv_key = i2r_key;
    keys.send_key = r2i_key;
    OQS_MEM_cleanse(i2r_key.data(), i2r_key.size());
    OQS_MEM_cleanse(r2i_key.data(), r2i_key.size());

    keys_ = std::move(keys);
    hello_processed_ = true;

    return initiator_id_;
}

std::optional<std::vector<uint8_t>> HandshakeResponder::generate_accept(
    std::span<const uint8_t> own_signing_key,
    std::span<const uint8_t> signing_pubkey) {
    if (!hello_processed_) return std::nullopt;

    // Sign: hello_random || initiator_id || responder_id || accept_random || kem_ciphertext
    auto sig_data = build_accept_sig_data(hello_random_, initiator_id_, local_id_,
                                          accept_random_, kem_ciphertext_);
    auto sig = crypto::sign(sig_data, own_signing_key);

    // ACCEPT format:
    // [1B version] [1B cipher] [32B responder_node_id]
    // [2B BE ct_len] [ct] [32B accept_random]
    // [2B BE sig_pk_len] [signing_pk]
    // [2B BE sig_len] [sig]
    uint16_t ct_len = static_cast<uint16_t>(kem_ciphertext_.size());
    uint16_t sig_pk_len = static_cast<uint16_t>(signing_pubkey.size());
    uint16_t sig_len = static_cast<uint16_t>(sig.size());

    std::vector<uint8_t> accept;
    accept.reserve(2 + 32 + 2 + kem_ciphertext_.size() + 32 +
                   2 + signing_pubkey.size() + 2 + sig.size());

    accept.push_back(HANDSHAKE_VERSION);
    accept.push_back(CIPHER_CHACHA20_POLY1305);

    accept.insert(accept.end(), local_id_.id.begin(), local_id_.id.end());

    accept.push_back(static_cast<uint8_t>((ct_len >> 8) & 0xFF));
    accept.push_back(static_cast<uint8_t>(ct_len & 0xFF));
    accept.insert(accept.end(), kem_ciphertext_.begin(), kem_ciphertext_.end());

    accept.insert(accept.end(), accept_random_.begin(), accept_random_.end());

    accept.push_back(static_cast<uint8_t>((sig_pk_len >> 8) & 0xFF));
    accept.push_back(static_cast<uint8_t>(sig_pk_len & 0xFF));
    accept.insert(accept.end(), signing_pubkey.begin(), signing_pubkey.end());

    accept.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    accept.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    accept.insert(accept.end(), sig.begin(), sig.end());

    return accept;
}

bool HandshakeResponder::process_confirm(std::span<const uint8_t> confirm_bytes) {
    if (initiator_signing_pubkey_.empty()) return false;

    // CONFIRM format: [2B BE sig_len] [sig]
    if (confirm_bytes.size() < 2) return false;

    uint16_t sig_len = (static_cast<uint16_t>(confirm_bytes[0]) << 8) |
                        confirm_bytes[1];
    if (confirm_bytes.size() < 2u + sig_len) return false;

    auto sig = confirm_bytes.subspan(2, sig_len);

    // Verify: hello_random || accept_random || kem_ciphertext || responder_id
    auto sig_data = build_confirm_sig_data(hello_random_, accept_random_,
                                           kem_ciphertext_, local_id_);
    return crypto::verify(sig_data, sig, initiator_signing_pubkey_);
}

std::optional<SessionKeys> HandshakeResponder::session_keys() const {
    if (!keys_) return std::nullopt;
    SessionKeys copy;
    copy.send_key = keys_->send_key;
    copy.recv_key = keys_->recv_key;
    copy.send_nonce = keys_->send_nonce;
    copy.recv_nonce = keys_->recv_nonce;
    return copy;
}

std::optional<NodeId> HandshakeResponder::initiator_id() const {
    if (!hello_processed_) return std::nullopt;
    return initiator_id_;
}

// ---------------------------------------------------------------------------
// Encrypted frame helpers
// ---------------------------------------------------------------------------

std::vector<uint8_t> encrypt_frame(SessionKeys& keys,
                                    std::span<const uint8_t> plaintext) {
    // Build 4-byte length header (ciphertext length = plaintext + 16 tag)
    uint32_t ct_len = static_cast<uint32_t>(plaintext.size() + crypto::AEAD_TAG_SIZE);
    std::array<uint8_t, 4> len_header;
    len_header[0] = static_cast<uint8_t>((ct_len >> 24) & 0xFF);
    len_header[1] = static_cast<uint8_t>((ct_len >> 16) & 0xFF);
    len_header[2] = static_cast<uint8_t>((ct_len >> 8) & 0xFF);
    len_header[3] = static_cast<uint8_t>(ct_len & 0xFF);

    // Encrypt with AAD = length header
    auto ciphertext = crypto::aead_encrypt(keys.send_key, keys.send_nonce,
                                           plaintext, len_header);
    keys.send_nonce++;

    // Prepend length header
    std::vector<uint8_t> frame;
    frame.reserve(4 + ciphertext.size());
    frame.insert(frame.end(), len_header.begin(), len_header.end());
    frame.insert(frame.end(), ciphertext.begin(), ciphertext.end());

    return frame;
}

std::optional<std::vector<uint8_t>> decrypt_frame(
    SessionKeys& keys,
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> aad) {

    auto plaintext = crypto::aead_decrypt(keys.recv_key, keys.recv_nonce,
                                          ciphertext, aad);
    if (plaintext) {
        keys.recv_nonce++;
    }
    return plaintext;
}

} // namespace chromatin::kademlia::tcp_encryption
