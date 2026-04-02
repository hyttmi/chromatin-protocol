#include "relay/core/message_filter.h"

using chromatindb::wire::TransportMsgType;
using namespace chromatindb::wire;

namespace chromatindb::relay::core {

bool is_client_allowed(TransportMsgType type) {
    switch (type) {
        // Invalid
        case TransportMsgType_None:
        // Handshake types
        case TransportMsgType_KemPubkey:
        case TransportMsgType_KemCiphertext:
        case TransportMsgType_AuthSignature:
        case TransportMsgType_AuthPubkey:
        case TransportMsgType_TrustedHello:
        case TransportMsgType_PQRequired:
        // Peer sync types
        case TransportMsgType_SyncRequest:
        case TransportMsgType_SyncAccept:
        case TransportMsgType_SyncComplete:
        case TransportMsgType_SyncRejected:
        case TransportMsgType_NamespaceList:
        case TransportMsgType_BlobRequest:
        case TransportMsgType_BlobTransfer:
        case TransportMsgType_ReconcileInit:
        case TransportMsgType_ReconcileRanges:
        case TransportMsgType_ReconcileItems:
        // Peer exchange
        case TransportMsgType_PeerListRequest:
        case TransportMsgType_PeerListResponse:
        // Internal signals
        case TransportMsgType_StorageFull:
        case TransportMsgType_QuotaExceeded:
        // Push notifications (peer-internal)
        case TransportMsgType_BlobNotify:
        // Targeted blob fetch (peer-internal)
        case TransportMsgType_BlobFetch:
        case TransportMsgType_BlobFetchResponse:
            return false;
        // All other types (client operations, queries, pub/sub, keepalive)
        // pass through — new message types work without relay changes.
        default:
            return true;
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
