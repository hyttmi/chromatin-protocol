#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"

namespace chromatin::kademlia::tcp_encryption {

// Handshake probe byte — distinguishes encrypted from plaintext connections.
// 0xCE is never a valid first byte of a CHRM message (magic starts with 'C' = 0x43).
inline constexpr uint8_t PROBE_BYTE = 0xCE;

// Cipher suite identifier
inline constexpr uint8_t CIPHER_CHACHA20_POLY1305 = 0x01;

// Handshake version
inline constexpr uint8_t HANDSHAKE_VERSION = 0x01;

// Per-direction session keys derived from ML-KEM shared secret.
struct SessionKeys {
    std::array<uint8_t, 32> send_key{};
    std::array<uint8_t, 32> recv_key{};
    uint64_t send_nonce = 0;
    uint64_t recv_nonce = 0;

    ~SessionKeys();

    SessionKeys() = default;
    SessionKeys(SessionKeys&&) noexcept;
    SessionKeys& operator=(SessionKeys&&) noexcept;
    SessionKeys(const SessionKeys&) = delete;
    SessionKeys& operator=(const SessionKeys&) = delete;
};

// Initiator side of the 3-message handshake.
//
// Usage:
//   HandshakeInitiator init(my_node_id);
//   auto hello = init.generate_hello(my_signing_pubkey);
//   // send hello, receive accept_bytes
//   auto confirm = init.process_accept(accept_bytes, my_signing_key);
//   // send confirm
//   auto keys = init.session_keys();
class HandshakeInitiator {
public:
    explicit HandshakeInitiator(const NodeId& local_id);

    // Generate HELLO message bytes to send to responder.
    // signing_pubkey is embedded so the responder can verify our identity
    // without prior knowledge (SHA3-256(signing_pubkey) == our node_id).
    std::vector<uint8_t> generate_hello(
        std::span<const uint8_t> signing_pubkey);

    // Process ACCEPT from responder. Returns CONFIRM bytes on success.
    // Responder's signing pubkey is extracted from ACCEPT and verified via
    // SHA3-256(pubkey) == responder_node_id.
    // own_signing_key = our ML-DSA-87 secret key for signing CONFIRM.
    std::optional<std::vector<uint8_t>> process_accept(
        std::span<const uint8_t> accept_bytes,
        std::span<const uint8_t> own_signing_key);

    // Get derived session keys. Only valid after process_accept() succeeds.
    std::optional<SessionKeys> session_keys() const;

private:
    NodeId local_id_;
    std::array<uint8_t, 32> hello_random_{};
    std::vector<uint8_t> kem_secret_key_;  // ephemeral ML-KEM secret key
    std::vector<uint8_t> kem_public_key_;  // ephemeral ML-KEM public key
    std::optional<SessionKeys> keys_;
};

// Responder side of the 3-message handshake.
//
// Usage:
//   HandshakeResponder resp(my_node_id);
//   auto initiator_id = resp.process_hello(hello_bytes);
//   auto accept = resp.generate_accept(my_signing_key, my_signing_pubkey);
//   // send accept, receive confirm_bytes
//   bool ok = resp.process_confirm(confirm_bytes);
//   auto keys = resp.session_keys();
class HandshakeResponder {
public:
    explicit HandshakeResponder(const NodeId& local_id);

    // Process HELLO from initiator. Returns initiator's node ID on success.
    // Extracts and verifies initiator's signing pubkey from HELLO via
    // SHA3-256(pubkey) == initiator_node_id.
    std::optional<NodeId> process_hello(std::span<const uint8_t> hello_bytes);

    // Generate ACCEPT message bytes.
    // own_signing_key = ML-DSA-87 secret key for signing ACCEPT.
    // signing_pubkey is embedded so the initiator can verify our identity.
    std::optional<std::vector<uint8_t>> generate_accept(
        std::span<const uint8_t> own_signing_key,
        std::span<const uint8_t> signing_pubkey);

    // Process CONFIRM from initiator. Returns true on signature verification success.
    // Uses the initiator's signing pubkey extracted from HELLO.
    bool process_confirm(std::span<const uint8_t> confirm_bytes);

    // Get derived session keys. Only valid after successful handshake.
    std::optional<SessionKeys> session_keys() const;

    // Get the initiator's node ID (available after process_hello).
    std::optional<NodeId> initiator_id() const;

private:
    NodeId local_id_;
    NodeId initiator_id_{};
    std::array<uint8_t, 32> hello_random_{};
    std::array<uint8_t, 32> accept_random_{};
    std::vector<uint8_t> kem_ciphertext_;
    std::vector<uint8_t> initiator_signing_pubkey_;  // extracted from HELLO
    bool hello_processed_ = false;
    std::optional<SessionKeys> keys_;
};

// Encrypt a CHRM message for sending over an encrypted connection.
// Prepends 4-byte BE ciphertext length. AAD = the 4-byte length header.
// Increments session send_nonce.
std::vector<uint8_t> encrypt_frame(SessionKeys& keys,
                                    std::span<const uint8_t> plaintext);

// Decrypt a received encrypted frame.
// ciphertext includes the 16-byte Poly1305 tag but NOT the 4-byte length header.
// aad = the 4-byte length header read from the wire.
// Increments session recv_nonce.
std::optional<std::vector<uint8_t>> decrypt_frame(
    SessionKeys& keys,
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> aad);

} // namespace chromatin::kademlia::tcp_encryption
