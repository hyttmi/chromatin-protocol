#include <catch2/catch_test_macros.hpp>
#include "db/net/handshake.h"
#include "db/net/auth_helpers.h"
#include "db/net/framing.h"
#include "db/identity/identity.h"
#include "db/crypto/kem.h"
#include "db/crypto/aead.h"

#include <sodium.h>

using namespace chromatindb::net;
using namespace chromatindb::crypto;

TEST_CASE("derive_session_keys produces correct directional keys", "[handshake][keys]") {
    // Simulate a shared secret from KEM
    auto shared_secret = AEAD::keygen(); // 32 bytes, just for testing

    // Create two identities
    auto initiator = chromatindb::identity::NodeIdentity::generate();
    auto responder = chromatindb::identity::NodeIdentity::generate();

    auto init_keys = derive_session_keys(
        shared_secret.span(),
        initiator.public_key(),
        responder.public_key(),
        true);

    auto resp_keys = derive_session_keys(
        shared_secret.span(),
        initiator.public_key(),
        responder.public_key(),
        false);

    SECTION("initiator send_key == responder recv_key") {
        REQUIRE(init_keys.send_key.size() == AEAD::KEY_SIZE);
        REQUIRE(resp_keys.recv_key.size() == AEAD::KEY_SIZE);
        REQUIRE(std::equal(
            init_keys.send_key.data(),
            init_keys.send_key.data() + init_keys.send_key.size(),
            resp_keys.recv_key.data()));
    }

    SECTION("responder send_key == initiator recv_key") {
        REQUIRE(resp_keys.send_key.size() == AEAD::KEY_SIZE);
        REQUIRE(init_keys.recv_key.size() == AEAD::KEY_SIZE);
        REQUIRE(std::equal(
            resp_keys.send_key.data(),
            resp_keys.send_key.data() + resp_keys.send_key.size(),
            init_keys.recv_key.data()));
    }

    SECTION("session fingerprint identical on both sides") {
        REQUIRE(init_keys.session_fingerprint == resp_keys.session_fingerprint);
    }

    SECTION("send_key != recv_key (different directions)") {
        REQUIRE_FALSE(std::equal(
            init_keys.send_key.data(),
            init_keys.send_key.data() + init_keys.send_key.size(),
            init_keys.recv_key.data()));
    }

    SECTION("different shared secrets produce different keys") {
        auto other_secret = AEAD::keygen();
        auto other_keys = derive_session_keys(
            other_secret.span(),
            initiator.public_key(),
            responder.public_key(),
            true);

        REQUIRE_FALSE(std::equal(
            init_keys.send_key.data(),
            init_keys.send_key.data() + init_keys.send_key.size(),
            other_keys.send_key.data()));
    }

    SECTION("fingerprint includes both signing pubkeys") {
        // Swapping pubkey order should change fingerprint
        auto swapped_keys = derive_session_keys(
            shared_secret.span(),
            responder.public_key(),  // swapped
            initiator.public_key(),  // swapped
            true);
        REQUIRE(init_keys.session_fingerprint != swapped_keys.session_fingerprint);
    }
}

