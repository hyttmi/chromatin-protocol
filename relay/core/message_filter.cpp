#include "relay/core/message_filter.h"

using chromatindb::wire::TransportMsgType;
using namespace chromatindb::wire;

namespace chromatindb::relay::core {

bool is_client_allowed(TransportMsgType type) {
    switch (type) {
        // Client data operations
        case TransportMsgType_Data:
        case TransportMsgType_WriteAck:
        case TransportMsgType_Delete:
        case TransportMsgType_DeleteAck:
        case TransportMsgType_ReadRequest:
        case TransportMsgType_ReadResponse:
        case TransportMsgType_ListRequest:
        case TransportMsgType_ListResponse:
        case TransportMsgType_StatsRequest:
        case TransportMsgType_StatsResponse:
        // Query extensions
        case TransportMsgType_ExistsRequest:
        case TransportMsgType_ExistsResponse:
        // Pub/sub
        case TransportMsgType_Subscribe:
        case TransportMsgType_Unsubscribe:
        case TransportMsgType_Notification:
        // Keepalive and disconnect
        case TransportMsgType_Ping:
        case TransportMsgType_Pong:
        case TransportMsgType_Goodbye:
            return true;
        // Default-deny: peer-only, handshake, internal, and unknown types
        default:
            return false;
    }
}

std::string type_name(TransportMsgType type) {
    const char* name = EnumNameTransportMsgType(type);
    if (name != nullptr && name[0] != '\0') {
        return std::string(name);
    }
    return "Unknown(" + std::to_string(static_cast<int>(type)) + ")";
}

} // namespace chromatindb::relay::core
