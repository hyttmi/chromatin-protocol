/*
 * Helix - Relay (node-to-node message delivery)
 */

#include "helix/relay.h"
#include "helix/log.h"

namespace helix {

Relay::Relay(Storage &storage, Dht &dht)
    : storage_(storage)
    , dht_(dht)
{
}

Relay::~Relay() = default;

void Relay::deliver(const std::string &recipient_fp, const std::string &msg_id,
                    const uint8_t *blob, size_t blob_len, DeliverCallback cb)
{
    auto logger = log::get("relay");
    (void)blob; (void)blob_len;

    /* Check if recipient is local */
    if (storage_.is_local_user(recipient_fp)) {
        logger->debug("Local delivery for {:.16}...", recipient_fp);
        bool ok = storage_.store_message(recipient_fp, msg_id, blob, blob_len);
        if (cb) cb(ok, ok ? "" : "storage error");
        return;
    }

    /* Remote: look up recipient's helix via DHT */
    logger->debug("Remote delivery for {:.16}..., looking up route", recipient_fp);

    /* Check route cache first */
    std::vector<std::string> urls;
    if (storage_.get_cached_route(recipient_fp, urls)) {
        /* TODO: HTTP POST /relay to each URL */
        logger->debug("Route cache hit for {:.16}..., {} helix(es)", recipient_fp, urls.size());
        if (cb) cb(false, "relay not yet implemented");
        return;
    }

    /* DHT lookup */
    dht_.lookup_user(recipient_fp, [this, recipient_fp, msg_id, cb, logger]
                     (bool found, const std::vector<uint8_t> &data) {
        if (!found) {
            logger->warn("DHT lookup failed for {:.16}...", recipient_fp);
            if (cb) cb(false, "recipient not found");
            return;
        }

        /* TODO: Parse profile, extract helix URLs, relay */
        (void)data;
        if (cb) cb(false, "relay not yet implemented");
    });
}

} // namespace helix
