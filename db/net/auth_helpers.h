#pragma once

#include "db/crypto/signing.h"
#include "db/net/role.h"
#include "db/util/endian.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::net {

/// Decoded auth payload: role, public key, and signature.
struct AuthPayload {
    Role role;
    std::vector<uint8_t> pubkey;
    std::vector<uint8_t> signature;
};

/// Encode an auth payload as [role:1][pubkey_size (4B BE)][pubkey][signature].
///
/// The role byte is the initiator-declared connection role (peer, client, ...).
/// It rides inside the AEAD-encrypted AuthSignature message, so its integrity
/// is protected by the session keys.
inline std::vector<uint8_t> encode_auth_payload(
    Role role,
    std::span<const uint8_t> signing_pubkey,
    std::span<const uint8_t> signature) {

    std::vector<uint8_t> payload;
    payload.reserve(1 + 4 + signing_pubkey.size() + signature.size());

    // Role byte
    payload.push_back(static_cast<uint8_t>(role));

    // 4-byte BE pubkey size
    uint32_t pk_size = static_cast<uint32_t>(signing_pubkey.size());
    chromatindb::util::write_u32_be(payload, pk_size);

    // Pubkey
    payload.insert(payload.end(), signing_pubkey.begin(), signing_pubkey.end());

    // Signature
    payload.insert(payload.end(), signature.begin(), signature.end());

    return payload;
}

/// Decode an auth payload from [role:1][pubkey_size (4B BE)][pubkey][signature].
/// Returns std::nullopt if the data is too short, malformed, or declares an
/// unknown role. Fail-closed on unknown roles prevents old binaries from
/// misinterpreting future role values.
inline std::optional<AuthPayload> decode_auth_payload(std::span<const uint8_t> data) {
    // Need at least role(1) + pk_size(4)
    if (data.size() < 5) return std::nullopt;

    uint8_t role_byte = data[0];
    if (!is_implemented_role(role_byte)) return std::nullopt;

    uint32_t pk_size = chromatindb::util::read_u32_be(data.subspan(1, 4));

    // Step 0: exact pubkey size check (PROTO-02, per D-05)
    if (pk_size != chromatindb::crypto::Signer::PUBLIC_KEY_SIZE) return std::nullopt;

    // Overflow-safe bounds check: pk_size could be up to UINT32_MAX
    if (pk_size > data.size() - 5) return std::nullopt;

    AuthPayload result;
    result.role = static_cast<Role>(role_byte);
    result.pubkey.assign(data.begin() + 5, data.begin() + 5 + pk_size);
    result.signature.assign(data.begin() + 5 + pk_size, data.end());
    return result;
}

} // namespace chromatindb::net
