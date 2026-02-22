#include <gtest/gtest.h>

#include "crypto/aead.h"
#include "crypto/crypto.h"
#include "crypto/kem.h"
#include "kademlia/tcp_encryption.h"

namespace crypto = chromatin::crypto;
namespace enc = chromatin::kademlia::tcp_encryption;

class TcpEncryptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate signing keypairs for both nodes
        initiator_signing_kp_ = crypto::generate_keypair();
        responder_signing_kp_ = crypto::generate_keypair();

        // Derive node IDs from public keys
        initiator_id_.id = crypto::sha3_256(initiator_signing_kp_.public_key);
        responder_id_.id = crypto::sha3_256(responder_signing_kp_.public_key);
    }

    crypto::KeyPair initiator_signing_kp_;
    crypto::KeyPair responder_signing_kp_;
    chromatin::kademlia::NodeId initiator_id_;
    chromatin::kademlia::NodeId responder_id_;
};

TEST_F(TcpEncryptionTest, FullHandshakeRoundtrip) {
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    // Step 1: Initiator generates HELLO (with embedded signing pubkey)
    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    ASSERT_FALSE(hello.empty());
    EXPECT_EQ(hello[0], enc::PROBE_BYTE);

    // Step 2: Responder processes HELLO (extracts + verifies initiator pubkey)
    auto got_initiator_id = responder.process_hello(hello);
    ASSERT_TRUE(got_initiator_id.has_value());
    EXPECT_EQ(got_initiator_id->id, initiator_id_.id);

    // Step 3: Responder generates ACCEPT (with embedded signing pubkey)
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    ASSERT_TRUE(accept.has_value());

    // Step 4: Initiator processes ACCEPT (extracts + verifies responder pubkey)
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    ASSERT_TRUE(confirm.has_value());

    // Step 5: Responder processes CONFIRM (uses stored initiator pubkey)
    bool ok = responder.process_confirm(*confirm);
    EXPECT_TRUE(ok);

    // Both sides should have valid session keys
    auto i_keys = initiator.session_keys();
    auto r_keys = responder.session_keys();
    ASSERT_TRUE(i_keys.has_value());
    ASSERT_TRUE(r_keys.has_value());

    // Initiator's send key == Responder's recv key (i2r)
    EXPECT_EQ(i_keys->send_key, r_keys->recv_key);
    // Initiator's recv key == Responder's send key (r2i)
    EXPECT_EQ(i_keys->recv_key, r_keys->send_key);
    // Directional keys should differ
    EXPECT_NE(i_keys->send_key, i_keys->recv_key);
}

TEST_F(TcpEncryptionTest, EncryptedFrameRoundtrip) {
    // Complete handshake first
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    responder.process_confirm(*confirm);

    auto i_keys = initiator.session_keys();
    auto r_keys = responder.session_keys();
    ASSERT_TRUE(i_keys.has_value());
    ASSERT_TRUE(r_keys.has_value());

    // Initiator encrypts a message
    std::vector<uint8_t> plaintext = {0x43, 0x48, 0x52, 0x4D, 0x01};  // "CHRM" + version
    auto frame = enc::encrypt_frame(*i_keys, plaintext);

    // Frame should have 4-byte length header + ciphertext
    ASSERT_GT(frame.size(), 4u + crypto::AEAD_TAG_SIZE);

    // Extract length header and ciphertext
    std::span<const uint8_t> len_header(frame.data(), 4);
    std::span<const uint8_t> ciphertext(frame.data() + 4, frame.size() - 4);

    // Responder decrypts
    auto decrypted = enc::decrypt_frame(*r_keys, ciphertext, len_header);
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(*decrypted, plaintext);
}

TEST_F(TcpEncryptionTest, BidirectionalEncryption) {
    // Complete handshake
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    responder.process_confirm(*confirm);

    auto i_keys = initiator.session_keys();
    auto r_keys = responder.session_keys();

    // Initiator → Responder
    std::vector<uint8_t> msg1 = {0x01, 0x02, 0x03};
    auto frame1 = enc::encrypt_frame(*i_keys, msg1);
    auto dec1 = enc::decrypt_frame(*r_keys,
                                    std::span(frame1.data() + 4, frame1.size() - 4),
                                    std::span(frame1.data(), 4));
    ASSERT_TRUE(dec1.has_value());
    EXPECT_EQ(*dec1, msg1);

    // Responder → Initiator
    std::vector<uint8_t> msg2 = {0x04, 0x05, 0x06};
    auto frame2 = enc::encrypt_frame(*r_keys, msg2);
    auto dec2 = enc::decrypt_frame(*i_keys,
                                    std::span(frame2.data() + 4, frame2.size() - 4),
                                    std::span(frame2.data(), 4));
    ASSERT_TRUE(dec2.has_value());
    EXPECT_EQ(*dec2, msg2);
}

