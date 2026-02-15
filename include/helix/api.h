/*
 * Helix - REST/WebSocket API
 *
 * Boost.Beast HTTP/WebSocket server.
 * Handles client requests and node-to-node relay.
 */

#pragma once

#include "helix/config.h"

#include <memory>

namespace helix {

class Storage;
class Dht;
class Auth;

class Api {
public:
    Api(const Config &config, Storage &storage, Dht &dht);
    ~Api();

    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;

    bool start();
    void stop();

private:
    Config config_;
    Storage &storage_;
    Dht &dht_;
    std::unique_ptr<Auth> auth_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace helix
