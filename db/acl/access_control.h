#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::acl {

/// Access control list for restricting which clients and peers can connect.
///
/// Client keys (UDS): restricts relay/client connections via Unix domain socket.
///   Empty = no restriction (open mode for clients).
/// Peer keys (TCP): restricts node-to-node connections via TCP.
///   Empty = any peer that completes PQ handshake can sync (open mode for peers).
///
/// The node's own namespace is always implicitly allowed in both lists
/// when that list is non-empty (closed mode).
class AccessControl {
public:
    using NamespaceHash = std::array<uint8_t, 32>;

    struct ReloadResult {
        size_t client_added = 0;
        size_t client_removed = 0;
        size_t peer_added = 0;
        size_t peer_removed = 0;
    };

    /// Construct with separate client and peer key lists.
    /// Empty client_keys = no client restriction on UDS.
    /// Empty peer_keys = any peer that completes PQ handshake can sync.
    AccessControl(const std::vector<std::string>& client_hex_keys,
                  const std::vector<std::string>& peer_hex_keys,
                  std::span<const uint8_t, 32> own_namespace);

    /// Check if a namespace hash is allowed for a client (UDS) connection.
    bool is_client_allowed(std::span<const uint8_t, 32> namespace_hash) const;

    /// Check if a namespace hash is allowed for a peer (TCP) connection.
    bool is_peer_allowed(std::span<const uint8_t, 32> namespace_hash) const;

    /// Whether client access control is active (non-empty allowed_client_keys).
    bool is_client_closed_mode() const;

    /// Whether peer access control is active (non-empty allowed_peer_keys).
    bool is_peer_closed_mode() const;

    /// Number of explicitly configured client keys (excludes implicit self).
    size_t client_allowed_count() const;

    /// Number of explicitly configured peer keys (excludes implicit self).
    size_t peer_allowed_count() const;

    /// Reload with new key lists. Returns diff for logging.
    ReloadResult reload(const std::vector<std::string>& client_hex_keys,
                        const std::vector<std::string>& peer_hex_keys);

private:
    static NamespaceHash hex_to_bytes(const std::string& hex);

    std::set<NamespaceHash> allowed_client_keys_;
    std::set<NamespaceHash> allowed_peer_keys_;
    NamespaceHash own_namespace_;
    size_t client_configured_count_ = 0;
    size_t peer_configured_count_ = 0;
};

} // namespace chromatindb::acl
