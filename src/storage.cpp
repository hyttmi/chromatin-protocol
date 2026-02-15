/*
 * Helix - Storage (libmdbx)
 */

#include "helix/storage.h"
#include "helix/log.h"

namespace helix {

Storage::Storage(const std::string &data_dir)
    : data_dir_(data_dir)
{
}

Storage::~Storage()
{
    close();
}

bool Storage::open()
{
    auto logger = log::get("storage");
    logger->info("Opening libmdbx database at {}", data_dir_);

    /* TODO: Initialize libmdbx environment */

    return true;
}

void Storage::close()
{
    auto logger = log::get("storage");
    logger->debug("Closing storage");

    /* TODO: Close libmdbx environment */
}

bool Storage::store_message(const std::string &recipient_fp, const std::string &msg_id,
                            const uint8_t *blob, size_t blob_len)
{
    (void)recipient_fp; (void)msg_id; (void)blob; (void)blob_len;
    /* TODO */
    return false;
}

std::vector<Message> Storage::fetch_messages(const std::string &fingerprint)
{
    (void)fingerprint;
    /* TODO */
    return {};
}

bool Storage::delete_message(const std::string &recipient_fp, const std::string &msg_id)
{
    (void)recipient_fp; (void)msg_id;
    /* TODO */
    return false;
}

bool Storage::register_user(const std::string &fingerprint, const UserRecord &record)
{
    (void)fingerprint; (void)record;
    /* TODO */
    return false;
}

bool Storage::get_user(const std::string &fingerprint, UserRecord &record)
{
    (void)fingerprint; (void)record;
    /* TODO */
    return false;
}

bool Storage::is_local_user(const std::string &fingerprint)
{
    (void)fingerprint;
    /* TODO */
    return false;
}

bool Storage::update_last_seen(const std::string &fingerprint, int64_t ts)
{
    (void)fingerprint; (void)ts;
    /* TODO */
    return false;
}

bool Storage::store_prekeys(const std::string &fingerprint, const uint8_t *data, size_t len)
{
    (void)fingerprint; (void)data; (void)len;
    /* TODO */
    return false;
}

bool Storage::get_prekeys(const std::string &fingerprint, std::vector<uint8_t> &out)
{
    (void)fingerprint; (void)out;
    /* TODO */
    return false;
}

bool Storage::cache_route(const std::string &fingerprint, const std::vector<std::string> &urls,
                          int64_t ttl_seconds)
{
    (void)fingerprint; (void)urls; (void)ttl_seconds;
    /* TODO */
    return false;
}

bool Storage::get_cached_route(const std::string &fingerprint, std::vector<std::string> &urls)
{
    (void)fingerprint; (void)urls;
    /* TODO */
    return false;
}

bool Storage::store_node_identity(const uint8_t *pk, size_t pk_len,
                                  const uint8_t *sk, size_t sk_len)
{
    (void)pk; (void)pk_len; (void)sk; (void)sk_len;
    /* TODO */
    return false;
}

bool Storage::get_node_identity(std::vector<uint8_t> &pk, std::vector<uint8_t> &sk)
{
    (void)pk; (void)sk;
    /* TODO */
    return false;
}

} // namespace helix
