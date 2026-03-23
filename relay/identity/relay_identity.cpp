#include "relay/identity/relay_identity.h"

namespace chromatindb::relay::identity {

std::filesystem::path pub_path_from_key_path(const std::filesystem::path& /*key_path*/) {
    return {};  // stub
}

RelayIdentity RelayIdentity::generate() {
    return {};  // stub
}

RelayIdentity RelayIdentity::load_from(const std::filesystem::path& /*key_path*/) {
    return {};  // stub
}

RelayIdentity RelayIdentity::load_or_generate(const std::filesystem::path& /*key_path*/) {
    return {};  // stub
}

void RelayIdentity::save_to(const std::filesystem::path& /*key_path*/) const {
    // stub
}

std::span<const uint8_t, 32> RelayIdentity::public_key_hash() const {
    return std::span<const uint8_t, 32>(pk_hash_.data(), 32);
}

std::span<const uint8_t> RelayIdentity::public_key() const {
    return signer_.export_public_key();
}

std::vector<uint8_t> RelayIdentity::sign(std::span<const uint8_t> message) const {
    return signer_.sign(message);
}

} // namespace chromatindb::relay::identity
