#include "kademlia/node_id.h"

#include <cstring>

namespace chromatin::kademlia {

NodeId NodeId::from_pubkey(std::span<const uint8_t> pubkey) {
    NodeId result;
    result.id = crypto::sha3_256(pubkey);
    return result;
}

crypto::Hash NodeId::distance_to(const NodeId& other) const {
    crypto::Hash dist{};
    for (size_t i = 0; i < id.size(); ++i) {
        dist[i] = id[i] ^ other.id[i];
    }
    return dist;
}

size_t NodeIdHash::operator()(const NodeId& n) const {
    size_t result = 0;
    std::memcpy(&result, n.id.data(), sizeof(result));
    return result;
}

} // namespace chromatin::kademlia
