#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::cli::envelope {

/// Encrypt plaintext for one or more recipients.
/// Each recipient is an ML-KEM-1024 public key (1568 bytes).
/// Returns the full envelope binary.
std::vector<uint8_t> encrypt(
    std::span<const uint8_t> plaintext,
    std::vector<std::span<const uint8_t>> recipient_kem_pubkeys);

/// Decrypt an envelope using our KEM secret key.
/// Returns plaintext on success, nullopt if not a recipient or decryption fails.
std::optional<std::vector<uint8_t>> decrypt(
    std::span<const uint8_t> envelope_data,
    std::span<const uint8_t> our_kem_seckey,
    std::span<const uint8_t> our_kem_pubkey);

/// Check if data starts with envelope magic "CENV" (0x43454E56).
bool is_envelope(std::span<const uint8_t> data);

} // namespace chromatindb::cli::envelope
