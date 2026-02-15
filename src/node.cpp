/*
 * Helix - Node
 */

#include "helix/node.h"
#include "helix/storage.h"
#include "helix/dht.h"
#include "helix/api.h"
#include "helix/log.h"

namespace helix {

Node::Node(const Config &config)
    : config_(config)
    , storage_(std::make_unique<Storage>(config.data_dir))
    , dht_(std::make_unique<Dht>(config))
    , api_(std::make_unique<Api>(config, *storage_, *dht_))
{
}

Node::~Node()
{
    stop();
}

bool Node::start()
{
    auto logger = log::get("node");

    logger->info("Opening storage at {}", config_.data_dir);
    if (!storage_->open()) {
        logger->error("Failed to open storage");
        return false;
    }

    logger->info("Starting DHT on port {}", config_.dht_port);
    if (!dht_->start()) {
        logger->error("Failed to start DHT");
        return false;
    }

    logger->info("Starting API on {}:{}", config_.listen_addr, config_.listen_port);
    if (!api_->start()) {
        logger->error("Failed to start API");
        return false;
    }

    logger->info("Node started successfully");
    return true;
}

void Node::stop()
{
    auto logger = log::get("node");
    logger->info("Stopping node");

    if (api_) api_->stop();
    if (dht_) dht_->stop();
    if (storage_) storage_->close();
}

} // namespace helix
