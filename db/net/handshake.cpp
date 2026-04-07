#include "db/net/handshake.h"

#include "db/crypto/signing.h"
#include "db/net/auth_helpers.h"
#include "db/wire/transport_generated.h"

#include <cstring>

namespace chromatindb::net {

// =============================================================================
// Session key derivation
// =============================================================================

SessionKeys derive_session_keys(
    std::span<const uint8_t> shared_secret,
    std::span<const uint8_t> initiator_signing_pubkey,
    std::span<const uint8_t> responder_signing_pubkey,
    bool is_initiator) {

    // HKDF extract: PRK from shared secret (empty salt per RFC 5869)
    std::span<const uint8_t> empty_salt{};
    auto prk = crypto::KDF::extract(empty_salt, shared_secret);

    // Derive directional keys
    auto init_to_resp = crypto::KDF::expand(
        prk.span(), "chromatin-init-to-resp-v1", crypto::AEAD::KEY_SIZE);
    auto resp_to_init = crypto::KDF::expand(
        prk.span(), "chromatin-resp-to-init-v1", crypto::AEAD::KEY_SIZE);

    // Session fingerprint: SHA3-256(shared_secret || initiator_pubkey || responder_pubkey)
    std::vector<uint8_t> fingerprint_input;
    fingerprint_input.reserve(shared_secret.size() +
                               initiator_signing_pubkey.size() +
                               responder_signing_pubkey.size());
    fingerprint_input.insert(fingerprint_input.end(),
        shared_secret.begin(), shared_secret.end());
    fingerprint_input.insert(fingerprint_input.end(),
        initiator_signing_pubkey.begin(), initiator_signing_pubkey.end());
    fingerprint_input.insert(fingerprint_input.end(),
        responder_signing_pubkey.begin(), responder_signing_pubkey.end());
    auto fingerprint = crypto::sha3_256(fingerprint_input);

    SessionKeys keys;
    keys.session_fingerprint = fingerprint;

    if (is_initiator) {
        keys.send_key = std::move(init_to_resp);
        keys.recv_key = std::move(resp_to_init);
    } else {
        keys.send_key = std::move(resp_to_init);
        keys.recv_key = std::move(init_to_resp);
    }

    return keys;
}

// =============================================================================
// Lightweight session key derivation (non-PQ, for trusted connections)
// =============================================================================

SessionKeys derive_lightweight_session_keys(
    std::span<const uint8_t> initiator_nonce,
    std::span<const uint8_t> responder_nonce,
    std::span<const uint8_t> initiator_signing_pk,
    std::span<const uint8_t> responder_signing_pk,
    bool is_initiator) {

    // IKM = initiator_nonce || responder_nonce (64 bytes)
    std::vector<uint8_t> ikm;
    ikm.reserve(initiator_nonce.size() + responder_nonce.size());
    ikm.insert(ikm.end(), initiator_nonce.begin(), initiator_nonce.end());
    ikm.insert(ikm.end(), responder_nonce.begin(), responder_nonce.end());

    // Salt = initiator_signing_pk || responder_signing_pk (5184 bytes)
    std::vector<uint8_t> salt;
    salt.reserve(initiator_signing_pk.size() + responder_signing_pk.size());
    salt.insert(salt.end(), initiator_signing_pk.begin(), initiator_signing_pk.end());
    salt.insert(salt.end(), responder_signing_pk.begin(), responder_signing_pk.end());

    // HKDF extract: PRK from nonces (IKM) with pubkeys as salt
    auto prk = crypto::KDF::extract(salt, ikm);

    // Derive directional keys -- same context strings as PQ path
    auto init_to_resp = crypto::KDF::expand(
        prk.span(), "chromatin-init-to-resp-v1", crypto::AEAD::KEY_SIZE);
    auto resp_to_init = crypto::KDF::expand(
        prk.span(), "chromatin-resp-to-init-v1", crypto::AEAD::KEY_SIZE);

    // Session fingerprint: SHA3-256(IKM || Salt)
    std::vector<uint8_t> fp_input;
    fp_input.reserve(ikm.size() + salt.size());
    fp_input.insert(fp_input.end(), ikm.begin(), ikm.end());
    fp_input.insert(fp_input.end(), salt.begin(), salt.end());
    auto fingerprint = crypto::sha3_256(fp_input);

    SessionKeys keys;
    keys.session_fingerprint = fingerprint;

    if (is_initiator) {
        keys.send_key = std::move(init_to_resp);
        keys.recv_key = std::move(resp_to_init);
    } else {
        keys.send_key = std::move(resp_to_init);
        keys.recv_key = std::move(init_to_resp);
    }

    return keys;
}

// =============================================================================
// HandshakeInitiator
// =============================================================================

HandshakeInitiator::HandshakeInitiator(const identity::NodeIdentity& identity)
    : identity_(identity) {}

std::vector<uint8_t> HandshakeInitiator::start() {
    kem_.generate_keypair();
    auto kem_pk = kem_.export_public_key();
    auto signing_pk = identity_.public_key();

    // Payload: [kem_pubkey (1568B)][signing_pubkey (2592B)]
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), kem_pk.begin(), kem_pk.end());
    payload.insert(payload.end(), signing_pk.begin(), signing_pk.end());

    // Encode as unencrypted TransportMessage (KEM exchange is plaintext)
    return TransportCodec::encode(wire::TransportMsgType_KemPubkey, payload);
}

