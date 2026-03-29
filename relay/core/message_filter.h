#pragma once

#include "db/wire/transport_generated.h"
#include <string>

namespace chromatindb::relay::core {

/// Check if a message type is allowed through the relay from a client.
/// Blocklist approach: only peer-internal types are blocked. New client-visible
/// message types added to the node pass through without relay changes.
/// Blocked: None, handshake (KemPubkey, KemCiphertext, AuthSignature, AuthPubkey,
///          TrustedHello, PQRequired), sync (SyncRequest, SyncAccept, SyncComplete,
///          SyncRejected, NamespaceList, BlobRequest, BlobTransfer, ReconcileInit,
///          ReconcileRanges, ReconcileItems), PEX (PeerListRequest, PeerListResponse),
///          internal signals (StorageFull, QuotaExceeded).
bool is_client_allowed(chromatindb::wire::TransportMsgType type);

/// Human-readable name for a message type (for logging blocked types per D-07).
/// Returns "Unknown(N)" for unrecognized values.
std::string type_name(chromatindb::wire::TransportMsgType type);

} // namespace chromatindb::relay::core
