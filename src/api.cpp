/*
 * Helix - REST/WebSocket API (Boost.Beast)
 */

#include "helix/api.h"
#include "helix/auth.h"
#include "helix/storage.h"
#include "helix/dht.h"
#include "helix/log.h"

namespace helix {

struct Api::Impl {
    /* TODO: Beast acceptor, session management */
};

Api::Api(const Config &config, Storage &storage, Dht &dht)
    : config_(config)
    , storage_(storage)
    , dht_(dht)
    , auth_(std::make_unique<Auth>(storage))
    , impl_(std::make_unique<Impl>())
{
}

Api::~Api()
{
    stop();
}

bool Api::start()
{
    auto logger = log::get("api");
    logger->info("Starting API server on {}:{}", config_.listen_addr, config_.listen_port);

    /* TODO: Start Beast HTTP listener */

    return true;
}

void Api::stop()
{
    auto logger = log::get("api");
    logger->debug("Stopping API server");

    /* TODO: Stop Beast listener */
}

} // namespace helix
