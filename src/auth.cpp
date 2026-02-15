/*
 * Helix - Authentication (Dilithium5 challenge-response)
 */

#include "helix/auth.h"
#include "helix/log.h"

namespace helix {

struct Auth::Impl {
    /* TODO: Pending challenges, active sessions */
};

Auth::Auth(Storage &storage)
    : storage_(storage)
    , impl_(std::make_unique<Impl>())
{
}

Auth::~Auth() = default;

std::string Auth::create_challenge()
{
    /* TODO: Generate random nonce, store as pending */
    return {};
}

std::string Auth::verify_challenge(const std::string &fingerprint,
                                   const std::string &nonce,
                                   const uint8_t *signature, size_t sig_len)
{
    (void)fingerprint; (void)nonce; (void)signature; (void)sig_len;
    /* TODO: Verify Dilithium5 signature, issue session token */
    return {};
}

std::string Auth::validate_session(const std::string &token)
{
    (void)token;
    /* TODO: Look up session, return fingerprint */
    return {};
}

} // namespace helix