TEST_CASE("Full handshake happy path", "[handshake]") {
    auto initiator_id = chromatindb::identity::NodeIdentity::generate();
    auto responder_id = chromatindb::identity::NodeIdentity::generate();

    HandshakeInitiator initiator(initiator_id);
    HandshakeResponder responder(responder_id);

    // Step 1: Initiator generates KEM pubkey message
    auto kem_pk_msg = initiator.start();
    REQUIRE(!kem_pk_msg.empty());

    // Step 2: Responder receives KEM pubkey, sends ciphertext
    auto [resp_err, kem_ct_msg] = responder.receive_kem_pubkey(kem_pk_msg);
    REQUIRE(resp_err == HandshakeError::Success);
    REQUIRE(!kem_ct_msg.empty());

    // Step 3: Initiator receives ciphertext, derives session keys
    auto init_err = initiator.receive_kem_ciphertext(kem_ct_msg);
    REQUIRE(init_err == HandshakeError::Success);

    // Step 4: Initiator creates encrypted auth message
    auto init_auth = initiator.create_auth_message(Role::Peer);
    REQUIRE(!init_auth.empty());

    // Step 5: Responder verifies initiator's auth
    auto [resp_auth_err, init_pubkey] = responder.verify_peer_auth(init_auth);
    REQUIRE(resp_auth_err == HandshakeError::Success);

    // Verify the initiator's pubkey matches
    auto expected_pk = initiator_id.public_key();
    REQUIRE(init_pubkey.size() == expected_pk.size());
    REQUIRE(std::equal(init_pubkey.begin(), init_pubkey.end(), expected_pk.begin()));

    // Step 6: Responder creates encrypted auth message
    auto resp_auth = responder.create_auth_message(Role::Peer);
    REQUIRE(!resp_auth.empty());

    // Step 7: Initiator verifies responder's auth
    auto [init_auth_err, resp_pubkey] = initiator.verify_peer_auth(resp_auth);
    REQUIRE(init_auth_err == HandshakeError::Success);

    // Verify the responder's pubkey matches
    auto expected_resp_pk = responder_id.public_key();
    REQUIRE(resp_pubkey.size() == expected_resp_pk.size());
    REQUIRE(std::equal(resp_pubkey.begin(), resp_pubkey.end(), expected_resp_pk.begin()));

    // Both sides can now exchange encrypted data using session keys
    auto init_session = initiator.take_session_keys();
    auto resp_session = responder.take_session_keys();

    // Verify key symmetry
    REQUIRE(init_session.session_fingerprint == resp_session.session_fingerprint);
    REQUIRE(std::equal(
        init_session.send_key.data(),
        init_session.send_key.data() + init_session.send_key.size(),
        resp_session.recv_key.data()));
    REQUIRE(std::equal(
        init_session.recv_key.data(),
        init_session.recv_key.data() + init_session.recv_key.size(),
        resp_session.send_key.data()));
}

TEST_CASE("Handshake with self (same identity) succeeds", "[handshake]") {
    auto id = chromatindb::identity::NodeIdentity::generate();

    HandshakeInitiator initiator(id);
    HandshakeResponder responder(id);

    auto kem_pk_msg = initiator.start();
    auto [resp_err, kem_ct_msg] = responder.receive_kem_pubkey(kem_pk_msg);
    REQUIRE(resp_err == HandshakeError::Success);

    auto init_err = initiator.receive_kem_ciphertext(kem_ct_msg);
    REQUIRE(init_err == HandshakeError::Success);

    auto init_auth = initiator.create_auth_message(Role::Peer);
    auto [resp_auth_err, _pk1] = responder.verify_peer_auth(init_auth);
    REQUIRE(resp_auth_err == HandshakeError::Success);

    auto resp_auth = responder.create_auth_message(Role::Peer);
    auto [init_auth_err, _pk2] = initiator.verify_peer_auth(resp_auth);
    REQUIRE(init_auth_err == HandshakeError::Success);
}

TEST_CASE("Handshake auth failure with wrong identity", "[handshake]") {
    auto initiator_id = chromatindb::identity::NodeIdentity::generate();
    auto responder_id = chromatindb::identity::NodeIdentity::generate();
    auto imposter_id = chromatindb::identity::NodeIdentity::generate();

    // Setup: initiator and responder complete KEM exchange
    HandshakeInitiator initiator(initiator_id);
    HandshakeResponder responder(responder_id);

    auto kem_pk_msg = initiator.start();
    auto [resp_err, kem_ct_msg] = responder.receive_kem_pubkey(kem_pk_msg);
    REQUIRE(resp_err == HandshakeError::Success);
    auto init_err = initiator.receive_kem_ciphertext(kem_ct_msg);
    REQUIRE(init_err == HandshakeError::Success);

    SECTION("imposter creates auth message -- responder rejects") {
        // An imposter tries to authenticate as initiator
        // The imposter can't create a valid auth because:
        // 1. They don't know the session keys (can't decrypt/encrypt)
        // 2. Even if they somehow got the keys, their signature won't match the
        //    initiator's pubkey that was sent during KEM exchange
        //
        // Test: initiator sends valid auth, responder verifies (should pass)
        // Then try with tampered auth (simulated imposter)
        auto init_auth = initiator.create_auth_message(Role::Peer);

        // Tamper with the encrypted auth message (flip a byte in ciphertext)
        auto tampered = init_auth;
        if (tampered.size() > 10) {
            tampered[10] ^= 0xFF;
        }
        auto [auth_err, _pk] = responder.verify_peer_auth(tampered);
        // Should fail -- either decrypt fails or signature doesn't verify
        REQUIRE(auth_err != HandshakeError::Success);
    }
}

