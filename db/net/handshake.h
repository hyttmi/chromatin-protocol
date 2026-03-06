#pragma once

#include "crypto/aead.h"
#include "crypto/hash.h"
#include "crypto/kdf.h"
#include "crypto/kem.h"
#include "crypto/secure_bytes.h"
#include "identity/identity.h"
#include "net/framing.h"
#include "net/protocol.h"

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace chromatin::net {

/// Directional session keys derived from ML-KEM shared secret.
struct SessionKeys {
    crypto::SecureBytes send_key;   ///< 32 bytes, for encrypting outbound frames
    crypto::SecureBytes recv_key;   ///< 32 bytes, for decrypting inbound frames
    std::array<uint8_t, 32> session_fingerprint;  ///< For mutual auth signing
};

/// Derive directional session keys from ML-KEM shared secret.
/// is_initiator determines which direction maps to send vs recv.
/// HKDF context strings: "chromatin-init-to-resp-v1", "chromatin-resp-to-init-v1"
SessionKeys derive_session_keys(
    std::span<const uint8_t> shared_secret,
    std::span<const uint8_t> initiator_signing_pubkey,
    std::span<const uint8_t> responder_signing_pubkey,
    bool is_initiator);

/// Handshake error codes.
enum class HandshakeError {
    Success,
    KemFailed,
    AuthFailed,
    DecryptFailed,
    InvalidMessage,
};

/// Handshake initiator state machine.
/// Drives the initiator side of the PQ handshake protocol.
class HandshakeInitiator {
public:
    explicit HandshakeInitiator(const identity::NodeIdentity& identity);

    /// Step 1: Generate ephemeral KEM keypair, return KemPubkey message bytes (unencrypted).
    std::vector<uint8_t> start();

    /// Step 2: Receive KemCiphertext from responder, derive session keys.
    HandshakeError receive_kem_ciphertext(std::span<const uint8_t> kem_ct_msg);

    /// Step 3: Produce encrypted AuthSignature message (signing pubkey + signature).
    /// Must be called after receive_kem_ciphertext succeeds.
    std::vector<uint8_t> create_auth_message();

    /// Step 4: Receive and verify responder's encrypted AuthSignature.
    /// Returns error code and responder's signing public key on success.
    std::pair<HandshakeError, std::vector<uint8_t>> verify_peer_auth(
        std::span<const uint8_t> encrypted_auth_msg);

    /// Get the established session keys (valid after successful handshake).
    SessionKeys take_session_keys();

private:
    const identity::NodeIdentity& identity_;
    crypto::KEM kem_;
    SessionKeys keys_;
    bool keys_derived_ = false;
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;
};

/// Handshake responder state machine.
/// Drives the responder side of the PQ handshake protocol.
class HandshakeResponder {
public:
    explicit HandshakeResponder(const identity::NodeIdentity& identity);

    /// Step 1: Receive KemPubkey from initiator, encapsulate, return KemCiphertext message bytes.
    std::pair<HandshakeError, std::vector<uint8_t>> receive_kem_pubkey(
        std::span<const uint8_t> kem_pk_msg);

    /// Step 2: Receive and verify initiator's encrypted AuthSignature.
    /// Returns error code and initiator's signing public key on success.
    std::pair<HandshakeError, std::vector<uint8_t>> verify_peer_auth(
        std::span<const uint8_t> encrypted_auth_msg);

    /// Step 3: Produce encrypted AuthSignature message.
    std::vector<uint8_t> create_auth_message();

    /// Get the established session keys.
    SessionKeys take_session_keys();

private:
    const identity::NodeIdentity& identity_;
    SessionKeys keys_;
    bool keys_derived_ = false;
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;

    // Stored after KEM exchange for auth phase
    std::vector<uint8_t> initiator_signing_pubkey_;
};

} // namespace chromatin::net
