#pragma once

#include "db/crypto/signing.h"
#include "db/util/endian.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::net {

/// Decoded auth payload: public key and signature.
struct AuthPayload {
    std::vector<uint8_t> pubkey;
    std::vector<uint8_t> signature;
};

/// Encode an auth payload as [pubkey_size (4B BE)][pubkey][signature].
inline std::vector<uint8_t> encode_auth_payload(
    std::span<const uint8_t> signing_pubkey,
    std::span<const uint8_t> signature) {

    std::vector<uint8_t> payload;
    payload.reserve(4 + signing_pubkey.size() + signature.size());

    // 4-byte BE pubkey size
    uint32_t pk_size = static_cast<uint32_t>(signing_pubkey.size());
    chromatindb::util::write_u32_be(payload, pk_size);

    // Pubkey
    payload.insert(payload.end(), signing_pubkey.begin(), signing_pubkey.end());

    // Signature
    payload.insert(payload.end(), signature.begin(), signature.end());

    return payload;
}

/// Decode an auth payload from [pubkey_size (4B BE)][pubkey][signature].
/// Returns std::nullopt if the data is too short or malformed.
inline std::optional<AuthPayload> decode_auth_payload(std::span<const uint8_t> data) {
    if (data.size() < 4) return std::nullopt;

    uint32_t pk_size = chromatindb::util::read_u32_be(data.subspan(0, 4));

    // Step 0: exact pubkey size check (PROTO-02, per D-05)
    if (pk_size != chromatindb::crypto::Signer::PUBLIC_KEY_SIZE) return std::nullopt;

    // Overflow-safe bounds check: pk_size could be up to UINT32_MAX
    if (pk_size > data.size() - 4) return std::nullopt;

    AuthPayload result;
    result.pubkey.assign(data.begin() + 4, data.begin() + 4 + pk_size);
    result.signature.assign(data.begin() + 4 + pk_size, data.end());
    return result;
}

} // namespace chromatindb::net
