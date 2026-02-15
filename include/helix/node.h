/*
 * Helix - Node
 *
 * Top-level node object. Owns and coordinates all components:
 * storage, DHT, API server, and relay.
 */

#pragma once

#include "helix/config.h"

#include <memory>

namespace helix {

/* Forward declarations */
class Storage;
class Dht;
class Api;

class Node {
public:
    explicit Node(const Config &config);
    ~Node();

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    bool start();
    void stop();

private:
    Config config_;
    std::unique_ptr<Storage> storage_;
    std::unique_ptr<Dht> dht_;
    std::unique_ptr<Api> api_;
};

} // namespace helix
