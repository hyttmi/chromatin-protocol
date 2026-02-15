/*
 * Helix - Relay
 *
 * Node-to-node message relay. Delivers encrypted blobs to remote
 * helix instances for their local users.
 */

#pragma once

#include "helix/storage.h"
#include "helix/dht.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

class Relay {
public:
    Relay(Storage &storage, Dht &dht);
    ~Relay();

    Relay(const Relay &) = delete;
    Relay &operator=(const Relay &) = delete;

    /* Deliver a blob to a fingerprint. Resolves nodus via DHT if needed. */
    using DeliverCallback = std::function<void(bool success, const std::string &error)>;
    void deliver(const std::string &recipient_fp, const std::string &msg_id,
                 const uint8_t *blob, size_t blob_len, DeliverCallback cb);

private:
    Storage &storage_;
    Dht &dht_;
};

} // namespace helix