TEST_F(TcpEncryptionTest, NonceIncrements) {
    // Complete handshake
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    responder.process_confirm(*confirm);

    auto i_keys = initiator.session_keys();
    auto r_keys = responder.session_keys();

    EXPECT_EQ(i_keys->send_nonce, 0u);
    EXPECT_EQ(r_keys->recv_nonce, 0u);

    // Send 3 messages, verify nonce increments
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> msg = {static_cast<uint8_t>(i)};
        auto frame = enc::encrypt_frame(*i_keys, msg);
        auto dec = enc::decrypt_frame(*r_keys,
                                       std::span(frame.data() + 4, frame.size() - 4),
                                       std::span(frame.data(), 4));
        ASSERT_TRUE(dec.has_value());
        EXPECT_EQ(*dec, msg);
    }

    EXPECT_EQ(i_keys->send_nonce, 3u);
    EXPECT_EQ(r_keys->recv_nonce, 3u);
}

TEST_F(TcpEncryptionTest, TamperedAcceptSignatureFails) {
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    ASSERT_TRUE(accept.has_value());

    // Tamper with the accept bytes (node_id region)
    (*accept)[10] ^= 0xFF;

    // Initiator should reject tampered ACCEPT
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    EXPECT_FALSE(confirm.has_value());
}

TEST_F(TcpEncryptionTest, WrongSigningPubkeyInAcceptFails) {
    // Generate a third identity to use as a wrong pubkey
    auto wrong_kp = crypto::generate_keypair();

    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);

    // Responder embeds wrong signing pubkey (doesn't match its node ID)
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             wrong_kp.public_key);
    ASSERT_TRUE(accept.has_value());

    // Initiator should reject: SHA3-256(wrong_pk) != responder_node_id
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    EXPECT_FALSE(confirm.has_value());
}

TEST_F(TcpEncryptionTest, TamperedConfirmFails) {
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    ASSERT_TRUE(confirm.has_value());

    // Tamper with the confirm
    (*confirm)[5] ^= 0xFF;

    bool ok = responder.process_confirm(*confirm);
    EXPECT_FALSE(ok);
}

TEST_F(TcpEncryptionTest, TamperedEncryptedFrameFails) {
    // Complete handshake
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    responder.process_confirm(*confirm);

    auto i_keys = initiator.session_keys();
    auto r_keys = responder.session_keys();

    // Encrypt a message
    std::vector<uint8_t> msg = {0x01, 0x02, 0x03};
    auto frame = enc::encrypt_frame(*i_keys, msg);

    // Tamper with ciphertext payload
    frame[6] ^= 0xFF;

    auto dec = enc::decrypt_frame(*r_keys,
                                   std::span(frame.data() + 4, frame.size() - 4),
                                   std::span(frame.data(), 4));
    EXPECT_FALSE(dec.has_value());
}

TEST_F(TcpEncryptionTest, ReplayDetectionViaNonce) {
    // Complete handshake
    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    auto hello = initiator.generate_hello(initiator_signing_kp_.public_key);
    responder.process_hello(hello);
    auto accept = responder.generate_accept(responder_signing_kp_.secret_key,
                                             responder_signing_kp_.public_key);
    auto confirm = initiator.process_accept(*accept,
                                             initiator_signing_kp_.secret_key);
    responder.process_confirm(*confirm);

    auto i_keys = initiator.session_keys();
    auto r_keys = responder.session_keys();

    // Send and receive a message normally
    std::vector<uint8_t> msg = {0x01, 0x02, 0x03};
    auto frame = enc::encrypt_frame(*i_keys, msg);
    auto dec = enc::decrypt_frame(*r_keys,
                                   std::span(frame.data() + 4, frame.size() - 4),
                                   std::span(frame.data(), 4));
    ASSERT_TRUE(dec.has_value());

    // Replay the same frame — should fail because nonce has advanced
    auto replay = enc::decrypt_frame(*r_keys,
                                      std::span(frame.data() + 4, frame.size() - 4),
                                      std::span(frame.data(), 4));
    EXPECT_FALSE(replay.has_value());
}

TEST_F(TcpEncryptionTest, WrongSigningPubkeyInHelloFails) {
    // Initiator embeds wrong signing pubkey (doesn't match its node ID)
    auto wrong_kp = crypto::generate_keypair();

    enc::HandshakeInitiator initiator(initiator_id_);
    enc::HandshakeResponder responder(responder_id_);

    // Embed wrong pubkey in HELLO — SHA3-256(wrong_pk) != initiator_node_id
    auto hello = initiator.generate_hello(wrong_kp.public_key);
    ASSERT_FALSE(hello.empty());

    // Responder should reject: identity verification fails
    auto got_id = responder.process_hello(hello);
    EXPECT_FALSE(got_id.has_value());
}
