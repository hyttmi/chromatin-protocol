/*
 * Helix - Storage
 *
 * libmdbx-backed storage for messages, users, prekeys, and route cache.
 *
 * Key schema:
 *   msg:<recipient_fp>:<timestamp>:<msg_id>  -> encrypted blob
 *   usr:<fingerprint>                         -> user record (pubkeys, metadata)
 *   pkey:<fingerprint>:<key_id>               -> prekey bundle
 *   route:<fingerprint>                       -> cached nodus URL(s) + TTL
 *   node:identity                             -> node Dilithium5 keypair
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace helix {

struct Message {
    std::string msg_id;
    std::vector<uint8_t> blob;
    int64_t timestamp;
};

struct UserRecord {
    std::vector<uint8_t> dilithium_pk;
    std::vector<uint8_t> kyber_pk;
    int64_t registered_at;
    int64_t last_seen;
};

class Storage {
public:
    explicit Storage(const std::string &data_dir);
    ~Storage();

    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    bool open();
    void close();

    /* Messages */
    bool store_message(const std::string &recipient_fp, const std::string &msg_id,
                       const uint8_t *blob, size_t blob_len);
    std::vector<Message> fetch_messages(const std::string &fingerprint);
    bool delete_message(const std::string &recipient_fp, const std::string &msg_id);

    /* Users */
    bool register_user(const std::string &fingerprint, const UserRecord &record);
    bool get_user(const std::string &fingerprint, UserRecord &record);
    bool is_local_user(const std::string &fingerprint);
    bool update_last_seen(const std::string &fingerprint, int64_t ts);

    /* Prekeys */
    bool store_prekeys(const std::string &fingerprint, const uint8_t *data, size_t len);
    bool get_prekeys(const std::string &fingerprint, std::vector<uint8_t> &out);

    /* Route cache */
    bool cache_route(const std::string &fingerprint, const std::vector<std::string> &urls,
                     int64_t ttl_seconds);
    bool get_cached_route(const std::string &fingerprint, std::vector<std::string> &urls);

    /* Node identity */
    bool store_node_identity(const uint8_t *pk, size_t pk_len,
                             const uint8_t *sk, size_t sk_len);
    bool get_node_identity(std::vector<uint8_t> &pk, std::vector<uint8_t> &sk);

private:
    std::string data_dir_;
    void *env_ = nullptr;  /* MDBX_env* */
    void *dbi_ = nullptr;  /* MDBX_dbi */
};

} // namespace helix