HandshakeError HandshakeInitiator::receive_kem_ciphertext(
    std::span<const uint8_t> kem_ct_msg) {

    auto decoded = TransportCodec::decode(kem_ct_msg);
    if (!decoded || decoded->type != wire::TransportMsgType_KemCiphertext) {
        return HandshakeError::InvalidMessage;
    }

    // Payload format: [ciphertext (1568B)][signing_pubkey (2592B)]
    size_t expected_size = crypto::KEM::CIPHERTEXT_SIZE + crypto::Signer::PUBLIC_KEY_SIZE;
    if (decoded->payload.size() != expected_size) {
        return HandshakeError::InvalidMessage;
    }

    auto ct_span = std::span<const uint8_t>(
        decoded->payload.data(), crypto::KEM::CIPHERTEXT_SIZE);
    auto resp_signing_pk = std::span<const uint8_t>(
        decoded->payload.data() + crypto::KEM::CIPHERTEXT_SIZE,
        crypto::Signer::PUBLIC_KEY_SIZE);

    // Decapsulate to get shared secret
    auto shared_secret = kem_.decaps(ct_span, kem_.export_secret_key());

    // Derive session keys (initiator side)
    keys_ = derive_session_keys(
        shared_secret.span(),
        identity_.public_key(),
        resp_signing_pk,
        true);
    keys_derived_ = true;

    return HandshakeError::Success;
}

std::vector<uint8_t> HandshakeInitiator::create_auth_message() {
    // Sign the session fingerprint
    auto signature = identity_.sign(keys_.session_fingerprint);

    // Encode: [pubkey_size][pubkey][signature]
    auto auth_payload = encode_auth_payload(identity_.public_key(), signature);

    // Encode as TransportMessage
    auto msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);

    // Encrypt with session send key
    auto frame = write_frame(msg, keys_.send_key.span(), send_counter_++);
    return frame;
}

std::pair<HandshakeError, std::vector<uint8_t>> HandshakeInitiator::verify_peer_auth(
    std::span<const uint8_t> encrypted_auth_msg) {

    // Decrypt with recv key
    auto frame_result = read_frame(encrypted_auth_msg, keys_.recv_key.span(), recv_counter_++);
    if (!frame_result) {
        return {HandshakeError::DecryptFailed, {}};
    }

    // Decode transport message
    auto decoded = TransportCodec::decode(frame_result->plaintext);
    if (!decoded || decoded->type != wire::TransportMsgType_AuthSignature) {
        return {HandshakeError::InvalidMessage, {}};
    }

    // Parse auth payload
    auto auth = decode_auth_payload(decoded->payload);
    if (!auth) {
        return {HandshakeError::InvalidMessage, {}};
    }

    // Verify signature over session fingerprint
    bool valid = crypto::Signer::verify(
        keys_.session_fingerprint, auth->signature, auth->pubkey);
    if (!valid) {
        return {HandshakeError::AuthFailed, {}};
    }

    return {HandshakeError::Success, std::move(auth->pubkey)};
}

