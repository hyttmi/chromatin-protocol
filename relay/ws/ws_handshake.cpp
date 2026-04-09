#include "relay/ws/ws_handshake.h"

namespace chromatindb::relay::ws {

std::string compute_accept_key(std::string_view /*client_key*/) {
    return {};  // Stub
}

ParseResult parse_upgrade_request(std::string_view /*raw_request*/) {
    return {false, std::nullopt, ""};  // Stub
}

std::string build_upgrade_response(const std::string& /*accept_key*/) {
    return {};  // Stub
}

std::string build_426_response() {
    return {};  // Stub
}

} // namespace chromatindb::relay::ws
