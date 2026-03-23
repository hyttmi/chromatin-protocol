#include "relay/config/relay_config.h"

namespace chromatindb::relay::config {

RelayConfig load_relay_config(const std::filesystem::path& /*path*/) {
    // Stub -- will be implemented in GREEN phase
    return RelayConfig{};
}

void validate_relay_config(const RelayConfig& /*cfg*/) {
    // Stub -- will be implemented in GREEN phase
}

} // namespace chromatindb::relay::config
