#pragma once

#include <cstdint>
#include <string_view>

namespace chromatindb::peer {

// Sync rejection reason bytes (SyncRejected payload).
// Shared between sender (PeerManager responder) and receiver (initiator log).
constexpr uint8_t SYNC_REJECT_COOLDOWN            = 0x01;
constexpr uint8_t SYNC_REJECT_SESSION_LIMIT        = 0x02;
constexpr uint8_t SYNC_REJECT_BYTE_RATE            = 0x03;
constexpr uint8_t SYNC_REJECT_STORAGE_FULL         = 0x04;
constexpr uint8_t SYNC_REJECT_QUOTA_EXCEEDED       = 0x05;
constexpr uint8_t SYNC_REJECT_NAMESPACE_NOT_FOUND  = 0x06;
constexpr uint8_t SYNC_REJECT_BLOB_TOO_LARGE       = 0x07;
constexpr uint8_t SYNC_REJECT_TIMESTAMP_REJECTED   = 0x08;

/// Map a rejection reason byte to a human-readable string.
/// Returns "unknown" for unrecognized values.
constexpr std::string_view sync_reject_reason_string(uint8_t reason) {
    switch (reason) {
        case SYNC_REJECT_COOLDOWN:           return "cooldown";
        case SYNC_REJECT_SESSION_LIMIT:      return "session_limit";
        case SYNC_REJECT_BYTE_RATE:          return "byte_rate";
        case SYNC_REJECT_STORAGE_FULL:       return "storage_full";
        case SYNC_REJECT_QUOTA_EXCEEDED:     return "quota_exceeded";
        case SYNC_REJECT_NAMESPACE_NOT_FOUND: return "namespace_not_found";
        case SYNC_REJECT_BLOB_TOO_LARGE:     return "blob_too_large";
        case SYNC_REJECT_TIMESTAMP_REJECTED: return "timestamp_rejected";
        default:                             return "unknown";
    }
}

} // namespace chromatindb::peer
