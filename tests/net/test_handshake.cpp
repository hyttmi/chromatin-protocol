#include <catch2/catch_test_macros.hpp>
#include "net/handshake.h"
#include "identity/identity.h"
#include "crypto/kem.h"
#include "crypto/aead.h"

using namespace chromatin::net;
using namespace chromatin::crypto;

TEST_CASE("derive_session_keys produces correct directional keys", "[handshake][keys]") {
    // Simulate a shared secret from KEM
    auto shared_secret = AEAD::keygen(); // 32 bytes, just for testing

    // Create two identities
    auto initiator = chromatin::identity::NodeIdentity::generate();
    auto responder = chromatin::identity::NodeIdentity::generate();

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
    auto initiator_id = chromatin::identity::NodeIdentity::generate();
    auto responder_id = chromatin::identity::NodeIdentity::generate();

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
    auto init_auth = initiator.create_auth_message();
    REQUIRE(!init_auth.empty());

    // Step 5: Responder verifies initiator's auth
    auto [resp_auth_err, init_pubkey] = responder.verify_peer_auth(init_auth);
    REQUIRE(resp_auth_err == HandshakeError::Success);

    // Verify the initiator's pubkey matches
    auto expected_pk = initiator_id.public_key();
    REQUIRE(init_pubkey.size() == expected_pk.size());
    REQUIRE(std::equal(init_pubkey.begin(), init_pubkey.end(), expected_pk.begin()));

    // Step 6: Responder creates encrypted auth message
    auto resp_auth = responder.create_auth_message();
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
    auto id = chromatin::identity::NodeIdentity::generate();

    HandshakeInitiator initiator(id);
    HandshakeResponder responder(id);

    auto kem_pk_msg = initiator.start();
    auto [resp_err, kem_ct_msg] = responder.receive_kem_pubkey(kem_pk_msg);
    REQUIRE(resp_err == HandshakeError::Success);

    auto init_err = initiator.receive_kem_ciphertext(kem_ct_msg);
    REQUIRE(init_err == HandshakeError::Success);

    auto init_auth = initiator.create_auth_message();
    auto [resp_auth_err, _pk1] = responder.verify_peer_auth(init_auth);
    REQUIRE(resp_auth_err == HandshakeError::Success);

    auto resp_auth = responder.create_auth_message();
    auto [init_auth_err, _pk2] = initiator.verify_peer_auth(resp_auth);
    REQUIRE(init_auth_err == HandshakeError::Success);
}

TEST_CASE("Handshake auth failure with wrong identity", "[handshake]") {
    auto initiator_id = chromatin::identity::NodeIdentity::generate();
    auto responder_id = chromatin::identity::NodeIdentity::generate();
    auto imposter_id = chromatin::identity::NodeIdentity::generate();

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
        auto init_auth = initiator.create_auth_message();

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
        auto id = chromatin::identity::NodeIdentity::generate();
        HandshakeResponder responder(id);

        // Send a Ping message instead of KemPubkey
        std::span<const uint8_t> empty{};
        auto wrong_msg = TransportCodec::encode(
            chromatin::wire::TransportMsgType_Ping, empty);
        auto [err, _] = responder.receive_kem_pubkey(wrong_msg);
        REQUIRE(err == HandshakeError::InvalidMessage);
    }

    SECTION("responder rejects wrong payload size") {
        auto id = chromatin::identity::NodeIdentity::generate();
        HandshakeResponder responder(id);

        // Send KemPubkey with wrong size payload
        std::vector<uint8_t> bad_payload(100, 0x42);
        auto bad_msg = TransportCodec::encode(
            chromatin::wire::TransportMsgType_KemPubkey, bad_payload);
        auto [err, _] = responder.receive_kem_pubkey(bad_msg);
        REQUIRE(err == HandshakeError::InvalidMessage);
    }

    SECTION("initiator rejects wrong message type for ciphertext") {
        auto init_id = chromatin::identity::NodeIdentity::generate();
        HandshakeInitiator initiator(init_id);
        initiator.start(); // Generate KEM keypair

        std::span<const uint8_t> empty{};
        auto wrong_msg = TransportCodec::encode(
            chromatin::wire::TransportMsgType_Ping, empty);
        auto err = initiator.receive_kem_ciphertext(wrong_msg);
        REQUIRE(err == HandshakeError::InvalidMessage);
    }
}
