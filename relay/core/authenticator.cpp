#include "relay/core/authenticator.h"

#include <oqs/oqs.h>
#include <oqs/sha3.h>
#include <openssl/rand.h>

#include <stdexcept>

namespace chromatindb::relay::core {

// ML-DSA-87 key and signature sizes (must match relay_identity.h)
static constexpr size_t ML_DSA_87_PUBLIC_KEY_SIZE = 2592;
static constexpr size_t ML_DSA_87_SIGNATURE_SIZE = 4627;

Authenticator::Authenticator(KeySet allowed_keys)
    : allowed_keys_(std::move(allowed_keys)) {}

std::array<uint8_t, 32> Authenticator::generate_challenge() {
    std::array<uint8_t, 32> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed to generate challenge nonce");
    }
    return nonce;
}

AuthResult Authenticator::verify(
    std::span<const uint8_t, 32> challenge,
    std::span<const uint8_t> pubkey,
    std::span<const uint8_t> signature) {

    // Step 0: cheapest validation first (int compare before crypto)
    if (pubkey.size() != ML_DSA_87_PUBLIC_KEY_SIZE) {
        return {false, "bad_pubkey_size", {}, {}};
    }

    if (signature.size() != ML_DSA_87_SIGNATURE_SIZE) {
        return {false, "bad_signature_size", {}, {}};
    }

    // Create ML-DSA-87 verifier
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) {
        return {false, "verify_failed", {}, {}};
    }

    // Verify signature over the 32-byte challenge
    OQS_STATUS rc = OQS_SIG_verify(
        sig,
        challenge.data(), challenge.size(),
        signature.data(), signature.size(),
        pubkey.data());

    OQS_SIG_free(sig);

    if (rc != OQS_SUCCESS) {
        return {false, "invalid_signature", {}, {}};
    }

    // Compute namespace hash: SHA3-256(pubkey)
    std::array<uint8_t, 32> ns_hash{};
    OQS_SHA3_sha3_256(ns_hash.data(), pubkey.data(), pubkey.size());

    // ACL check (per D-07: empty set = open relay)
    {
        std::lock_guard<std::mutex> lock(acl_mutex_);
        if (!allowed_keys_.empty() && allowed_keys_.find(ns_hash) == allowed_keys_.end()) {
            return {false, "unknown_key", {}, {}};
        }
    }

    return {
        true,
        "",
        ns_hash,
        std::vector<uint8_t>(pubkey.begin(), pubkey.end())
    };
}

void Authenticator::reload_allowed_keys(KeySet new_keys) {
    std::lock_guard<std::mutex> lock(acl_mutex_);
    allowed_keys_ = std::move(new_keys);
}

bool Authenticator::has_acl() const {
    std::lock_guard<std::mutex> lock(acl_mutex_);
    return !allowed_keys_.empty();
}

} // namespace chromatindb::relay::core
