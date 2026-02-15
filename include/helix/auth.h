/*
 * Helix - Authentication
 *
 * Dilithium5 challenge-response authentication.
 * Issues session tokens after successful auth.
 */

#pragma once

#include "helix/storage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix {

class Auth {
public:
    explicit Auth(Storage &storage);
    ~Auth();

    Auth(const Auth &) = delete;
    Auth &operator=(const Auth &) = delete;

    /* Generate a random challenge nonce */
    std::string create_challenge();

    /* Verify a signed challenge. Returns session token on success, empty on failure. */
    std::string verify_challenge(const std::string &fingerprint,
                                 const std::string &nonce,
                                 const uint8_t *signature, size_t sig_len);

    /* Validate a session token. Returns fingerprint if valid, empty if not. */
    std::string validate_session(const std::string &token);

private:
    Storage &storage_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace helix
