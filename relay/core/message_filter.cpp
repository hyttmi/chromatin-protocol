#include "relay/core/message_filter.h"

namespace chromatindb::relay::core {

bool is_client_allowed(chromatindb::wire::TransportMsgType /*type*/) {
    // Stub: always returns false (tests will fail)
    return false;
}

std::string type_name(chromatindb::wire::TransportMsgType /*type*/) {
    // Stub: always returns empty string (tests will fail)
    return "";
}

} // namespace chromatindb::relay::core
