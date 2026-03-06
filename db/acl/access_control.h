#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::acl {

/// Access control list for restricting which peers can connect.
///
/// Open mode (empty allowed_keys): all peers allowed.
/// Closed mode (non-empty allowed_keys): only listed namespace hashes allowed.
/// The node's own namespace is always implicitly allowed.
class AccessControl {
public:
    using NamespaceHash = std::array<uint8_t, 32>;

    struct ReloadResult {
        size_t added = 0;
        size_t removed = 0;
    };

    /// Construct with allowed keys (hex strings, already validated) and own namespace.
    /// Empty keys = open mode.
    AccessControl(const std::vector<std::string>& hex_keys,
                  std::span<const uint8_t, 32> own_namespace);

    /// Check if a namespace hash is allowed to connect.
    bool is_allowed(std::span<const uint8_t, 32> namespace_hash) const;

    /// Whether access control is active (non-empty allowed_keys).
    bool is_closed_mode() const;

    /// Number of explicitly configured allowed keys (excludes implicit self).
    size_t allowed_count() const;

    /// Reload with new key list. Returns diff for logging.
    ReloadResult reload(const std::vector<std::string>& hex_keys);

private:
    static NamespaceHash hex_to_bytes(const std::string& hex);

    std::set<NamespaceHash> allowed_keys_;
    NamespaceHash own_namespace_;
    size_t configured_count_ = 0;
};

} // namespace chromatindb::acl
