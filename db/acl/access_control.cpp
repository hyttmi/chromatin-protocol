#include "db/acl/access_control.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace chromatindb::acl {

AccessControl::NamespaceHash AccessControl::hex_to_bytes(const std::string& hex) {
    NamespaceHash result{};
    for (size_t i = 0; i < 32; ++i) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];

        auto hex_val = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;  // Already validated by validate_allowed_keys
        };

        result[i] = static_cast<uint8_t>((hex_val(hi) << 4) | hex_val(lo));
    }
    return result;
}

AccessControl::AccessControl(const std::vector<std::string>& client_hex_keys,
                             const std::vector<std::string>& peer_hex_keys,
                             std::span<const uint8_t, 32> own_namespace) {
    std::copy(own_namespace.begin(), own_namespace.end(), own_namespace_.begin());

    client_configured_count_ = client_hex_keys.size();
    for (const auto& hex : client_hex_keys) {
        allowed_client_keys_.insert(hex_to_bytes(hex));
    }
    if (client_configured_count_ > 0) {
        allowed_client_keys_.insert(own_namespace_);
    }

    peer_configured_count_ = peer_hex_keys.size();
    for (const auto& hex : peer_hex_keys) {
        allowed_peer_keys_.insert(hex_to_bytes(hex));
    }
    if (peer_configured_count_ > 0) {
        allowed_peer_keys_.insert(own_namespace_);
    }
}

bool AccessControl::is_client_allowed(std::span<const uint8_t, 32> namespace_hash) const {
    if (!is_client_closed_mode()) {
        return true;  // Open mode: allow all clients
    }

    NamespaceHash key;
    std::copy(namespace_hash.begin(), namespace_hash.end(), key.begin());
    return allowed_client_keys_.count(key) > 0;
}

bool AccessControl::is_peer_allowed(std::span<const uint8_t, 32> namespace_hash) const {
    if (!is_peer_closed_mode()) {
        return true;  // Open mode: allow all peers
    }

    NamespaceHash key;
    std::copy(namespace_hash.begin(), namespace_hash.end(), key.begin());
    return allowed_peer_keys_.count(key) > 0;
}

bool AccessControl::is_client_closed_mode() const {
    return client_configured_count_ > 0;
}

bool AccessControl::is_peer_closed_mode() const {
    return peer_configured_count_ > 0;
}

size_t AccessControl::client_allowed_count() const {
    return client_configured_count_;
}

size_t AccessControl::peer_allowed_count() const {
    return peer_configured_count_;
}

AccessControl::ReloadResult AccessControl::reload(const std::vector<std::string>& client_hex_keys,
                                                   const std::vector<std::string>& peer_hex_keys) {
    ReloadResult result;

    // --- Client keys ---
    {
        std::set<NamespaceHash> new_keys;
        for (const auto& hex : client_hex_keys) {
            new_keys.insert(hex_to_bytes(hex));
        }

        std::set<NamespaceHash> old_keys;
        for (const auto& key : allowed_client_keys_) {
            if (key != own_namespace_) {
                old_keys.insert(key);
            }
        }

        for (const auto& key : new_keys) {
            if (old_keys.count(key) == 0) {
                result.client_added++;
            }
        }
        for (const auto& key : old_keys) {
            if (new_keys.count(key) == 0) {
                result.client_removed++;
            }
        }

        allowed_client_keys_ = std::move(new_keys);
        client_configured_count_ = client_hex_keys.size();
        if (client_configured_count_ > 0) {
            allowed_client_keys_.insert(own_namespace_);
        }
    }

    // --- Peer keys ---
    {
        std::set<NamespaceHash> new_keys;
        for (const auto& hex : peer_hex_keys) {
            new_keys.insert(hex_to_bytes(hex));
        }

        std::set<NamespaceHash> old_keys;
        for (const auto& key : allowed_peer_keys_) {
            if (key != own_namespace_) {
                old_keys.insert(key);
            }
        }

        for (const auto& key : new_keys) {
            if (old_keys.count(key) == 0) {
                result.peer_added++;
            }
        }
        for (const auto& key : old_keys) {
            if (new_keys.count(key) == 0) {
                result.peer_removed++;
            }
        }

        allowed_peer_keys_ = std::move(new_keys);
        peer_configured_count_ = peer_hex_keys.size();
        if (peer_configured_count_ > 0) {
            allowed_peer_keys_.insert(own_namespace_);
        }
    }

    return result;
}

} // namespace chromatindb::acl
