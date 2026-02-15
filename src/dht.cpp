/*
 * Helix - DHT (vanilla OpenDHT)
 */

#include "helix/dht.h"
#include "helix/log.h"

namespace helix {

Dht::Dht(const Config &config)
    : config_(config)
{
}

Dht::~Dht()
{
    stop();
}

bool Dht::start()
{
    auto logger = log::get("dht");
    logger->info("Starting DHT node on port {}", config_.dht_port);

    /* TODO: Initialize dht::DhtRunner, bootstrap */

    return true;
}

void Dht::stop()
{
    auto logger = log::get("dht");
    logger->debug("Stopping DHT");

    /* TODO: Stop DhtRunner */
}

bool Dht::publish_user_profile(const std::string &fingerprint,
                                const std::vector<uint8_t> &profile_data)
{
    (void)fingerprint; (void)profile_data;
    /* TODO */
    return false;
}

void Dht::lookup_user(const std::string &fingerprint, LookupCallback cb)
{
    (void)fingerprint; (void)cb;
    /* TODO */
}

bool Dht::publish_node_profile(const std::vector<uint8_t> &profile_data)
{
    (void)profile_data;
    /* TODO */
    return false;
}

} // namespace helix
