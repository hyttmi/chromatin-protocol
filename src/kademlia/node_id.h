#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "crypto/crypto.h"

namespace chromatin::kademlia {

struct NodeId {
    crypto::Hash id{};

    static NodeId from_pubkey(std::span<const uint8_t> pubkey);
    crypto::Hash distance_to(const NodeId& other) const;

    bool operator==(const NodeId& other) const = default;
    auto operator<=>(const NodeId& other) const = default;
};

struct NodeIdHash {
    size_t operator()(const NodeId& n) const;
};

} // namespace chromatin::kademlia
