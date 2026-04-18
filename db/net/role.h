#pragma once

#include <cstdint>

namespace chromatindb::net {

/// Connection role, declared by the initiator in the AuthSignature payload.
///
/// The role is carried over the AEAD-encrypted channel after session-key
/// derivation, so its integrity is protected by the session keys and it
/// cannot be modified in flight.
///
/// Receivers MUST reject unknown role values (fail-closed) so future roles
/// are never misinterpreted by old binaries.
///
/// Reserved values exist for planned roles that haven't shipped yet. Leaving
/// holes now avoids another protocol bump later.
enum class Role : uint8_t {
    Peer     = 0x00,  ///< Full node-to-node replication (sync, PEX, dedup).
    Client   = 0x01,  ///< Read/write API access (blobs, queries, subscriptions).
    Observer = 0x02,  ///< (reserved) Read-only — metrics, backup, auditors.
    Admin    = 0x03,  ///< (reserved) Privileged CLI — config reload, revoke.
    Relay    = 0x04,  ///< (reserved) Bridge/relay node.
    // 0x05..0xFE reserved for future roles.
    // 0xFF reserved as a sentinel / error value.
};

/// True if the byte matches a currently-implemented role.
/// Reserved-but-unimplemented roles return false — receivers should reject
/// them until they're explicitly wired up.
inline bool is_implemented_role(uint8_t byte) {
    switch (static_cast<Role>(byte)) {
        case Role::Peer:
        case Role::Client:
            return true;
        default:
            return false;
    }
}

/// Short human-readable name for logging.
inline const char* role_name(Role r) {
    switch (r) {
        case Role::Peer:     return "peer";
        case Role::Client:   return "client";
        case Role::Observer: return "observer";
        case Role::Admin:    return "admin";
        case Role::Relay:    return "relay";
    }
    return "unknown";
}

} // namespace chromatindb::net
