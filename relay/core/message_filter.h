#pragma once

#include "db/wire/transport_generated.h"
#include <string>

namespace chromatindb::relay::core {

/// Check if a message type is allowed through the relay from a client.
/// 38 client-allowed types:
/// Allowed: Data, WriteAck, Delete, DeleteAck, ReadRequest, ReadResponse,
///          ListRequest, ListResponse, StatsRequest, StatsResponse,
///          ExistsRequest, ExistsResponse, NodeInfoRequest, NodeInfoResponse,
///          NamespaceListRequest, NamespaceListResponse,
///          StorageStatusRequest, StorageStatusResponse,
///          NamespaceStatsRequest, NamespaceStatsResponse,
///          MetadataRequest, MetadataResponse,
///          BatchExistsRequest, BatchExistsResponse,
///          DelegationListRequest, DelegationListResponse,
///          BatchReadRequest, BatchReadResponse,
///          PeerInfoRequest, PeerInfoResponse,
///          TimeRangeRequest, TimeRangeResponse,
///          Subscribe, Unsubscribe, Notification, Ping, Pong, Goodbye
/// Blocked: All peer-only types, handshake types, None, unknown types.
/// Per RELAY-03: default-deny on unknown types.
bool is_client_allowed(chromatindb::wire::TransportMsgType type);

/// Human-readable name for a message type (for logging blocked types per D-07).
/// Returns "Unknown(N)" for unrecognized values.
std::string type_name(chromatindb::wire::TransportMsgType type);

} // namespace chromatindb::relay::core