SessionKeys HandshakeInitiator::take_session_keys() {
    return std::move(keys_);
}

// =============================================================================
// HandshakeResponder
// =============================================================================

HandshakeResponder::HandshakeResponder(const identity::NodeIdentity& identity)
    : identity_(identity) {}

std::pair<HandshakeError, std::vector<uint8_t>> HandshakeResponder::receive_kem_pubkey(
    std::span<const uint8_t> kem_pk_msg) {

    auto decoded = TransportCodec::decode(kem_pk_msg);
    if (!decoded || decoded->type != wire::TransportMsgType_KemPubkey) {
        return {HandshakeError::InvalidMessage, {}};
    }

    // Payload format: [kem_pubkey (1568B)][signing_pubkey (2592B)]
    size_t expected_size = crypto::KEM::PUBLIC_KEY_SIZE + crypto::Signer::PUBLIC_KEY_SIZE;
    if (decoded->payload.size() != expected_size) {
        return {HandshakeError::InvalidMessage, {}};
    }

    auto kem_pk = std::span<const uint8_t>(
        decoded->payload.data(), crypto::KEM::PUBLIC_KEY_SIZE);
    auto init_signing_pk = std::span<const uint8_t>(
        decoded->payload.data() + crypto::KEM::PUBLIC_KEY_SIZE,
        crypto::Signer::PUBLIC_KEY_SIZE);

    // Encapsulate to produce ciphertext + shared secret
    crypto::KEM kem;
    auto [ct, ss] = kem.encaps(kem_pk);

    // Store initiator signing pubkey for later auth verification
    initiator_signing_pubkey_.assign(init_signing_pk.begin(), init_signing_pk.end());

    // Derive session keys (responder side)
    keys_ = derive_session_keys(
        ss.span(),
        init_signing_pk,
        identity_.public_key(),
        false);
    keys_derived_ = true;

    // Build KemCiphertext response: [ciphertext (1568B)][our signing_pubkey (2592B)]
    std::vector<uint8_t> response_payload;
    response_payload.insert(response_payload.end(), ct.begin(), ct.end());
    response_payload.insert(response_payload.end(),
        identity_.public_key().begin(), identity_.public_key().end());

    auto response = TransportCodec::encode(
        wire::TransportMsgType_KemCiphertext, response_payload);

    return {HandshakeError::Success, std::move(response)};
}

std::pair<HandshakeError, std::vector<uint8_t>> HandshakeResponder::verify_peer_auth(
    std::span<const uint8_t> encrypted_auth_msg) {

    // Decrypt with recv key
    auto frame_result = read_frame(encrypted_auth_msg, keys_.recv_key.span(), recv_counter_++);
    if (!frame_result) {
        return {HandshakeError::DecryptFailed, {}};
    }

    // Decode transport message
    auto decoded = TransportCodec::decode(frame_result->plaintext);
    if (!decoded || decoded->type != wire::TransportMsgType_AuthSignature) {
        return {HandshakeError::InvalidMessage, {}};
    }

    // Parse auth payload
    auto auth = decode_auth_payload(decoded->payload);
    if (!auth) {
        return {HandshakeError::InvalidMessage, {}};
    }

    // Verify the signing pubkey matches what was sent in KemPubkey
    if (auth->pubkey != initiator_signing_pubkey_) {
        return {HandshakeError::AuthFailed, {}};
    }

    // Verify signature over session fingerprint
    bool valid = crypto::Signer::verify(
        keys_.session_fingerprint, auth->signature, auth->pubkey);
    if (!valid) {
        return {HandshakeError::AuthFailed, {}};
    }

    return {HandshakeError::Success, std::move(auth->pubkey)};
}

std::vector<uint8_t> HandshakeResponder::create_auth_message() {
    // Sign the session fingerprint
    auto signature = identity_.sign(keys_.session_fingerprint);

    // Encode auth payload
    auto auth_payload = encode_auth_payload(identity_.public_key(), signature);

    // Encode as TransportMessage
    auto msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);

    // Encrypt with session send key
    auto frame = write_frame(msg, keys_.send_key.span(), send_counter_++);
    return frame;
}

SessionKeys HandshakeResponder::take_session_keys() {
    return std::move(keys_);
}

} // namespace chromatindb::net
