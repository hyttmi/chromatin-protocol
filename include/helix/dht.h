/*
 * Helix - DHT
 *
 * Embedded vanilla OpenDHT node for peer/node discovery.
 * Handles profile publishing and lookup on behalf of registered users.
 */

#pragma once

#include "helix/config.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

class Dht {
public:
    explicit Dht(const Config &config);
    ~Dht();

    Dht(const Dht &) = delete;
    Dht &operator=(const Dht &) = delete;

    bool start();
    void stop();

    /* Publish a user's profile (fingerprint -> helix URL + pubkeys) */
    bool publish_user_profile(const std::string &fingerprint,
                              const std::vector<uint8_t> &profile_data);

    /* Look up a user's profile by fingerprint */
    using LookupCallback = std::function<void(bool found, const std::vector<uint8_t> &data)>;
    void lookup_user(const std::string &fingerprint, LookupCallback cb);

    /* Publish node profile */
    bool publish_node_profile(const std::vector<uint8_t> &profile_data);

private:
    Config config_;
    void *runner_ = nullptr;  /* dht::DhtRunner* */
};

} // namespace helix