TEST_CASE("Handshake rejects invalid KEM messages", "[handshake]") {
    SECTION("responder rejects wrong message type") {
        auto id = chromatindb::identity::NodeIdentity::generate();
        HandshakeResponder responder(id);

        // Send a Ping message instead of KemPubkey
        std::span<const uint8_t> empty{};
        auto wrong_msg = TransportCodec::encode(
            chromatindb::wire::TransportMsgType_Ping, empty);
        auto [err, _] = responder.receive_kem_pubkey(wrong_msg);
        REQUIRE(err == HandshakeError::InvalidMessage);
    }

    SECTION("responder rejects wrong payload size") {
        auto id = chromatindb::identity::NodeIdentity::generate();
        HandshakeResponder responder(id);

        // Send KemPubkey with wrong size payload
        std::vector<uint8_t> bad_payload(100, 0x42);
        auto bad_msg = TransportCodec::encode(
            chromatindb::wire::TransportMsgType_KemPubkey, bad_payload);
        auto [err, _] = responder.receive_kem_pubkey(bad_msg);
        REQUIRE(err == HandshakeError::InvalidMessage);
    }

    SECTION("initiator rejects wrong message type for ciphertext") {
        auto init_id = chromatindb::identity::NodeIdentity::generate();
        HandshakeInitiator initiator(init_id);
        initiator.start(); // Generate KEM keypair

        std::span<const uint8_t> empty{};
        auto wrong_msg = TransportCodec::encode(
            chromatindb::wire::TransportMsgType_Ping, empty);
        auto err = initiator.receive_kem_ciphertext(wrong_msg);
        REQUIRE(err == HandshakeError::InvalidMessage);
    }
}

// =============================================================================
// Lightweight session key derivation tests
// =============================================================================

TEST_CASE("derive_lightweight_session_keys produces symmetric directional keys", "[handshake][lightweight]") {
    auto initiator_id = chromatindb::identity::NodeIdentity::generate();
    auto responder_id = chromatindb::identity::NodeIdentity::generate();

    std::array<uint8_t, 32> nonce_i{}, nonce_r{};
    randombytes_buf(nonce_i.data(), nonce_i.size());
    randombytes_buf(nonce_r.data(), nonce_r.size());

    auto init_keys = derive_lightweight_session_keys(
        nonce_i, nonce_r,
        initiator_id.public_key(),
        responder_id.public_key(),
        true);

    auto resp_keys = derive_lightweight_session_keys(
        nonce_i, nonce_r,
        initiator_id.public_key(),
        responder_id.public_key(),
        false);

    SECTION("initiator send_key == responder recv_key") {
        REQUIRE(init_keys.send_key.size() == AEAD::KEY_SIZE);
        REQUIRE(resp_keys.recv_key.size() == AEAD::KEY_SIZE);
        REQUIRE(std::equal(
            init_keys.send_key.data(),
            init_keys.send_key.data() + init_keys.send_key.size(),
            resp_keys.recv_key.data()));
    }

    SECTION("responder send_key == initiator recv_key") {
        REQUIRE(std::equal(
            resp_keys.send_key.data(),
            resp_keys.send_key.data() + resp_keys.send_key.size(),
            init_keys.recv_key.data()));
    }

    SECTION("send_key != recv_key (different directions)") {
        REQUIRE_FALSE(std::equal(
            init_keys.send_key.data(),
            init_keys.send_key.data() + init_keys.send_key.size(),
            init_keys.recv_key.data()));
    }

    SECTION("session fingerprint identical on both sides") {
        REQUIRE(init_keys.session_fingerprint == resp_keys.session_fingerprint);
    }
}

