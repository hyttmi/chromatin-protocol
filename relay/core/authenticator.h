#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace chromatindb::relay::core {

struct AuthResult {
    bool success = false;
    std::string error_code;  // "invalid_signature", "unknown_key", "bad_pubkey_size", "bad_signature_size"
    std::array<uint8_t, 32> namespace_hash{};
    std::vector<uint8_t> public_key;
};

// Hash for std::array<uint8_t, 32> in unordered_set
struct ArrayHash32 {
    size_t operator()(const std::array<uint8_t, 32>& a) const noexcept {
        size_t h = 0;
        // Use first 8 bytes as hash (namespace hashes are already crypto hashes)
        std::memcpy(&h, a.data(), sizeof(h));
        return h;
    }
};

class Authenticator {
public:
    using KeySet = std::unordered_set<std::array<uint8_t, 32>, ArrayHash32>;

    /// Construct with optional ACL. Empty set = open relay (per D-07).
    explicit Authenticator(KeySet allowed_keys = {});

    /// Generate a 32-byte random challenge via OpenSSL RAND_bytes (per D-01).
    std::array<uint8_t, 32> generate_challenge();

    /// Verify client's challenge_response. CPU-heavy -- offloaded to thread pool in http_router.cpp.
    /// Validation order (Step 0 pattern -- cheapest first):
    ///   1. pubkey.size() == 2592
    ///   2. signature.size() == 4627
    ///   3. OQS_SIG_verify(challenge, signature, pubkey)
    ///   4. SHA3-256(pubkey) -> namespace_hash
    ///   5. If ACL non-empty: namespace_hash in allowed set (per D-07)
    AuthResult verify(
        std::span<const uint8_t, 32> challenge,
        std::span<const uint8_t> pubkey,
        std::span<const uint8_t> signature);

    /// Reload allowed keys on SIGHUP (per D-07, D-34).
    /// Called from event loop thread (single-threaded model).
    void reload_allowed_keys(KeySet new_keys);

    /// Check if ACL is active (non-empty allowed keys).
    bool has_acl() const;

private:
    KeySet allowed_keys_;
};

} // namespace chromatindb::relay::core
