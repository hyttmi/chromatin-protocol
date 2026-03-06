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

AccessControl::AccessControl(const std::vector<std::string>& hex_keys,
                             std::span<const uint8_t, 32> own_namespace) {
    std::copy(own_namespace.begin(), own_namespace.end(), own_namespace_.begin());

    configured_count_ = hex_keys.size();
    for (const auto& hex : hex_keys) {
        allowed_keys_.insert(hex_to_bytes(hex));
    }

    // Always allow own namespace (even if not listed)
    if (configured_count_ > 0) {
        allowed_keys_.insert(own_namespace_);
    }
}

bool AccessControl::is_allowed(std::span<const uint8_t, 32> namespace_hash) const {
    if (!is_closed_mode()) {
        return true;  // Open mode: allow all
    }

    NamespaceHash key;
    std::copy(namespace_hash.begin(), namespace_hash.end(), key.begin());
    return allowed_keys_.count(key) > 0;
}

bool AccessControl::is_closed_mode() const {
    return configured_count_ > 0;
}

size_t AccessControl::allowed_count() const {
    return configured_count_;
}

AccessControl::ReloadResult AccessControl::reload(const std::vector<std::string>& hex_keys) {
    // Build new set (without own namespace for accurate diff)
    std::set<NamespaceHash> new_keys;
    for (const auto& hex : hex_keys) {
        new_keys.insert(hex_to_bytes(hex));
    }

    // Build old configured set (without own namespace)
    std::set<NamespaceHash> old_keys;
    for (const auto& key : allowed_keys_) {
        if (key != own_namespace_) {
            old_keys.insert(key);
        }
    }

    // Compute diff
    ReloadResult result;
    for (const auto& key : new_keys) {
        if (old_keys.count(key) == 0) {
            result.added++;
        }
    }
    for (const auto& key : old_keys) {
        if (new_keys.count(key) == 0) {
            result.removed++;
        }
    }

    // Swap
    allowed_keys_ = std::move(new_keys);
    configured_count_ = hex_keys.size();

    // Re-add own namespace if in closed mode
    if (configured_count_ > 0) {
        allowed_keys_.insert(own_namespace_);
    }

    return result;
}

} // namespace chromatindb::acl