TEST_CASE("derive_lightweight_session_keys: different nonces produce different keys", "[handshake][lightweight]") {
    auto initiator_id = chromatindb::identity::NodeIdentity::generate();
    auto responder_id = chromatindb::identity::NodeIdentity::generate();

    std::array<uint8_t, 32> nonce_i1{}, nonce_r1{}, nonce_i2{}, nonce_r2{};
    randombytes_buf(nonce_i1.data(), nonce_i1.size());
    randombytes_buf(nonce_r1.data(), nonce_r1.size());
    randombytes_buf(nonce_i2.data(), nonce_i2.size());
    randombytes_buf(nonce_r2.data(), nonce_r2.size());

    auto keys1 = derive_lightweight_session_keys(
        nonce_i1, nonce_r1,
        initiator_id.public_key(),
        responder_id.public_key(),
        true);

    auto keys2 = derive_lightweight_session_keys(
        nonce_i2, nonce_r2,
        initiator_id.public_key(),
        responder_id.public_key(),
        true);

    REQUIRE_FALSE(std::equal(
        keys1.send_key.data(),
        keys1.send_key.data() + keys1.send_key.size(),
        keys2.send_key.data()));
}

// =============================================================================
// CRYPTO-02 pubkey binding verification
// =============================================================================

TEST_CASE("verify_peer_auth rejects auth with mismatched pubkey", "[handshake][binding]") {
    // CRYPTO-02: A valid AuthSignature from a different identity is rejected
    // by the responder because the signing pubkey doesn't match the one
    // sent during the KEM exchange.

    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();
    auto attacker_id = chromatindb::identity::NodeIdentity::generate();

    // Normal KEM exchange between initiator and responder
    HandshakeInitiator hs_init(init_id);
    auto kem_pk_msg = hs_init.start();

    HandshakeResponder hs_resp(resp_id);
    auto [resp_err, kem_ct_msg] = hs_resp.receive_kem_pubkey(kem_pk_msg);
    REQUIRE(resp_err == HandshakeError::Success);

    auto init_err = hs_init.receive_kem_ciphertext(kem_ct_msg);
    REQUIRE(init_err == HandshakeError::Success);

    // Get the initiator's session keys to encrypt an attacker's auth message.
    // take_session_keys() moves the keys out, so the initiator can't create
    // auth messages after this -- we'll build our own manually.
    auto init_keys = hs_init.take_session_keys();

    // Attacker signs the session fingerprint with their own key (valid signature)
    auto attacker_sig = attacker_id.sign(init_keys.session_fingerprint);

    // Encode attacker's auth payload: [attacker_pubkey_size LE][attacker_pubkey][attacker_sig]
    auto attacker_payload = encode_auth_payload(
        Role::Peer, attacker_id.public_key(), attacker_sig);

    // Wrap as TransportMessage (AuthSignature type)
    auto attacker_msg = TransportCodec::encode(
        chromatindb::wire::TransportMsgType_AuthSignature, attacker_payload);

    // Encrypt with the CORRECT session send key and counter 0
    // (what create_auth_message would have used for the initiator)
    auto encrypted_attacker_auth = write_frame(
        attacker_msg, init_keys.send_key.span(), 0);

    // Responder verifies: decryption succeeds (correct key), auth payload
    // parses (valid format), but pubkey != initiator_signing_pubkey_ (mismatch).
    auto [verify_err, peer_pk] = hs_resp.verify_peer_auth(encrypted_attacker_auth);
    REQUIRE(verify_err == HandshakeError::AuthFailed);
    REQUIRE(peer_pk.empty());
}

TEST_CASE("derive_lightweight_session_keys: deterministic with same inputs", "[handshake][lightweight]") {
    auto initiator_id = chromatindb::identity::NodeIdentity::generate();
    auto responder_id = chromatindb::identity::NodeIdentity::generate();

    std::array<uint8_t, 32> nonce_i{}, nonce_r{};
    randombytes_buf(nonce_i.data(), nonce_i.size());
    randombytes_buf(nonce_r.data(), nonce_r.size());

    auto keys1 = derive_lightweight_session_keys(
        nonce_i, nonce_r,
        initiator_id.public_key(),
        responder_id.public_key(),
        true);

    auto keys2 = derive_lightweight_session_keys(
        nonce_i, nonce_r,
        initiator_id.public_key(),
        responder_id.public_key(),
        true);

    REQUIRE(std::equal(
        keys1.send_key.data(),
        keys1.send_key.data() + keys1.send_key.size(),
        keys2.send_key.data()));

    REQUIRE(keys1.session_fingerprint == keys2.session_fingerprint);
}
