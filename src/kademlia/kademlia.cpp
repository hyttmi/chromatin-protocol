#include "kademlia/kademlia.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <regex>
#include <thread>

#include <arpa/inet.h>

#include <spdlog/spdlog.h>

#include "../version.h"

namespace chromatin::kademlia {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Kademlia::Kademlia(const config::Config& cfg, NodeInfo self, TcpTransport& transport,
                   RoutingTable& table, storage::Storage& storage,
                   replication::ReplLog& repl_log, const crypto::KeyPair& keypair)
    : cfg_(cfg)
    , self_(std::move(self))
    , transport_(transport)
    , table_(table)
    , storage_(storage)
    , repl_log_(repl_log)
    , keypair_(keypair) {}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

void Kademlia::bootstrap(const std::vector<std::pair<std::string, uint16_t>>& addrs) {
    // Build FIND_NODE payload with our pubkey so receivers can immediately
    // verify our identity.  Format: pubkey_len(2 BE) || pubkey(N).
    // Empty payload is still accepted for backwards compatibility.
    auto payload = make_find_node_payload();
    Message msg = make_message(MessageType::FIND_NODE, payload);
    for (const auto& [addr, port] : addrs) {
        spdlog::info("Bootstrap: sending FIND_NODE to {}:{}", addr, port);
        transport_.send(addr, port, msg);
    }
}

// ---------------------------------------------------------------------------
// Bootstrap address persistence
// ---------------------------------------------------------------------------

void Kademlia::set_bootstrap_addrs(std::vector<std::pair<std::string, uint16_t>> addrs) {
    bootstrap_addrs_ = std::move(addrs);
}

void Kademlia::set_on_store(StoreCallback cb) {
    on_store_ = std::move(cb);
}

void Kademlia::set_on_relay(RelayCallback cb) {
    on_relay_ = std::move(cb);
}

void Kademlia::relay(const crypto::Hash& key, std::span<const uint8_t> payload) {
    auto nodes = responsible_nodes(key);
    Message msg = make_message(MessageType::RELAY,
                               std::vector<uint8_t>(payload.begin(), payload.end()));
    for (const auto& node : nodes) {
        if (node.id != self_.id) {
            send_to_node(node, msg);
        }
    }
}

void Kademlia::handle_relay(const Message& msg, const std::string& from, uint16_t port) {
    if (on_relay_) {
        on_relay_(msg.payload);
    }
}

// ---------------------------------------------------------------------------
// Periodic maintenance (self-healing)
// ---------------------------------------------------------------------------

static constexpr size_t SPARSE_TABLE_SIZE = 3;

void Kademlia::tick() {
    auto now = std::chrono::steady_clock::now();

    // 1. Re-bootstrap / refresh
    auto refresh_interval = (table_.size() < SPARSE_TABLE_SIZE)
                                ? refresh_interval_sparse_
                                : refresh_interval_;

    if (now - last_refresh_ >= refresh_interval) {
        last_refresh_ = now;

        // Send FIND_NODE to bootstrap peers
        if (!bootstrap_addrs_.empty()) {
            bootstrap(bootstrap_addrs_);
        }

        // Also query all known nodes (iterative discovery)
        auto known = table_.all_nodes();
        auto fn_payload = make_find_node_payload();
        for (const auto& node : known) {
            Message msg = make_message(MessageType::FIND_NODE, fn_payload);
            send_to_node(node, msg);
        }
    }

    // 2. Ping stale nodes + evict dead nodes
    if (now - last_ping_sweep_ >= ping_sweep_interval_) {
        last_ping_sweep_ = now;

        auto stale_cutoff = now - stale_threshold_;
        auto evict_cutoff = now - evict_threshold_;

        // Evict dead nodes first
        table_.evict_older_than(evict_cutoff);

        // Ping stale (but not yet dead) nodes
        auto nodes = table_.all_nodes();
        for (const auto& node : nodes) {
            if (node.last_seen < stale_cutoff) {
                Message msg = make_message(MessageType::PING, {});
                send_to_node(node, msg);
            }
        }
    }

    // 3. TTL expiry sweep
    if (now - last_ttl_sweep_ >= ttl_sweep_interval_) {
        last_ttl_sweep_ = now;
        expire_ttl();
    }

    // 4. Pending stores cleanup
    cleanup_pending_stores();

    // 5. Responsibility transfer when routing table changes
    size_t current_size = table_.size();
    if (current_size != last_table_size_ &&
        now - last_transfer_check_ >= transfer_check_interval_) {
        last_transfer_check_ = now;
        last_table_size_ = current_size;
        transfer_responsibility();
    }

    // 6. Periodic repl_log compaction
    if (now - last_compact_ > compact_interval_) {
        last_compact_ = now;
        compact_repl_log();
    }

    // 7. Cleanup idle pooled TCP connections
    transport_.cleanup_idle_connections();

    // 8. Active sync: pull missing entries from peers
    if (now - last_sync_ >= sync_interval_) {
        last_sync_ = now;
        sync_with_peers();
    }

    // 9. Clean up stale FIND_NODE rate entries (older than 30 seconds)
    {
        std::lock_guard lock(find_node_rate_mutex_);
        auto cutoff = now - std::chrono::seconds(30);
        for (auto it = find_node_rate_.begin(); it != find_node_rate_.end(); ) {
            if (it->second < cutoff) {
                it = find_node_rate_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 10. Periodic integrity sweep
    if (now - last_integrity_sweep_ >= integrity_sweep_interval_) {
        last_integrity_sweep_ = now;
        integrity_sweep();
    }
}

// ---------------------------------------------------------------------------
// TTL expiry — delete inbox messages and contact requests older than 7 days
// ---------------------------------------------------------------------------

void Kademlia::expire_ttl() {
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto ttl_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ttl_duration_).count());
    uint64_t cutoff_ms = (now_ms > ttl_ms) ? (now_ms - ttl_ms) : 0;

    // Expire inbox messages: scan TABLE_INBOX_INDEX
    // Index value format: sender_fp(32) || timestamp(8 BE) || blob_len(4)
    std::vector<std::vector<uint8_t>> expired_idx_keys;
    std::vector<std::vector<uint8_t>> expired_blob_keys;

    storage_.foreach(storage::TABLE_INBOX_INDEX,
        [&](std::span<const uint8_t> key, std::span<const uint8_t> value) -> bool {
            if (value.size() < 44) return true;  // skip malformed
            // Timestamp is at offset 32 in the value (after sender_fp)
            uint64_t ts = 0;
            for (int i = 0; i < 8; ++i)
                ts = (ts << 8) | value[32 + i];

            if (ts < cutoff_ms) {
                expired_idx_keys.emplace_back(key.begin(), key.end());
                // msg_id is at offset 32 in the key (after recipient_fp)
                if (key.size() >= 64) {
                    expired_blob_keys.emplace_back(key.begin() + 32, key.begin() + 64);
                }
            }
            return true;
        });

    for (const auto& k : expired_idx_keys)
        storage_.del(storage::TABLE_INBOX_INDEX, k);
    for (const auto& k : expired_blob_keys)
        storage_.del(storage::TABLE_MESSAGE_BLOBS, k);

    if (!expired_idx_keys.empty()) {
        spdlog::info("TTL expiry: removed {} expired inbox messages", expired_idx_keys.size());
    }

    // Expire contact requests: scan TABLE_REQUESTS
    // Contact request value: recipient_fp(32) || sender_fp(32) || pow_nonce(8) || blob_len(4) || blob
    // No timestamp in the value itself — use repl_log timestamp for age check.
    // For each request, compute its routing key and find the most recent repl_log
    // entry. If that entry's timestamp is older than TTL, the request has expired.
    //
    // Two-pass approach: first collect all request keys and their recipient_fps
    // (to avoid nested read transactions in mdbx), then check repl_log timestamps.

    struct RequestInfo {
        std::vector<uint8_t> storage_key;   // recipient_fp(32) || sender_fp(32)
        crypto::Hash recipient_fp;
    };
    std::vector<RequestInfo> all_requests;

    storage_.foreach(storage::TABLE_REQUESTS,
        [&](std::span<const uint8_t> key, std::span<const uint8_t> value) -> bool {
            if (key.size() < 64 || value.size() < 76) return true;
            RequestInfo ri;
            ri.storage_key.assign(key.begin(), key.end());
            std::copy_n(key.data(), 32, ri.recipient_fp.begin());
            all_requests.push_back(std::move(ri));
            return true;
        });

    // Now check repl_log timestamps outside of the foreach transaction
    std::vector<std::vector<uint8_t>> expired_request_keys;
    for (const auto& ri : all_requests) {
        auto routing_key = crypto::sha3_256_prefixed("inbox:", ri.recipient_fp);
        auto entries = repl_log_.entries_after(routing_key, 0);
        if (entries.empty()) continue;  // no repl_log entry — can't determine age, skip

        // Find the latest entry's timestamp
        uint64_t latest_ts = 0;
        for (const auto& e : entries) {
            if (e.timestamp > latest_ts) latest_ts = e.timestamp;
        }

        if (latest_ts > 0 && latest_ts < cutoff_ms) {
            expired_request_keys.push_back(ri.storage_key);
        }
    }

    for (const auto& k : expired_request_keys)
        storage_.del(storage::TABLE_REQUESTS, k);

    if (!expired_request_keys.empty()) {
        spdlog::info("TTL expiry: removed {} expired contact requests", expired_request_keys.size());
    }
}

// ---------------------------------------------------------------------------
// Pending store cleanup — remove entries that never received all ACKs
// ---------------------------------------------------------------------------

void Kademlia::cleanup_pending_stores() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(pending_mutex_);
    for (auto it = pending_stores_.begin(); it != pending_stores_.end();) {
        if (now - it->second.created > pending_store_timeout_) {
            spdlog::debug("Pending store timed out (expected {} ACKs, got {})",
                          it->second.expected, it->second.acked);
            it = pending_stores_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Responsibility transfer — push data to newly-responsible nodes
// ---------------------------------------------------------------------------

void Kademlia::transfer_responsibility() {
    // Build the current set of routing node IDs for dedup.
    // Only push data to nodes that are NEWLY in the routing table
    // (not in prev_routing_nodes_), since existing nodes already have the
    // data from normal replication.
    auto current_nodes = table_.all_nodes();
    std::unordered_set<NodeId, NodeIdHash> current_set;
    std::unordered_set<NodeId, NodeIdHash> new_nodes;
    for (const auto& n : current_nodes) {
        current_set.insert(n.id);
        if (prev_routing_nodes_.find(n.id) == prev_routing_nodes_.end()) {
            new_nodes.insert(n.id);
        }
    }

    // Update previous set for next call
    prev_routing_nodes_ = current_set;

    // No new nodes → nothing to transfer
    if (new_nodes.empty()) {
        spdlog::debug("Responsibility transfer: no new nodes, skipping");
        return;
    }

    struct TableInfo {
        const char* table;
        uint8_t data_type;
    };

    static constexpr TableInfo tables[] = {
        {storage::TABLE_PROFILES,   0x00},
        {storage::TABLE_NAMES,      0x01},
        {storage::TABLE_REQUESTS,   0x03},
        {storage::TABLE_ALLOWLISTS, 0x04},
        {storage::TABLE_GROUP_META, 0x06},
    };

    size_t pushed = 0;

    for (const auto& ti : tables) {
        // Collect corrupt keys to purge after foreach (can't delete inside read txn)
        std::vector<std::vector<uint8_t>> corrupt_keys;

        storage_.foreach(ti.table,
            [&](std::span<const uint8_t> key, std::span<const uint8_t> value) -> bool {
                if (key.size() < 32) return true;

                crypto::Hash routing_key{};
                std::copy_n(key.data(), 32, routing_key.begin());

                // Re-validate before sending — collect corrupt keys for later purge
                if (!validate_readonly(routing_key, ti.data_type, value)) {
                    corrupt_keys.emplace_back(key.begin(), key.end());
                    return true;
                }

                auto nodes = responsible_nodes(routing_key);

                // Only push to newly-added responsible nodes
                for (const auto& node : nodes) {
                    if (node.id == self_.id) continue;
                    if (new_nodes.find(node.id) == new_nodes.end()) continue;

                    std::vector<uint8_t> payload;
                    payload.insert(payload.end(), routing_key.begin(), routing_key.end());
                    payload.push_back(ti.data_type);
                    uint32_t vlen = static_cast<uint32_t>(value.size());
                    payload.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
                    payload.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
                    payload.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
                    payload.push_back(static_cast<uint8_t>(vlen & 0xFF));
                    payload.insert(payload.end(), value.begin(), value.end());

                    Message msg = make_message(MessageType::STORE, payload);
                    send_to_node(node, msg);
                    ++pushed;
                }

                return true;
            });

        // Purge corrupt entries outside the read transaction
        for (const auto& key : corrupt_keys) {
            spdlog::warn("transfer: purging corrupt {} entry", ti.table);
            storage_.del(ti.table, key);
        }
    }

    // Transfer inbox messages to newly-responsible nodes.
    struct InboxEntry {
        crypto::Hash recipient_fp;
        crypto::Hash msg_id;
        std::vector<uint8_t> index_value;
    };
    std::vector<InboxEntry> inbox_entries;

    storage_.foreach(storage::TABLE_INBOX_INDEX,
        [&](std::span<const uint8_t> key, std::span<const uint8_t> index_value) -> bool {
            if (key.size() < 64) return true;
            if (index_value.size() < 44) return true;

            InboxEntry entry;
            std::copy_n(key.data(), 32, entry.recipient_fp.begin());
            std::copy_n(key.data() + 32, 32, entry.msg_id.begin());
            entry.index_value.assign(index_value.begin(), index_value.end());
            inbox_entries.push_back(std::move(entry));
            return true;
        });

    for (const auto& entry : inbox_entries) {
        auto routing_key = crypto::sha3_256_prefixed("inbox:", entry.recipient_fp);
        auto nodes = responsible_nodes(routing_key);

        // Check if any newly-added node is responsible before doing blob lookup
        bool has_new_responsible = false;
        for (const auto& node : nodes) {
            if (node.id != self_.id && new_nodes.find(node.id) != new_nodes.end()) {
                has_new_responsible = true;
                break;
            }
        }
        if (!has_new_responsible) continue;

        auto blob = storage_.get(storage::TABLE_MESSAGE_BLOBS, entry.msg_id);
        if (!blob) continue;

        std::vector<uint8_t> store_value;
        store_value.reserve(108 + blob->size());
        store_value.insert(store_value.end(), entry.recipient_fp.begin(), entry.recipient_fp.end());
        store_value.insert(store_value.end(), entry.msg_id.begin(), entry.msg_id.end());
        store_value.insert(store_value.end(), entry.index_value.begin(), entry.index_value.begin() + 40);
        uint32_t blen = static_cast<uint32_t>(blob->size());
        store_value.push_back(static_cast<uint8_t>((blen >> 24) & 0xFF));
        store_value.push_back(static_cast<uint8_t>((blen >> 16) & 0xFF));
        store_value.push_back(static_cast<uint8_t>((blen >> 8) & 0xFF));
        store_value.push_back(static_cast<uint8_t>(blen & 0xFF));
        store_value.insert(store_value.end(), blob->begin(), blob->end());

        // Structural validation of reassembled inbox message
        if (!validate_inbox_message(store_value)) {
            spdlog::warn("transfer: purging corrupt inbox entry");
            std::vector<uint8_t> idx_key;
            idx_key.reserve(64);
            idx_key.insert(idx_key.end(), entry.recipient_fp.begin(), entry.recipient_fp.end());
            idx_key.insert(idx_key.end(), entry.msg_id.begin(), entry.msg_id.end());
            storage_.del(storage::TABLE_INBOX_INDEX, idx_key);
            storage_.del(storage::TABLE_MESSAGE_BLOBS, entry.msg_id);
            continue;
        }

        for (const auto& node : nodes) {
            if (node.id == self_.id) continue;
            if (new_nodes.find(node.id) == new_nodes.end()) continue;

            std::vector<uint8_t> payload;
            payload.insert(payload.end(), routing_key.begin(), routing_key.end());
            payload.push_back(0x02);
            uint32_t vlen = static_cast<uint32_t>(store_value.size());
            payload.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
            payload.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
            payload.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
            payload.push_back(static_cast<uint8_t>(vlen & 0xFF));
            payload.insert(payload.end(), store_value.begin(), store_value.end());

            Message msg = make_message(MessageType::STORE, payload);
            send_to_node(node, msg);
            ++pushed;
        }
    }

    // Transfer group messages (TABLE_GROUP_INDEX + TABLE_GROUP_BLOBS) to newly-responsible nodes.
    // Similar to inbox message transfer: reassemble the full store value from index + blob.
    struct GroupMsgEntry {
        crypto::Hash group_id;
        crypto::Hash msg_id;
        std::vector<uint8_t> index_value; // sender_fp(32) || timestamp(8) || size(4) || gek_version(4) = 48
    };
    std::vector<GroupMsgEntry> group_entries;

    storage_.foreach(storage::TABLE_GROUP_INDEX,
        [&](std::span<const uint8_t> key, std::span<const uint8_t> index_value) -> bool {
            if (key.size() < 64) return true;
            if (index_value.size() < 48) return true;

            GroupMsgEntry entry;
            std::copy_n(key.data(), 32, entry.group_id.begin());
            std::copy_n(key.data() + 32, 32, entry.msg_id.begin());
            entry.index_value.assign(index_value.begin(), index_value.end());
            group_entries.push_back(std::move(entry));
            return true;
        });

    for (const auto& entry : group_entries) {
        auto gid_span = std::span<const uint8_t>(entry.group_id.data(), entry.group_id.size());
        auto routing_key = crypto::sha3_256_prefixed("group:", gid_span);
        auto nodes = responsible_nodes(routing_key);

        bool has_new_responsible = false;
        for (const auto& node : nodes) {
            if (node.id != self_.id && new_nodes.find(node.id) != new_nodes.end()) {
                has_new_responsible = true;
                break;
            }
        }
        if (!has_new_responsible) continue;

        // Look up blob
        std::vector<uint8_t> composite_key;
        composite_key.reserve(64);
        composite_key.insert(composite_key.end(), entry.group_id.begin(), entry.group_id.end());
        composite_key.insert(composite_key.end(), entry.msg_id.begin(), entry.msg_id.end());
        auto blob = storage_.get(storage::TABLE_GROUP_BLOBS, composite_key);
        if (!blob) continue;

        // Reassemble store value:
        // group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8) || gek_version(4) || blob_len(4) || blob
        std::vector<uint8_t> store_value;
        store_value.reserve(112 + blob->size());
        store_value.insert(store_value.end(), entry.group_id.begin(), entry.group_id.end());     // group_id(32)
        store_value.insert(store_value.end(), entry.index_value.begin(), entry.index_value.begin() + 32); // sender_fp(32)
        store_value.insert(store_value.end(), entry.msg_id.begin(), entry.msg_id.end());         // msg_id(32)
        store_value.insert(store_value.end(), entry.index_value.begin() + 32, entry.index_value.begin() + 40); // timestamp(8)
        store_value.insert(store_value.end(), entry.index_value.begin() + 44, entry.index_value.begin() + 48); // gek_version(4)
        uint32_t blen = static_cast<uint32_t>(blob->size());
        store_value.push_back(static_cast<uint8_t>((blen >> 24) & 0xFF));
        store_value.push_back(static_cast<uint8_t>((blen >> 16) & 0xFF));
        store_value.push_back(static_cast<uint8_t>((blen >> 8) & 0xFF));
        store_value.push_back(static_cast<uint8_t>(blen & 0xFF));
        store_value.insert(store_value.end(), blob->begin(), blob->end());

        // Validate before sending
        if (!validate_group_message(store_value)) {
            spdlog::warn("transfer: skipping corrupt group message");
            continue;
        }

        for (const auto& node : nodes) {
            if (node.id == self_.id) continue;
            if (new_nodes.find(node.id) == new_nodes.end()) continue;

            std::vector<uint8_t> payload;
            payload.insert(payload.end(), routing_key.begin(), routing_key.end());
            payload.push_back(0x05);
            uint32_t vlen = static_cast<uint32_t>(store_value.size());
            payload.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
            payload.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
            payload.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
            payload.push_back(static_cast<uint8_t>(vlen & 0xFF));
            payload.insert(payload.end(), store_value.begin(), store_value.end());

            Message msg = make_message(MessageType::STORE, payload);
            send_to_node(node, msg);
            ++pushed;
        }
    }

    if (pushed > 0) {
        spdlog::info("Responsibility transfer: pushed {} entries to {} new node(s)",
                     pushed, new_nodes.size());
    }
}

// ---------------------------------------------------------------------------
// Replication log compaction — keep only the last N entries per unique key
// ---------------------------------------------------------------------------

void Kademlia::compact_repl_log() {
    // Compute time floor: entries younger than this are preserved
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    uint64_t min_age_ms = static_cast<uint64_t>(compact_min_age_hours_) * 3'600'000ULL;
    uint64_t before_timestamp_ms = (now_ms > min_age_ms) ? (now_ms - min_age_ms) : 0;

    // Collect unique 32-byte key prefixes from TABLE_REPL_LOG
    std::vector<crypto::Hash> unique_keys;
    crypto::Hash prev_key{};
    prev_key.fill(0xFF);  // sentinel — won't match any real key

    storage_.foreach(storage::TABLE_REPL_LOG,
        [&](std::span<const uint8_t> k, std::span<const uint8_t> /*v*/) -> bool {
            if (k.size() < 40) return true;
            crypto::Hash key_prefix{};
            std::copy_n(k.data(), 32, key_prefix.begin());
            if (key_prefix != prev_key) {
                unique_keys.push_back(key_prefix);
                prev_key = key_prefix;
            }
            return true;
        });

    size_t total_compacted = 0;
    for (const auto& key : unique_keys) {
        uint64_t max_seq = repl_log_.current_seq(key);
        if (max_seq > compact_keep_entries_) {
            uint64_t before_seq = max_seq - compact_keep_entries_;
            repl_log_.compact(key, before_seq, before_timestamp_ms);
            ++total_compacted;
        }
    }

    if (total_compacted > 0) {
        spdlog::info("Repl log compaction: compacted {} keys (keeping last {} entries, min age {} hours)",
                     total_compacted, compact_keep_entries_, compact_min_age_hours_);
    }
}

// ---------------------------------------------------------------------------
// Active sync — pull missing replication entries from peers
// ---------------------------------------------------------------------------

void Kademlia::sync_with_peers() {
    // Collect unique routing keys from the replication log.
    // These are the 32-byte prefixes of composite keys in TABLE_REPL_LOG.
    std::vector<crypto::Hash> all_keys;
    crypto::Hash prev_key{};
    prev_key.fill(0xFF);

    storage_.foreach(storage::TABLE_REPL_LOG,
        [&](std::span<const uint8_t> k, std::span<const uint8_t> /*v*/) -> bool {
            if (k.size() < 40) return true;
            crypto::Hash key_prefix{};
            std::copy_n(k.data(), 32, key_prefix.begin());
            if (key_prefix != prev_key) {
                all_keys.push_back(key_prefix);
                prev_key = key_prefix;
            }
            return true;
        });

    if (all_keys.empty()) return;

    // Batch: process sync_batch_size_ keys per tick, cycling with offset
    if (sync_key_offset_ >= all_keys.size()) {
        sync_key_offset_ = 0;
    }

    size_t end = std::min(sync_key_offset_ + sync_batch_size_, all_keys.size());
    size_t synced = 0;

    for (size_t i = sync_key_offset_; i < end; ++i) {
        const auto& key = all_keys[i];

        // Only sync keys we're responsible for
        if (!is_responsible(key)) continue;

        // Get our local seq for this key
        uint64_t local_seq = repl_log_.current_seq(key);

        // Find other responsible nodes to sync from
        auto nodes = responsible_nodes(key);

        for (const auto& node : nodes) {
            if (node.id == self_.id) continue;

            // Send SYNC_REQ asking for entries after our local seq
            std::vector<uint8_t> payload;
            payload.reserve(40);
            payload.insert(payload.end(), key.begin(), key.end());
            // after_seq (8 bytes BE)
            for (int b = 7; b >= 0; --b) {
                payload.push_back(static_cast<uint8_t>((local_seq >> (b * 8)) & 0xFF));
            }

            Message msg = make_message(MessageType::SYNC_REQ, payload);
            send_to_node(node, msg);
            ++synced;
        }
    }

    sync_key_offset_ = end;

    if (synced > 0) {
        spdlog::debug("Active sync: sent SYNC_REQ for {} keys (batch {}-{} of {})",
                      synced, sync_key_offset_ - (end - sync_key_offset_), end, all_keys.size());
    }
}

// ---------------------------------------------------------------------------
// Integrity sweep — validate stored data, purge corrupt entries
// ---------------------------------------------------------------------------

void Kademlia::integrity_sweep() {
    struct SweepTable {
        const char* table;
        uint8_t data_type;
    };

    static constexpr SweepTable sweep_tables[] = {
        {storage::TABLE_NAMES,      0x01},
        {storage::TABLE_PROFILES,   0x00},
        {storage::TABLE_GROUP_META, 0x06},
        {storage::TABLE_ALLOWLISTS, 0x04},
        {storage::TABLE_REQUESTS,   0x03},
    };
    static constexpr size_t NUM_SWEEP_TABLES = sizeof(sweep_tables) / sizeof(sweep_tables[0]);

    if (integrity_sweep_table_idx_ >= NUM_SWEEP_TABLES) {
        integrity_sweep_table_idx_ = 0;
    }

    const auto& st = sweep_tables[integrity_sweep_table_idx_];
    integrity_sweep_table_idx_ = (integrity_sweep_table_idx_ + 1) % NUM_SWEEP_TABLES;

    // Collect keys to delete (can't delete inside foreach)
    struct PurgeEntry {
        std::vector<uint8_t> key;
    };
    std::vector<PurgeEntry> to_purge;
    size_t checked = 0;

    storage_.foreach(st.table,
        [&](std::span<const uint8_t> key, std::span<const uint8_t> value) -> bool {
            if (checked >= config::defaults::INTEGRITY_SWEEP_BATCH_SIZE) return false;
            ++checked;

            if (key.size() < 32) return true;

            crypto::Hash routing_key{};
            std::copy_n(key.data(), 32, routing_key.begin());

            if (!validate_readonly(routing_key, st.data_type, value)) {
                to_purge.push_back({std::vector<uint8_t>(key.begin(), key.end())});
            }

            return true;
        });

    for (const auto& entry : to_purge) {
        storage_.del(st.table, entry.key);
    }

    if (!to_purge.empty()) {
        spdlog::warn("Integrity sweep: purged {}/{} corrupt entries from {}",
                     to_purge.size(), checked, st.table);
    }
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void Kademlia::handle_message(const Message& msg, const std::string& from_addr, uint16_t from_port) {
    // Signature verification policy:
    // - PING, FIND_NODE: fully exempt (unsigned discovery messages)
    // - PONG, NODES: verified when sender's pubkey is known, accepted otherwise
    //   (needed for initial discovery before pubkey exchange)
    // - All other types: mandatory verification, rejected if unknown/unverified
    if (msg.type == MessageType::PONG || msg.type == MessageType::NODES) {
        auto node_info = table_.find(msg.sender);
        if (node_info && !node_info->pubkey.empty()) {
            if (!verify_message(msg, node_info->pubkey)) {
                spdlog::warn("Rejected {} with invalid signature from {}:{}",
                             msg.type == MessageType::PONG ? "PONG" : "NODES",
                             from_addr, from_port);
                return;
            }
        }
        // Unknown sender or no pubkey: accept for discovery
    } else if (msg.type != MessageType::PING && msg.type != MessageType::FIND_NODE) {
        auto node_info = table_.find(msg.sender);
        if (!node_info || node_info->pubkey.empty()) {
            spdlog::warn("Rejected message type {} from unknown/unverified node {}:{}",
                         static_cast<int>(msg.type), from_addr, from_port);
            return;
        }
        if (!verify_message(msg, node_info->pubkey)) {
            spdlog::warn("Rejected message with invalid signature from {}:{}", from_addr, from_port);
            return;
        }
    }

    switch (msg.type) {
    case MessageType::PING:
        handle_ping(msg, from_addr, from_port);
        break;
    case MessageType::PONG:
        handle_pong(msg, from_addr, from_port);
        break;
    case MessageType::FIND_NODE:
        handle_find_node(msg, from_addr, from_port);
        break;
    case MessageType::NODES:
        handle_nodes(msg, from_addr, from_port);
        break;
    case MessageType::STORE:
        handle_store(msg, from_addr, from_port);
        break;
    case MessageType::FIND_VALUE:
        handle_find_value(msg, from_addr, from_port);
        break;
    case MessageType::VALUE:
        handle_value(msg, from_addr, from_port);
        break;
    case MessageType::SYNC_REQ:
        handle_sync_req(msg, from_addr, from_port);
        break;
    case MessageType::SYNC_RESP:
        handle_sync_resp(msg, from_addr, from_port);
        break;
    case MessageType::STORE_ACK:
        handle_store_ack(msg, from_addr, from_port);
        break;
    case MessageType::SEQ_REQ:
        handle_seq_req(msg, from_addr, from_port);
        break;
    case MessageType::SEQ_RESP:
        handle_seq_resp(msg, from_addr, from_port);
        break;
    case MessageType::RELAY:
        handle_relay(msg, from_addr, from_port);
        break;
    default:
        spdlog::warn("Unhandled message type: 0x{:02X}", static_cast<uint8_t>(msg.type));
        break;
    }
}

// ---------------------------------------------------------------------------
// PING / PONG
// ---------------------------------------------------------------------------

void Kademlia::handle_ping(const Message& msg, const std::string& from, uint16_t port) {
    spdlog::debug("Received PING from {}:{}", from, port);

    // Update routing table — the sender is alive.
    // Preserve the stored address if the node already exists — the TCP
    // source IP may be a LAN address when nodes share a network, while
    // the stored address (from FIND_NODE) is the self-reported external.
    NodeInfo sender_info;
    sender_info.id = msg.sender;
    sender_info.tcp_port = port;
    sender_info.ws_port = 0;
    sender_info.tcp_source_ip = from;
    sender_info.last_seen = std::chrono::steady_clock::now();
    auto existing = table_.find(msg.sender);
    sender_info.address = existing ? existing->address : from;
    table_.add_or_update(sender_info);

    // PONG payload: min_version(1) || max_version(1) || capabilities(4 BE) || app_major(1) || app_minor(1) || app_patch(1)
    std::vector<uint8_t> pong_payload(9);
    pong_payload[0] = PROTOCOL_VERSION;  // min supported
    pong_payload[1] = PROTOCOL_VERSION;  // max supported
    uint32_t caps = static_cast<uint32_t>(Capability::GROUPS)
                  | static_cast<uint32_t>(Capability::ENCRYPTED_TCP);
    pong_payload[2] = static_cast<uint8_t>((caps >> 24) & 0xFF);
    pong_payload[3] = static_cast<uint8_t>((caps >> 16) & 0xFF);
    pong_payload[4] = static_cast<uint8_t>((caps >> 8) & 0xFF);
    pong_payload[5] = static_cast<uint8_t>(caps & 0xFF);
    pong_payload[6] = VERSION_MAJOR;
    pong_payload[7] = VERSION_MINOR;
    pong_payload[8] = VERSION_PATCH;

    Message reply = make_message(MessageType::PONG, pong_payload);
    transport_.send(from, port, reply);
}

void Kademlia::handle_pong(const Message& msg, const std::string& from, uint16_t port) {
    spdlog::debug("Received PONG from {}:{}", from, port);

    // Update routing table — the sender is alive.
    // Preserve the stored address (see handle_ping comment).
    NodeInfo sender_info;
    sender_info.id = msg.sender;
    sender_info.tcp_port = port;
    sender_info.ws_port = 0;
    sender_info.tcp_source_ip = from;
    sender_info.last_seen = std::chrono::steady_clock::now();
    auto existing = table_.find(msg.sender);
    sender_info.address = existing ? existing->address : from;

    if (msg.payload.size() < 9) {
        spdlog::warn("PONG from {}:{}: payload too short ({} bytes), rejecting",
                     from, port, msg.payload.size());
        return;
    }
    sender_info.proto_version_min = msg.payload[0];
    sender_info.proto_version_max = msg.payload[1];
    sender_info.capabilities = (static_cast<uint32_t>(msg.payload[2]) << 24)
                             | (static_cast<uint32_t>(msg.payload[3]) << 16)
                             | (static_cast<uint32_t>(msg.payload[4]) << 8)
                             |  static_cast<uint32_t>(msg.payload[5]);
    sender_info.app_version_major = msg.payload[6];
    sender_info.app_version_minor = msg.payload[7];
    sender_info.app_version_patch = msg.payload[8];

    if (PROTOCOL_VERSION < sender_info.proto_version_min ||
        PROTOCOL_VERSION > sender_info.proto_version_max) {
        spdlog::warn("PONG from {}:{}: wire version mismatch (peer {}-{}, ours {}), rejecting",
                     from, port, sender_info.proto_version_min,
                     sender_info.proto_version_max, PROTOCOL_VERSION);
        table_.remove(sender_info.id);
        return;
    }

    if (sender_info.app_version_major != VERSION_MAJOR ||
        sender_info.app_version_minor != VERSION_MINOR ||
        sender_info.app_version_patch != VERSION_PATCH) {
        spdlog::warn("PONG from {}:{}: app version mismatch (peer={}.{}.{}, ours={}.{}.{}), rejecting",
                     from, port,
                     sender_info.app_version_major, sender_info.app_version_minor,
                     sender_info.app_version_patch,
                     VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
        table_.remove(sender_info.id);
        return;
    }

    table_.add_or_update(sender_info);
}

// ---------------------------------------------------------------------------
// FIND_NODE / NODES  (section 2 of PROTOCOL-SPEC.md)
// ---------------------------------------------------------------------------

void Kademlia::handle_find_node(const Message& msg, const std::string& from, uint16_t port) {
    // Rate limit: max 1 FIND_NODE per second per sender
    {
        std::lock_guard lock(find_node_rate_mutex_);
        auto key = from + ":" + std::to_string(port);
        auto now = std::chrono::steady_clock::now();
        auto it = find_node_rate_.find(key);
        if (it != find_node_rate_.end() && now - it->second < std::chrono::seconds(1)) {
            spdlog::debug("FIND_NODE rate limited for {}:{}", from, port);
            return;
        }
        find_node_rate_[key] = now;
    }

    spdlog::debug("Received FIND_NODE from {}:{}", from, port);

    // Add the requesting node to our routing table.
    // The payload carries:
    //   pubkey_len(2 BE) || pubkey(N)
    //   [optional: addr_family(1) || addr(4 or 16) || ws_port(2 BE)]
    // The self-reported address is preferred over the TCP source IP,
    // which may be a LAN address when nodes share a network.
    {
        NodeInfo sender_info;
        sender_info.id = msg.sender;
        sender_info.address = from;
        sender_info.tcp_source_ip = from;
        sender_info.tcp_port = port;
        sender_info.ws_port = 0;
        sender_info.last_seen = std::chrono::steady_clock::now();

        // Parse optional pubkey from FIND_NODE payload
        const auto& data = msg.payload;
        size_t offset = 0;
        if (data.size() >= 2) {
            uint16_t pk_len = static_cast<uint16_t>(
                (static_cast<uint16_t>(data[0]) << 8) | data[1]);
            offset = 2;
            if (pk_len > 0 && data.size() >= 2 + pk_len) {
                std::vector<uint8_t> sender_pk(data.begin() + 2, data.begin() + 2 + pk_len);
                auto expected_id = crypto::sha3_256(sender_pk);
                if (msg.sender.id == expected_id) {
                    sender_info.pubkey = std::move(sender_pk);
                    spdlog::debug("FIND_NODE from {}:{} included valid pubkey", from, port);
                } else {
                    spdlog::warn("FIND_NODE from {}:{}: pubkey doesn't match sender_id, ignoring",
                                 from, port);
                }
                offset = 2 + pk_len;
            }

            // Parse optional self-reported address:
            // addr_family(1) || addr(4 or 16) || ws_port(2 BE)
            if (offset < data.size()) {
                uint8_t af = data[offset];
                offset += 1;
                size_t addr_len = (af == 0x06) ? 16 : 4;
                if (offset + addr_len + 2 <= data.size()) {
                    if (af == 0x06) {
                        struct in6_addr addr6{};
                        std::memcpy(&addr6, data.data() + offset, 16);
                        char addr_str[INET6_ADDRSTRLEN];
                        inet_ntop(AF_INET6, &addr6, addr_str, sizeof(addr_str));
                        sender_info.address = addr_str;
                    } else {
                        struct in_addr addr4{};
                        std::memcpy(&addr4.s_addr, data.data() + offset, 4);
                        char addr_str[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &addr4, addr_str, sizeof(addr_str));
                        sender_info.address = addr_str;
                    }
                    offset += addr_len;
                    sender_info.ws_port = static_cast<uint16_t>(
                        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
                    spdlog::debug("FIND_NODE from {}:{} advertises address {}:{} (ws:{})",
                                  from, port, sender_info.address, sender_info.tcp_port,
                                  sender_info.ws_port);
                }
            }
        }

        table_.add_or_update(sender_info);
    }

    // Build NODES payload: K closest nodes to the requester's ID + self.
    // Using the requester's ID as target ensures they get the most relevant
    // routing information. K=20 is a common Kademlia parameter.
    static constexpr size_t K_CLOSEST = 20;
    auto closest = table_.closest_to(msg.sender.id, K_CLOSEST);

    std::vector<NodeInfo> all;
    all.push_back(self_);
    for (auto& n : closest) {
        if (n.id == msg.sender) continue;  // don't include requester
        if (n.id == self_.id) continue;    // don't duplicate self
        all.push_back(std::move(n));
    }

    // Serialize NODES payload per spec:
    // [2 bytes BE: node_count]
    // Per node:
    //   [32 bytes: node_id]
    //   [1 byte: address_family (0x04=IPv4, 0x06=IPv6)]
    //   [4 or 16 bytes: address]
    //   [2 bytes BE: tcp_port]
    //   [2 bytes BE: ws_port]
    //   [2 bytes BE: pubkey_length]
    //   [pubkey_length bytes: ML-DSA public key]
    std::vector<uint8_t> payload;

    uint16_t node_count = static_cast<uint16_t>(all.size());
    payload.push_back(static_cast<uint8_t>((node_count >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(node_count & 0xFF));

    for (const auto& node : all) {
        // node_id (32 bytes)
        payload.insert(payload.end(), node.id.id.begin(), node.id.id.end());

        // Detect address family: contains ':' → IPv6, else IPv4
        bool is_ipv6 = node.address.find(':') != std::string::npos;

        if (is_ipv6) {
            payload.push_back(0x06);  // address_family = IPv6
            struct in6_addr addr6{};
            if (inet_pton(AF_INET6, node.address.c_str(), &addr6) != 1) {
                std::memset(&addr6, 0, sizeof(addr6));
            }
            auto* bytes = reinterpret_cast<const uint8_t*>(&addr6);
            payload.insert(payload.end(), bytes, bytes + 16);
        } else {
            payload.push_back(0x04);  // address_family = IPv4
            struct in_addr addr4{};
            if (inet_pton(AF_INET, node.address.c_str(), &addr4) != 1) {
                addr4.s_addr = 0;
            }
            auto* bytes = reinterpret_cast<const uint8_t*>(&addr4.s_addr);
            payload.insert(payload.end(), bytes, bytes + 4);
        }

        // tcp_port (2 bytes BE)
        payload.push_back(static_cast<uint8_t>((node.tcp_port >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(node.tcp_port & 0xFF));

        // ws_port (2 bytes BE)
        payload.push_back(static_cast<uint8_t>((node.ws_port >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(node.ws_port & 0xFF));

        // pubkey_length (2 bytes BE)
        uint16_t pk_len = static_cast<uint16_t>(node.pubkey.size());
        payload.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
        payload.push_back(static_cast<uint8_t>(pk_len & 0xFF));

        // pubkey bytes
        payload.insert(payload.end(), node.pubkey.begin(), node.pubkey.end());
    }

    Message reply = make_message(MessageType::NODES, payload);
    transport_.send(from, port, reply);
}

void Kademlia::handle_nodes(const Message& msg, const std::string& /*from*/, uint16_t /*port*/) {
    const auto& data = msg.payload;
    if (data.size() < 2) {
        spdlog::warn("NODES payload too short");
        return;
    }

    size_t offset = 0;
    uint16_t node_count = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    offset += 2;

    spdlog::info("Received NODES with {} entries", node_count);

    // Track newly discovered nodes for iterative FIND_NODE
    std::vector<NodeInfo> new_nodes;

    for (uint16_t i = 0; i < node_count; ++i) {
        // Minimum per-node: 32 (id) + 1 (af) + 4 (addr) + 2+2+2 = 43 bytes
        if (offset + 43 > data.size()) {
            spdlog::warn("NODES payload truncated at node {}", i);
            return;
        }

        NodeInfo info;

        // node_id (32 bytes)
        std::copy_n(data.data() + offset, 32, info.id.id.begin());
        offset += 32;

        // address_family (1 byte)
        uint8_t af = data[offset];
        offset += 1;

        if (af == 0x06) {
            // IPv6 (16 bytes)
            if (offset + 16 > data.size()) {
                spdlog::warn("NODES payload truncated at IPv6 for node {}", i);
                return;
            }
            struct in6_addr addr6{};
            std::memcpy(&addr6, data.data() + offset, 16);
            offset += 16;
            char addr_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &addr6, addr_str, sizeof(addr_str));
            info.address = addr_str;
        } else {
            // IPv4 (4 bytes) — treat 0x04 and any unknown af as IPv4
            if (offset + 4 > data.size()) {
                spdlog::warn("NODES payload truncated at IPv4 for node {}", i);
                return;
            }
            struct in_addr addr4{};
            std::memcpy(&addr4.s_addr, data.data() + offset, 4);
            offset += 4;
            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr4, addr_str, sizeof(addr_str));
            info.address = addr_str;
        }

        // tcp_port (2 bytes BE)
        if (offset + 6 > data.size()) {
            spdlog::warn("NODES payload truncated at ports for node {}", i);
            return;
        }
        info.tcp_port = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
        offset += 2;

        // ws_port (2 bytes BE)
        info.ws_port = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
        offset += 2;

        // pubkey_length (2 bytes BE)
        uint16_t pk_len = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
        offset += 2;

        if (offset + pk_len > data.size()) {
            spdlog::warn("NODES payload truncated at pubkey for node {}", i);
            return;
        }

        info.pubkey.assign(data.data() + offset, data.data() + offset + pk_len);
        offset += pk_len;

        info.last_seen = std::chrono::steady_clock::now();

        // Skip ourselves
        if (info.id == self_.id) continue;

        // Verify node_id == SHA3-256(pubkey) to prevent eclipse attacks.
        // Nodes with empty pubkeys are accepted — they were learned from
        // FIND_NODE requests which don't carry pubkeys. Their identity will
        // be verified when we interact with them directly.
        if (!info.pubkey.empty()) {
            auto expected_id = crypto::sha3_256(info.pubkey);
            if (info.id.id != expected_id) {
                spdlog::warn("Rejected node from NODES: id != SHA3-256(pubkey) for {}:{}",
                             info.address, info.tcp_port);
                continue;
            }
        }

        spdlog::info("Discovered node {} at {}:{}", i, info.address, info.tcp_port);

        // Track whether this is a new node (not already in our routing table)
        bool is_new = !table_.find(info.id).has_value();
        table_.add_or_update(info);
        if (is_new) {
            new_nodes.push_back(info);
        }
    }

    // Iterative discovery: send FIND_NODE to newly discovered nodes.
    // This is standard Kademlia behavior and also propagates our pubkey
    // (included in the FIND_NODE payload) so they can verify our future
    // signed messages (STORE, FIND_VALUE, etc.).
    if (!new_nodes.empty()) {
        auto fn_payload = make_find_node_payload();
        for (const auto& node : new_nodes) {
            Message find_msg = make_message(MessageType::FIND_NODE, fn_payload);
            send_to_node(node, find_msg);
        }
    }
}

// ---------------------------------------------------------------------------
// Unified local storage helper — validates, dispatches to correct table(s),
// appends to repl_log, and fires the on_store_ callback.
// ---------------------------------------------------------------------------

bool Kademlia::store_locally(const crypto::Hash& key, uint8_t data_type,
                             std::span<const uint8_t> value,
                             bool validate, bool log_and_notify) {
    // 1. Data-type-specific validation
    if (validate) {
        bool valid = true;
        switch (data_type) {
        case 0x00: valid = validate_profile(value, key);     break;
        case 0x01: valid = validate_name_record(value, key); break;
        case 0x02: valid = validate_inbox_message(value);    break;
        case 0x03: valid = validate_contact_request(value);  break;
        case 0x04: valid = validate_allowlist_entry(value);  break;
        case 0x05: valid = validate_group_message(value);    break;
        case 0x06: valid = validate_group_meta(value, key);  break;
        default: valid = false; break;
        }
        if (!valid) {
            spdlog::warn("store_locally: validation failed for data_type 0x{:02X}", data_type);
            return false;
        }
    }

    // 2. Dispatch to the correct storage table(s)
    if (data_type == 0x02) {
        // Inbox: two-table write
        // Value: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
        std::span<const uint8_t> recipient_fp(value.data(), 32);
        std::span<const uint8_t> msg_id(value.data() + 32, 32);
        std::span<const uint8_t> sender_fp(value.data() + 64, 32);
        uint32_t blob_len = (static_cast<uint32_t>(value[104]) << 24)
                          | (static_cast<uint32_t>(value[105]) << 16)
                          | (static_cast<uint32_t>(value[106]) << 8)
                          | static_cast<uint32_t>(value[107]);

        // Dedup: reject if msg_id already exists
        crypto::Hash mid{};
        std::copy_n(msg_id.data(), 32, mid.begin());
        if (storage_.get(storage::TABLE_MESSAGE_BLOBS, mid)) {
            spdlog::debug("store_locally: duplicate inbox message rejected (msg_id already exists)");
            return false;
        }

        // INDEX key: recipient_fp(32) || msg_id(32)
        std::vector<uint8_t> idx_key;
        idx_key.reserve(64);
        idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
        idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

        // INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE)
        std::vector<uint8_t> idx_value;
        idx_value.reserve(44);
        idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
        idx_value.insert(idx_value.end(), value.data() + 96, value.data() + 104); // timestamp
        idx_value.insert(idx_value.end(), value.data() + 104, value.data() + 108); // blob_len

        // BLOB key: msg_id(32), value: raw blob
        std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
        std::vector<uint8_t> blob_value(value.data() + 108, value.data() + 108 + blob_len);

        // Atomic write: INDEX + BLOB in single transaction
        storage_.batch_put({
            {storage::TABLE_INBOX_INDEX, idx_key, idx_value},
            {storage::TABLE_MESSAGE_BLOBS, blob_key, blob_value}
        });
    } else if (data_type == 0x04) {
        // Allowlist: composite key storage (co-located with inbox)
        // Value: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
        std::span<const uint8_t> allowed_fp(value.data() + 32, 32);
        uint8_t action = value[64];

        // Build composite storage key: routing_key(32) || allowed_fp(32)
        std::vector<uint8_t> composite_key;
        composite_key.reserve(64);
        composite_key.insert(composite_key.end(), key.begin(), key.end());
        composite_key.insert(composite_key.end(), allowed_fp.begin(), allowed_fp.end());

        // Both ALLOW (0x01) and REVOKE (0x00): store the full entry.
        // Revoke entries are kept so scan() still finds "allowlist exists"
        // even after all contacts are revoked.
        std::vector<uint8_t> entry_vec(value.begin(), value.end());
        storage_.put(storage::TABLE_ALLOWLISTS, composite_key, entry_vec);
    } else if (data_type == 0x03) {
        // Contact request: composite key storage
        // Value: recipient_fp(32) || sender_fp(32) || pow_nonce(8 BE) || blob_length(4 BE) || blob
        crypto::Hash rfp, sfp;
        std::copy_n(value.data(), 32, rfp.begin());
        std::copy_n(value.data() + 32, 32, sfp.begin());
        // Composite storage key: recipient_fp(32) || sender_fp(32)
        std::vector<uint8_t> storage_key;
        storage_key.reserve(64);
        storage_key.insert(storage_key.end(), rfp.begin(), rfp.end());
        storage_key.insert(storage_key.end(), sfp.begin(), sfp.end());
        std::vector<uint8_t> value_vec(value.begin(), value.end());
        storage_.put(storage::TABLE_REQUESTS, storage_key, value_vec);
    } else if (data_type == 0x05) {
        // GROUP_MESSAGE: two-table write (index + blob)
        // Value: group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob
        std::span<const uint8_t> group_id(value.data(), 32);
        std::span<const uint8_t> sender_fp(value.data() + 32, 32);
        std::span<const uint8_t> msg_id(value.data() + 64, 32);

        // gek_version at offset 104 (after timestamp[96..103])
        uint32_t gek_version = (static_cast<uint32_t>(value[104]) << 24)
                             | (static_cast<uint32_t>(value[105]) << 16)
                             | (static_cast<uint32_t>(value[106]) << 8)
                             | static_cast<uint32_t>(value[107]);
        // blob_len at offset 108
        uint32_t blob_len = (static_cast<uint32_t>(value[108]) << 24)
                          | (static_cast<uint32_t>(value[109]) << 16)
                          | (static_cast<uint32_t>(value[110]) << 8)
                          | static_cast<uint32_t>(value[111]);

        // Dedup: reject if this group message already exists
        std::vector<uint8_t> idx_key;
        idx_key.reserve(64);
        idx_key.insert(idx_key.end(), group_id.begin(), group_id.end());
        idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

        if (storage_.get(storage::TABLE_GROUP_BLOBS, idx_key)) {
            spdlog::debug("store_locally: duplicate group message rejected (msg_id already exists)");
            return false;
        }

        // INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE) || gek_version(4 BE) = 48 bytes
        std::vector<uint8_t> idx_value;
        idx_value.reserve(48);
        idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
        idx_value.insert(idx_value.end(), value.data() + 96, value.data() + 104);  // timestamp
        idx_value.insert(idx_value.end(), value.data() + 108, value.data() + 112); // blob_len (size)
        // gek_version (4 BE)
        idx_value.push_back(static_cast<uint8_t>((gek_version >> 24) & 0xFF));
        idx_value.push_back(static_cast<uint8_t>((gek_version >> 16) & 0xFF));
        idx_value.push_back(static_cast<uint8_t>((gek_version >> 8) & 0xFF));
        idx_value.push_back(static_cast<uint8_t>(gek_version & 0xFF));

        // BLOB value: raw encrypted blob (starts at offset 112)
        std::vector<uint8_t> blob_value(value.data() + 112, value.data() + 112 + blob_len);

        // Atomic write: INDEX + BLOB in single transaction
        storage_.batch_put({
            {storage::TABLE_GROUP_INDEX, idx_key, idx_value},
            {storage::TABLE_GROUP_BLOBS, idx_key, blob_value}
        });
    } else if (data_type == 0x06) {
        // GROUP_META: single-table write keyed by routing key (= SHA3-256("group:" || group_id)).
        // This matches profiles/names which also use the routing key as storage key,
        // allowing FIND_VALUE to look up group meta on remote nodes.
        std::vector<uint8_t> value_vec(value.begin(), value.end());
        storage_.put(storage::TABLE_GROUP_META, key, value_vec);
    } else {
        const char* table_name = nullptr;
        switch (data_type) {
        case 0x00: table_name = storage::TABLE_PROFILES;   break;
        case 0x01: table_name = storage::TABLE_NAMES;      break;
        default:
            spdlog::warn("store_locally: unknown data_type 0x{:02X}", data_type);
            return false;
        }

        std::vector<uint8_t> value_vec(value.begin(), value.end());
        storage_.put(table_name, key, value_vec);
    }

    // 3. Record mutation in the replication log + fire callback
    if (log_and_notify) {
        auto op = replication::Op::ADD;
        repl_log_.append(key, op, data_type, value);

        if (on_store_) {
            on_store_(key, data_type, value);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// STORE (section 2 of PROTOCOL-SPEC.md)
// ---------------------------------------------------------------------------

void Kademlia::handle_store(const Message& msg, const std::string& from, uint16_t port) {
    const auto& data = msg.payload;
    // Minimum: 32 (key) + 1 (data_type) + 4 (value_length) = 37 bytes
    if (data.size() < 37) {
        spdlog::warn("STORE payload too short from {}:{}", from, port);
        return;
    }

    size_t offset = 0;

    // key (32 bytes)
    crypto::Hash key{};
    std::copy_n(data.data() + offset, 32, key.begin());
    offset += 32;

    // data_type (1 byte)
    uint8_t data_type = data[offset];
    offset += 1;

    // value_length (4 bytes BE)
    uint32_t value_length = (static_cast<uint32_t>(data[offset]) << 24)
                          | (static_cast<uint32_t>(data[offset + 1]) << 16)
                          | (static_cast<uint32_t>(data[offset + 2]) << 8)
                          | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    if (offset + value_length > data.size()) {
        spdlog::warn("STORE value truncated from {}:{}", from, port);
        return;
    }

    std::span<const uint8_t> value(data.data() + offset, value_length);

    // 1. Responsibility check
    if (!is_responsible(key)) {
        spdlog::warn("STORE rejected: not responsible for key");
        std::vector<uint8_t> ack_payload;
        ack_payload.insert(ack_payload.end(), key.begin(), key.end());
        ack_payload.push_back(0x01); // rejected
        ack_payload.push_back(0x01); // reason: not responsible
        Message ack = make_message(MessageType::STORE_ACK, ack_payload);
        transport_.send(from, port, ack);
        return;
    }

    // 2. Empty value = remote delete request
    if (value_length == 0) {
        // Only GROUP_META (0x06) supports empty-value delete (GROUP_DESTROY).
        // All other data types must use the replication/sync path for deletes.
        if (data_type == 0x06) {
            // Authorization: only responsible peers may propagate GROUP_META deletes
            bool sender_is_responsible = false;
            {
                auto responsible = responsible_nodes(key);
                for (const auto& rn : responsible) {
                    if (rn.id == msg.sender) {
                        sender_is_responsible = true;
                        break;
                    }
                }
            }
            if (!sender_is_responsible) {
                spdlog::warn("STORE rejected: empty-value GROUP_META delete from non-responsible peer {}:{}", from, port);
                std::vector<uint8_t> ack_payload;
                ack_payload.insert(ack_payload.end(), key.begin(), key.end());
                ack_payload.push_back(0x01); // rejected
                ack_payload.push_back(0x03); // reason: unauthorized
                Message ack = make_message(MessageType::STORE_ACK, ack_payload);
                transport_.send(from, port, ack);
                return;
            }

            storage_.del(storage::TABLE_GROUP_META, key);
            repl_log_.append(key, replication::Op::DEL, data_type,
                             std::vector<uint8_t>(key.begin(), key.end()));
            spdlog::info("Remote GROUP_META delete for key from {}:{}", from, port);

            std::vector<uint8_t> ack_payload;
            ack_payload.insert(ack_payload.end(), key.begin(), key.end());
            ack_payload.push_back(0x00); // OK
            Message ack = make_message(MessageType::STORE_ACK, ack_payload);
            transport_.send(from, port, ack);
        } else {
            spdlog::warn("STORE rejected: empty-value delete not supported for data_type 0x{:02X}", data_type);
            std::vector<uint8_t> ack_payload;
            ack_payload.insert(ack_payload.end(), key.begin(), key.end());
            ack_payload.push_back(0x01); // rejected
            ack_payload.push_back(0x03); // reason: unauthorized/unsupported delete
            Message ack = make_message(MessageType::STORE_ACK, ack_payload);
            transport_.send(from, port, ack);
        }
        return;
    }

    // 3. Validate, store in table(s), append repl_log, fire on_store_
    if (!store_locally(key, data_type, value)) {
        std::vector<uint8_t> ack_payload;
        ack_payload.insert(ack_payload.end(), key.begin(), key.end());
        ack_payload.push_back(0x01); // rejected
        ack_payload.push_back(0x02); // reason: validation failed
        Message ack = make_message(MessageType::STORE_ACK, ack_payload);
        transport_.send(from, port, ack);
        return;
    }

    spdlog::info("Stored data_type=0x{:02X} for key from {}:{}", data_type, from, port);

    // Send STORE_ACK back: [32 bytes: key][1 byte: status=0x00 OK]
    std::vector<uint8_t> ack_payload;
    ack_payload.insert(ack_payload.end(), key.begin(), key.end());
    ack_payload.push_back(0x00); // OK
    Message ack = make_message(MessageType::STORE_ACK, ack_payload);
    transport_.send(from, port, ack);
}

// ---------------------------------------------------------------------------
// FIND_VALUE / VALUE (section 2)
// ---------------------------------------------------------------------------

void Kademlia::handle_find_value(const Message& msg, const std::string& from, uint16_t port) {
    if (msg.payload.size() < 32) {
        spdlog::warn("FIND_VALUE payload too short from {}:{}", from, port);
        return;
    }

    crypto::Hash key{};
    std::copy_n(msg.payload.data(), 32, key.begin());

    // Try relevant tables for the key. Inbox, contact requests, and allowlists
    // use composite keys and can't be looked up by routing key alone — they use
    // SYNC or scan() instead of FIND_VALUE.
    struct TableLookup { const char* table; uint8_t data_type; };
    static constexpr TableLookup lookups[] = {
        {storage::TABLE_PROFILES,   0x00},
        {storage::TABLE_NAMES,      0x01},
        {storage::TABLE_GROUP_META, 0x06},
    };

    std::vector<uint8_t> payload;
    // key (32 bytes)
    payload.insert(payload.end(), key.begin(), key.end());

    for (const auto& tl : lookups) {
        auto result = storage_.get(tl.table, key);
        if (result) {
            // Re-validate before serving — purge if corrupt
            if (!validate_readonly(key, tl.data_type, *result)) {
                spdlog::warn("handle_find_value: purging corrupt {} entry", tl.table);
                storage_.del(tl.table, key);
                continue;
            }

            // found = 0x01
            payload.push_back(0x01);
            // value_length (4 bytes BE)
            uint32_t vlen = static_cast<uint32_t>(result->size());
            payload.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
            payload.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
            payload.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
            payload.push_back(static_cast<uint8_t>(vlen & 0xFF));
            // value
            payload.insert(payload.end(), result->begin(), result->end());

            Message reply = make_message(MessageType::VALUE, payload);
            transport_.send(from, port, reply);
            return;
        }
    }

    // Not found
    payload.push_back(0x00); // found = 0x00
    // value_length = 0
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);

    Message reply = make_message(MessageType::VALUE, payload);
    transport_.send(from, port, reply);
}

void Kademlia::handle_value(const Message& msg, const std::string& from, uint16_t port) {
    // VALUE response format: key(32) || found(1) || value_length(4 BE) || value
    const auto& data = msg.payload;
    if (data.size() < 37) {
        spdlog::debug("VALUE payload too short from {}:{}", from, port);
        return;
    }

    crypto::Hash key{};
    std::copy_n(data.data(), 32, key.begin());
    uint8_t found = data[32];
    uint32_t vlen = (static_cast<uint32_t>(data[33]) << 24) |
                    (static_cast<uint32_t>(data[34]) << 16) |
                    (static_cast<uint32_t>(data[35]) << 8) |
                    static_cast<uint32_t>(data[36]);

    std::optional<std::vector<uint8_t>> value;
    if (found == 0x01 && data.size() >= 37 + vlen) {
        value = std::vector<uint8_t>(data.begin() + 37, data.begin() + 37 + vlen);
    }

    // Populate pending value query using sender NodeId from message header
    {
        std::lock_guard lock(value_query_mutex_);
        auto it = pending_value_queries_.find(key);
        if (it != pending_value_queries_.end()) {
            it->second.responses[msg.sender] = value;
            value_query_cv_.notify_all();
        }
    }

    spdlog::debug("Received VALUE from {}:{} found={} vlen={}", from, port, found, vlen);
}

// ---------------------------------------------------------------------------
// High-level: store()
// ---------------------------------------------------------------------------

bool Kademlia::store(const crypto::Hash& key, uint8_t data_type, std::span<const uint8_t> value) {
    auto nodes = responsible_nodes(key);
    if (nodes.empty()) return false;

    // Build STORE payload per spec
    std::vector<uint8_t> payload;
    // key (32 bytes)
    payload.insert(payload.end(), key.begin(), key.end());
    // data_type (1 byte)
    payload.push_back(data_type);
    // value_length (4 bytes BE)
    uint32_t vlen = static_cast<uint32_t>(value.size());
    payload.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(vlen & 0xFF));
    // value
    payload.insert(payload.end(), value.begin(), value.end());

    bool stored_any = false;
    bool local_stored = false;
    size_t remote_count = 0;

    for (const auto& node : nodes) {
        if (node.id == self_.id) {
            // Store locally — validate, dispatch, log, notify
            if (!store_locally(key, data_type, value)) {
                continue;
            }
            stored_any = true;
            local_stored = true;
        } else {
            // Send STORE to remote node (async — don't block)
            Message msg = make_message(MessageType::STORE, payload);
            send_to_node(node, msg);
            stored_any = true;
            ++remote_count;
        }
    }

    // Track pending replication if we sent STORE to any remote nodes
    if (remote_count > 0) {
        std::lock_guard lock(pending_mutex_);
        PendingStore ps;
        ps.expected = remote_count;
        ps.acked = 0;
        ps.local_stored = local_stored;
        ps.created = std::chrono::steady_clock::now();
        pending_stores_[key] = ps;
    }

    return stored_any;
}

// ---------------------------------------------------------------------------
// High-level: delete_value() — replicates a deletion via repl_log
// ---------------------------------------------------------------------------

void Kademlia::delete_value(const crypto::Hash& key, uint8_t data_type,
                             std::span<const uint8_t> delete_info) {
    // Record DEL in repl_log so it propagates via SYNC
    repl_log_.append(key, replication::Op::DEL, data_type,
                     std::vector<uint8_t>(delete_info.begin(), delete_info.end()));

    spdlog::debug("delete_value: recorded DEL for data_type 0x{:02X}", data_type);
}

void Kademlia::delete_remote(const crypto::Hash& key, uint8_t data_type) {
    // Send STORE with empty value to responsible nodes — they interpret
    // value_length=0 as a delete request for the given key + data_type.
    auto nodes = responsible_nodes(key);

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), key.begin(), key.end());
    payload.push_back(data_type);
    // value_length = 0 (4 bytes BE)
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);

    for (const auto& node : nodes) {
        if (node.id == self_.id) continue;  // already deleted locally
        Message msg = make_message(MessageType::STORE, payload);
        send_to_node(node, msg);
    }

    spdlog::debug("delete_remote: sent delete to {} responsible nodes for data_type 0x{:02X}",
                  nodes.size(), data_type);
}

// ---------------------------------------------------------------------------
// High-level: find_value()
// ---------------------------------------------------------------------------

std::optional<std::vector<uint8_t>> Kademlia::find_value(const crypto::Hash& key) {
    // First check locally. Inbox, contact requests, and allowlists use
    // composite keys — they're accessed via scan(), not find_value().
    const char* tables[] = {
        storage::TABLE_PROFILES, storage::TABLE_NAMES,
        storage::TABLE_GROUP_META,
    };

    for (const char* table : tables) {
        auto result = storage_.get(table, key);
        if (result) return result;
    }

    // Not found locally — send FIND_VALUE to responsible nodes and wait
    // Build FIND_VALUE payload: [32 bytes: key]
    std::vector<uint8_t> payload(key.begin(), key.end());

    auto nodes = responsible_nodes(key);
    for (const auto& node : nodes) {
        if (node.id == self_.id) continue;
        Message msg = make_message(MessageType::FIND_VALUE, payload);
        send_to_node(node, msg);
    }

    // In a real implementation we'd use futures/callbacks.
    // For now, return nullopt if not found locally.
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Responsibility
// ---------------------------------------------------------------------------

size_t Kademlia::replication_factor() const {
    size_t network_size = table_.size() + 1; // +1 for self
    return std::min(replication_factor_, network_size);
}

std::vector<NodeInfo> Kademlia::responsible_nodes(const crypto::Hash& key) const {
    // Collect self + all nodes, partial-sort by XOR distance to key, take top R
    std::vector<NodeInfo> all;
    all.push_back(self_);

    auto table_nodes = table_.all_nodes();
    for (auto& n : table_nodes) {
        // Avoid duplicating self if it's somehow in the table
        if (n.id == self_.id) continue;
        all.push_back(std::move(n));
    }

    NodeId target;
    target.id = key;

    size_t r = replication_factor();
    size_t count = std::min(r, all.size());

    std::partial_sort(all.begin(), all.begin() + count, all.end(),
        [&target](const NodeInfo& a, const NodeInfo& b) {
            return a.id.distance_to(target) < b.id.distance_to(target);
        });
    all.resize(count);
    return all;
}

bool Kademlia::is_responsible(const crypto::Hash& key) const {
    auto nodes = responsible_nodes(key);
    for (const auto& n : nodes) {
        if (n.id == self_.id) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Read-only validation (re-validate stored data without sequence/timestamp checks)
// ---------------------------------------------------------------------------

bool Kademlia::validate_readonly(const crypto::Hash& key, uint8_t data_type,
                                  std::span<const uint8_t> value) {
    switch (data_type) {
    case 0x00: return validate_profile(value, key, /*skip_sequence_check=*/true);
    case 0x01: return validate_name_record(value, key, /*skip_sequence_check=*/true);
    case 0x02: return validate_inbox_message(value);
    case 0x03: return validate_contact_request(value, /*skip_timestamp_check=*/true);
    case 0x04: return validate_allowlist_entry(value, /*skip_storage_lookup=*/true);
    case 0x05: return validate_group_message(value);
    case 0x06: return validate_group_meta(value, key, /*skip_storage_lookup=*/true);
    default:   return false;
    }
}

// ---------------------------------------------------------------------------
// Name record validation (PROTOCOL-SPEC.md section 5)
// ---------------------------------------------------------------------------

bool Kademlia::validate_name_record(std::span<const uint8_t> value, const crypto::Hash& key,
                                     bool skip_sequence_check) {
    // Parse name record:
    // [1 byte: name_length]
    // [name_length bytes: name (ASCII, lowercase)]
    // [32 bytes: fingerprint]
    // [8 bytes BE: pow_nonce]
    // [8 bytes BE: sequence]
    // [2 bytes BE: pubkey_length]
    // [pubkey_length bytes: ML-DSA-87 public key]
    // [2 bytes BE: signature_length]
    // [signature_length bytes: ML-DSA-87 signature over all preceding fields]

    if (value.size() < 1) return false;

    size_t offset = 0;

    // name_length
    uint8_t name_length = value[offset];
    offset += 1;

    if (offset + name_length > value.size()) return false;

    // name
    std::string name(reinterpret_cast<const char*>(value.data() + offset), name_length);
    offset += name_length;

    // 1. Name regex: ^[a-z0-9]{3,36}$
    static const std::regex name_regex("^[a-z0-9]{3,36}$");
    if (!std::regex_match(name, name_regex)) {
        spdlog::warn("Name record validation: name '{}' does not match regex", name);
        return false;
    }

    // Need at least: 32 (fingerprint) + 8 (nonce) + 8 (sequence) + 2 (pubkey_len) = 50
    if (offset + 50 > value.size()) return false;

    // fingerprint (32 bytes)
    crypto::Hash fingerprint{};
    std::copy_n(value.data() + offset, 32, fingerprint.begin());
    offset += 32;

    // pow_nonce (8 bytes BE)
    uint64_t pow_nonce = 0;
    for (int i = 0; i < 8; ++i) {
        pow_nonce = (pow_nonce << 8) | value[offset + i];
    }
    offset += 8;

    // sequence (8 bytes BE)
    uint64_t sequence = 0;
    for (int i = 0; i < 8; ++i) {
        sequence = (sequence << 8) | value[offset + i];
    }
    offset += 8;

    // pubkey_length (2 bytes BE)
    if (offset + 2 > value.size()) return false;
    uint16_t pk_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1]);
    offset += 2;

    if (offset + pk_len > value.size()) return false;
    std::span<const uint8_t> pubkey(value.data() + offset, pk_len);
    offset += pk_len;

    // Verify fingerprint == SHA3-256(pubkey)
    auto computed_fp = crypto::sha3_256(pubkey);
    if (computed_fp != fingerprint) {
        spdlog::warn("Name record validation: fingerprint does not match SHA3-256(pubkey) for '{}'", name);
        return false;
    }

    // sig_length (2 bytes BE)
    if (offset + 2 > value.size()) return false;
    uint16_t sig_length = static_cast<uint16_t>(
        (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1]);
    offset += 2;

    if (offset + sig_length > value.size()) return false;

    std::span<const uint8_t> signature(value.data() + offset, sig_length);

    // 2. PoW: SHA3-256("chromatin:name:" || name || fingerprint || nonce) >= required zero bits
    // Build PoW preimage: "chromatin:name:" || name || fingerprint
    std::string prefix = "chromatin:name:";
    std::vector<uint8_t> pow_preimage;
    pow_preimage.insert(pow_preimage.end(), prefix.begin(), prefix.end());
    pow_preimage.insert(pow_preimage.end(), name.begin(), name.end());
    pow_preimage.insert(pow_preimage.end(), fingerprint.begin(), fingerprint.end());

    if (!crypto::verify_pow(pow_preimage, pow_nonce, name_pow_difficulty_)) {
        spdlog::warn("Name record validation: PoW check failed for '{}'", name);
        return false;
    }

    // 3. ML-DSA signature over all fields preceding the signature
    // signed_data = everything before sig_length field
    size_t pre_sig_len = 1 + name_length + 32 + 8 + 8 + 2 + pk_len;
    std::span<const uint8_t> signed_data(value.data(), pre_sig_len);

    if (!crypto::verify(signed_data, signature, pubkey)) {
        spdlog::warn("Name record validation: signature verification failed for '{}'", name);
        return false;
    }

    // 4. Conflict resolution for existing name records
    if (!skip_sequence_check) {
        auto existing = storage_.get(storage::TABLE_NAMES, key);
        if (existing) {
            if (existing->size() >= 1) {
                uint8_t existing_name_len = (*existing)[0];
                size_t fp_offset = 1 + existing_name_len;
                if (existing->size() >= fp_offset + 32) {
                    crypto::Hash existing_fp{};
                    std::copy_n(existing->data() + fp_offset, 32, existing_fp.begin());

                    if (existing_fp != fingerprint) {
                        // Different fingerprint claiming the same name.
                        // Deterministic tiebreaker: lower fingerprint wins.
                        // This ensures all nodes converge to the same owner
                        // even if STOREs arrive in different order during races.
                        if (fingerprint < existing_fp) {
                            spdlog::info("Name record conflict for '{}': incoming fp wins (lower)", name);
                            // Accept — incoming record replaces existing
                        } else {
                            spdlog::info("Name record conflict for '{}': existing fp wins (lower)", name);
                            return false;
                        }
                    } else {
                        // 5. Same fingerprint: sequence must be higher (owner update)
                        size_t existing_seq_offset = fp_offset + 32 + 8; // skip fingerprint + pow_nonce
                        if (existing->size() >= existing_seq_offset + 8) {
                            uint64_t existing_seq = 0;
                            for (int i = 0; i < 8; ++i) {
                                existing_seq = (existing_seq << 8) | (*existing)[existing_seq_offset + i];
                            }
                            if (sequence <= existing_seq) {
                                spdlog::warn("Name record validation: sequence {} <= existing {} for '{}'",
                                             sequence, existing_seq, name);
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }

    return true;
}

// Extract sequence number from a profile binary, or nullopt if malformed.
// Walks through all variable-length fields to locate the 8-byte BE sequence.
static std::optional<uint64_t> extract_profile_sequence(std::span<const uint8_t> profile) {
    if (profile.size() < 53) return std::nullopt;

    size_t off = 32; // skip fingerprint

    // pubkey_len(2) + pubkey
    if (off + 2 > profile.size()) return std::nullopt;
    uint16_t pk_len = (static_cast<uint16_t>(profile[off]) << 8) | profile[off + 1];
    off += 2;
    if (off + pk_len > profile.size()) return std::nullopt;
    off += pk_len;

    // kem_pubkey_len(2) + kem_pubkey
    if (off + 2 > profile.size()) return std::nullopt;
    uint16_t kem_len = (static_cast<uint16_t>(profile[off]) << 8) | profile[off + 1];
    off += 2;
    if (off + kem_len > profile.size()) return std::nullopt;
    off += kem_len;

    // bio_len(2) + bio
    if (off + 2 > profile.size()) return std::nullopt;
    uint16_t bio_len = (static_cast<uint16_t>(profile[off]) << 8) | profile[off + 1];
    off += 2;
    if (off + bio_len > profile.size()) return std::nullopt;
    off += bio_len;

    // avatar_len(4) + avatar
    if (off + 4 > profile.size()) return std::nullopt;
    uint32_t avatar_len = (static_cast<uint32_t>(profile[off]) << 24)
                        | (static_cast<uint32_t>(profile[off + 1]) << 16)
                        | (static_cast<uint32_t>(profile[off + 2]) << 8)
                        | static_cast<uint32_t>(profile[off + 3]);
    off += 4;
    if (off + avatar_len > profile.size()) return std::nullopt;
    off += avatar_len;

    // social_count(1) + social links
    if (off + 1 > profile.size()) return std::nullopt;
    uint8_t social_count = profile[off];
    off += 1;
    for (uint8_t i = 0; i < social_count; ++i) {
        if (off + 1 > profile.size()) return std::nullopt;
        uint8_t platform_len = profile[off]; off += 1;
        if (off + platform_len > profile.size()) return std::nullopt;
        off += platform_len;
        if (off + 1 > profile.size()) return std::nullopt;
        uint8_t handle_len = profile[off]; off += 1;
        if (off + handle_len > profile.size()) return std::nullopt;
        off += handle_len;
    }

    // sequence(8 BE)
    if (off + 8 > profile.size()) return std::nullopt;
    uint64_t seq = 0;
    for (int i = 0; i < 8; ++i) {
        seq = (seq << 8) | profile[off + i];
    }
    return seq;
}

// ---------------------------------------------------------------------------
// Profile validation (PROTOCOL-SPEC.md section 3)
// ---------------------------------------------------------------------------

bool Kademlia::validate_profile(std::span<const uint8_t> value, const crypto::Hash& key,
                                 bool skip_sequence_check) {
    if (value.size() > max_profile_size_) {
        spdlog::warn("Profile validation: size {} exceeds {} byte limit", value.size(), max_profile_size_);
        return false;
    }

    // Minimum: fingerprint(32) + pubkey_len(2) + kem_pubkey_len(2) + bio_len(2)
    //          + avatar_len(4) + social_count(1) + sequence(8) + sig_len(2) = 53
    if (value.size() < 53) {
        spdlog::warn("Profile validation: too short ({} bytes)", value.size());
        return false;
    }

    size_t offset = 0;

    // fingerprint (32 bytes)
    crypto::Hash fingerprint{};
    std::copy_n(value.data(), 32, fingerprint.begin());
    offset += 32;

    // pubkey_length (2 bytes BE)
    uint16_t pk_len = (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1];
    offset += 2;
    if (offset + pk_len > value.size()) return false;
    std::span<const uint8_t> pubkey(value.data() + offset, pk_len);
    offset += pk_len;

    // Verify fingerprint == SHA3-256(pubkey)
    auto computed_fp = crypto::sha3_256(pubkey);
    if (computed_fp != fingerprint) {
        spdlog::warn("Profile validation: fingerprint mismatch");
        return false;
    }

    // Verify storage key == SHA3-256("profile:" || fingerprint)
    auto expected_key = crypto::sha3_256_prefixed("profile:", fingerprint);
    if (expected_key != key) {
        spdlog::warn("Profile validation: storage key mismatch");
        return false;
    }

    // Skip kem_pubkey
    if (offset + 2 > value.size()) return false;
    uint16_t kem_len = (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1];
    offset += 2;
    if (offset + kem_len > value.size()) return false;
    offset += kem_len;

    // bio
    if (offset + 2 > value.size()) return false;
    uint16_t bio_len = (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1];
    offset += 2;
    if (offset + bio_len > value.size()) return false;
    if (bio_len > config::protocol::MAX_BIO_SIZE) {
        spdlog::warn("Profile validation: bio size {} exceeds {} byte limit", bio_len, config::protocol::MAX_BIO_SIZE);
        return false;
    }
    offset += bio_len;

    // avatar
    if (offset + 4 > value.size()) return false;
    uint32_t avatar_len = (static_cast<uint32_t>(value[offset]) << 24)
                        | (static_cast<uint32_t>(value[offset + 1]) << 16)
                        | (static_cast<uint32_t>(value[offset + 2]) << 8)
                        | static_cast<uint32_t>(value[offset + 3]);
    offset += 4;
    if (offset + avatar_len > value.size()) return false;
    if (avatar_len > config::protocol::MAX_AVATAR_SIZE) {
        spdlog::warn("Profile validation: avatar size {} exceeds {} byte limit", avatar_len, config::protocol::MAX_AVATAR_SIZE);
        return false;
    }
    offset += avatar_len;

    // social links
    if (offset + 1 > value.size()) return false;
    uint8_t social_count = value[offset];
    offset += 1;
    if (social_count > config::protocol::MAX_SOCIAL_LINKS) {
        spdlog::warn("Profile validation: social link count {} exceeds {} limit", social_count, config::protocol::MAX_SOCIAL_LINKS);
        return false;
    }
    for (uint8_t i = 0; i < social_count; ++i) {
        if (offset + 1 > value.size()) return false;
        uint8_t platform_len = value[offset]; offset += 1;
        if (offset + platform_len > value.size()) return false;
        if (platform_len > config::protocol::MAX_SOCIAL_PLATFORM_LENGTH) {
            spdlog::warn("Profile validation: social platform length {} exceeds {} byte limit", platform_len, config::protocol::MAX_SOCIAL_PLATFORM_LENGTH);
            return false;
        }
        offset += platform_len;
        if (offset + 1 > value.size()) return false;
        uint8_t handle_len = value[offset]; offset += 1;
        if (offset + handle_len > value.size()) return false;
        if (handle_len > config::protocol::MAX_SOCIAL_HANDLE_LENGTH) {
            spdlog::warn("Profile validation: social handle length {} exceeds {} byte limit", handle_len, config::protocol::MAX_SOCIAL_HANDLE_LENGTH);
            return false;
        }
        offset += handle_len;
    }

    // sequence (8 bytes BE)
    if (offset + 8 > value.size()) return false;
    uint64_t sequence = 0;
    for (int i = 0; i < 8; ++i) {
        sequence = (sequence << 8) | value[offset + i];
    }
    offset += 8;

    // sig_length (2 bytes BE)
    if (offset + 2 > value.size()) return false;
    uint16_t sig_len = (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1];
    offset += 2;
    if (offset + sig_len > value.size()) return false;

    std::span<const uint8_t> signature(value.data() + offset, sig_len);
    std::span<const uint8_t> signed_data(value.data(), offset - 2 - sig_len);

    // Verify ML-DSA-87 signature
    // signed_data = everything before sig_len field
    size_t pre_sig_offset = offset - 2; // before sig_len
    if (!crypto::verify(std::span<const uint8_t>(value.data(), pre_sig_offset), signature, pubkey)) {
        spdlog::warn("Profile validation: signature verification failed");
        return false;
    }

    // Sequence monotonicity: reject if new sequence <= existing sequence
    if (!skip_sequence_check) {
        auto existing = storage_.get(storage::TABLE_PROFILES, key);
        if (existing && !existing->empty()) {
            auto existing_seq = extract_profile_sequence(*existing);
            if (existing_seq && sequence <= *existing_seq) {
                spdlog::debug("Profile rejected: sequence {} <= existing {}", sequence, *existing_seq);
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Inbox message validation
// ---------------------------------------------------------------------------

bool Kademlia::validate_inbox_message(std::span<const uint8_t> value) {
    // Format: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    // Minimum: 32 + 32 + 32 + 8 + 4 = 108 bytes
    if (value.size() < 108) {
        spdlog::warn("Inbox validation: too short ({} bytes)", value.size());
        return false;
    }

    if (value.size() > config::protocol::MAX_MESSAGE_SIZE) {
        spdlog::warn("Inbox validation: size {} exceeds 50 MiB limit", value.size());
        return false;
    }

    // Parse blob_len and verify it matches remaining data
    size_t offset = 32 + 32 + 32 + 8; // skip recipient_fp + msg_id + sender_fp + timestamp
    uint32_t blob_len = (static_cast<uint32_t>(value[offset]) << 24)
                      | (static_cast<uint32_t>(value[offset + 1]) << 16)
                      | (static_cast<uint32_t>(value[offset + 2]) << 8)
                      | static_cast<uint32_t>(value[offset + 3]);
    offset += 4;

    if (offset + blob_len != value.size()) {
        spdlog::warn("Inbox validation: blob_len {} doesn't match remaining data {}", blob_len, value.size() - offset);
        return false;
    }

    // Allowlist enforcement: reject if recipient has an allowlist and sender is not on it.
    // Extract recipient_fp (bytes 0-31) and sender_fp (bytes 64-95)
    std::span<const uint8_t> recipient_fp = value.subspan(0, 32);
    std::span<const uint8_t> sender_fp    = value.subspan(64, 32);

    // Compute allowlist routing key: co-located with inbox on SHA3-256("inbox:" || recipient_fp)
    auto allowlist_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);

    // Build composite lookup key: allowlist_key(32) || sender_fp(32)
    std::vector<uint8_t> composite_key;
    composite_key.reserve(64);
    composite_key.insert(composite_key.end(), allowlist_key.begin(), allowlist_key.end());
    composite_key.insert(composite_key.end(), sender_fp.begin(), sender_fp.end());

    // Point lookup: is sender explicitly on the allowlist with active ALLOW?
    auto allowed = storage_.get(storage::TABLE_ALLOWLISTS, composite_key);
    if (allowed && allowed->size() > 64 && (*allowed)[64] == 0x01) {
        return true;  // Sender has active ALLOW entry
    }

    // Sender not found — check if ANY allowlist entries exist for this recipient.
    // If no allowlist exists at all, this is a new user with no allowlist configured → allow.
    bool has_any_entry = false;
    storage_.scan(storage::TABLE_ALLOWLISTS, allowlist_key, [&](std::span<const uint8_t>, std::span<const uint8_t>) {
        has_any_entry = true;
        return false;  // Stop after first match — we only need to know if any entry exists
    });

    if (has_any_entry) {
        spdlog::warn("Inbox validation: sender not on recipient's allowlist");
        return false;  // Allowlist exists but sender is not on it → reject
    }

    // No allowlist configured for this recipient → allow (open inbox)
    return true;
}

// ---------------------------------------------------------------------------
// Contact request validation
// ---------------------------------------------------------------------------

bool Kademlia::validate_contact_request(std::span<const uint8_t> value,
                                         bool skip_timestamp_check) {
    // Format: recipient_fp(32) || sender_fp(32) || pow_nonce(8 BE) || timestamp(8 BE) || blob_length(4 BE) || blob
    // Minimum: 32 + 32 + 8 + 8 + 4 = 84 bytes
    if (value.size() < 84) {
        spdlog::warn("Contact request validation: too short ({} bytes)", value.size());
        return false;
    }

    // Extract recipient_fp and sender_fp
    std::span<const uint8_t> recipient_fp = value.subspan(0, 32);
    std::span<const uint8_t> sender_fp = value.subspan(32, 32);

    // Extract pow_nonce (8 bytes BE at offset 64)
    uint64_t pow_nonce = 0;
    for (int i = 0; i < 8; ++i) {
        pow_nonce = (pow_nonce << 8) | value[64 + i];
    }

    // Extract timestamp (8 bytes BE at offset 72)
    uint64_t timestamp = 0;
    for (int i = 0; i < 8; ++i) {
        timestamp = (timestamp << 8) | value[72 + i];
    }

    // Validate timestamp: must be within 1 hour of current time
    if (!skip_timestamp_check) {
        auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        constexpr uint64_t MAX_TIMESTAMP_DRIFT = 3'600'000;  // 1 hour in ms
        if (timestamp > now + MAX_TIMESTAMP_DRIFT || now > timestamp + MAX_TIMESTAMP_DRIFT) {
            spdlog::debug("Contact request rejected: timestamp {} too far from now {}", timestamp, now);
            return false;
        }
    }

    // Verify PoW with domain separation:
    // preimage = "chromatin:request:" || sender_fp || recipient_fp || timestamp(8 BE)
    std::vector<uint8_t> preimage;
    const std::string prefix = "chromatin:request:";
    preimage.reserve(prefix.size() + 32 + 32 + 8);
    preimage.insert(preimage.end(), prefix.begin(), prefix.end());
    preimage.insert(preimage.end(), sender_fp.begin(), sender_fp.end());
    preimage.insert(preimage.end(), recipient_fp.begin(), recipient_fp.end());
    for (int i = 7; i >= 0; --i) {
        preimage.push_back(
            static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    }

    if (!crypto::verify_pow(preimage, pow_nonce, contact_pow_difficulty_)) {
        spdlog::debug("Contact request rejected: insufficient PoW (required {} bits)", contact_pow_difficulty_);
        return false;
    }

    // Validate blob_length (at offset 80 now)
    uint32_t blob_len = (static_cast<uint32_t>(value[80]) << 24)
                      | (static_cast<uint32_t>(value[81]) << 16)
                      | (static_cast<uint32_t>(value[82]) << 8)
                      | static_cast<uint32_t>(value[83]);

    if (blob_len > max_request_blob_size_) {
        spdlog::warn("Contact request validation: blob_len {} exceeds {} bytes", blob_len, max_request_blob_size_);
        return false;
    }

    if (84 + blob_len != value.size()) {
        spdlog::warn("Contact request validation: blob_len {} doesn't match remaining data", blob_len);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Allowlist entry validation
// ---------------------------------------------------------------------------

bool Kademlia::validate_allowlist_entry(std::span<const uint8_t> value, bool skip_storage_lookup) {
    // Format: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || pubkey_len(2 BE) || pubkey || signature
    // Minimum: 32 + 32 + 1 + 8 + 2 = 75 bytes + pubkey + signature
    if (value.size() < 75) {
        spdlog::warn("Allowlist validation: too short ({} bytes)", value.size());
        return false;
    }

    std::span<const uint8_t> owner_fp = value.subspan(0, 32);
    std::span<const uint8_t> allowed_fp = value.subspan(32, 32);
    uint8_t action = value[64];
    if (action != 0x00 && action != 0x01) {
        spdlog::warn("Allowlist validation: invalid action byte 0x{:02X}", action);
        return false;
    }

    // Extract pubkey from the entry itself (self-contained, no profile lookup needed)
    uint16_t pk_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(value[73]) << 8) | value[74]);
    if (value.size() < 75u + pk_len) {
        spdlog::warn("Allowlist validation: truncated pubkey (pk_len={}, remaining={})", pk_len, value.size() - 75);
        return false;
    }
    std::span<const uint8_t> pubkey = value.subspan(75, pk_len);

    // Verify that pubkey hashes to owner_fp
    auto computed_fp = crypto::sha3_256(pubkey);
    if (!std::equal(computed_fp.begin(), computed_fp.end(), owner_fp.begin())) {
        spdlog::warn("Allowlist validation: embedded pubkey does not match owner fingerprint");
        return false;
    }

    // Signed data with domain separation:
    // "chromatin:allowlist:" || owner_fp(32) || action(1) || allowed_fp(32) || sequence(8 BE)
    const std::string domain = "chromatin:allowlist:";
    std::vector<uint8_t> signed_data;
    signed_data.reserve(domain.size() + 32 + 1 + 32 + 8);
    signed_data.insert(signed_data.end(), domain.begin(), domain.end());
    signed_data.insert(signed_data.end(), owner_fp.begin(), owner_fp.end());
    signed_data.push_back(action);
    signed_data.insert(signed_data.end(), allowed_fp.begin(), allowed_fp.end());
    signed_data.insert(signed_data.end(), value.data() + 65, value.data() + 73);

    // Signature starts after pubkey
    std::span<const uint8_t> signature = value.subspan(75 + pk_len);
    if (!crypto::verify(signed_data, signature, pubkey)) {
        spdlog::warn("Allowlist validation: invalid signature");
        return false;
    }

    // Sequence monotonicity: reject if incoming sequence <= existing sequence
    // for the same owner_fp + allowed_fp pair
    if (!skip_storage_lookup) {
        uint64_t sequence = 0;
        for (int i = 0; i < 8; ++i) {
            sequence = (sequence << 8) | value[65 + i];
        }

        // Build composite key to look up existing entry
        crypto::Hash ofp{};
        std::copy_n(value.data(), 32, ofp.begin());
        auto inbox_key = crypto::sha3_256_prefixed("inbox:", ofp);
        std::vector<uint8_t> composite_key;
        composite_key.reserve(64);
        composite_key.insert(composite_key.end(), inbox_key.begin(), inbox_key.end());
        composite_key.insert(composite_key.end(), value.data() + 32, value.data() + 64);

        auto existing = storage_.get(storage::TABLE_ALLOWLISTS, composite_key);
        if (existing && !existing->empty() && existing->size() >= 73) {
            uint64_t existing_seq = 0;
            for (int i = 0; i < 8; ++i) {
                existing_seq = (existing_seq << 8) | (*existing)[65 + i];
            }
            if (sequence <= existing_seq) {
                spdlog::debug("Allowlist rejected: sequence {} <= existing {}", sequence, existing_seq);
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Group message validation
// ---------------------------------------------------------------------------

bool Kademlia::validate_group_message(std::span<const uint8_t> value) {
    // Format: group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE)
    //         || gek_version(4 BE) || blob_len(4 BE) || blob
    // Minimum: 32 + 32 + 32 + 8 + 4 + 4 = 112 bytes
    if (value.size() < 112) {
        spdlog::warn("Group message validation: too short ({} bytes)", value.size());
        return false;
    }

    if (value.size() > config::protocol::MAX_MESSAGE_SIZE) {
        spdlog::warn("Group message validation: size {} exceeds max message size", value.size());
        return false;
    }

    // Parse blob_len at offset 108 (after timestamp[96..103] + gek_version[104..107])
    uint32_t blob_len = (static_cast<uint32_t>(value[108]) << 24)
                      | (static_cast<uint32_t>(value[109]) << 16)
                      | (static_cast<uint32_t>(value[110]) << 8)
                      | static_cast<uint32_t>(value[111]);

    if (112 + blob_len != value.size()) {
        spdlog::warn("Group message validation: blob_len {} doesn't match remaining data {}", blob_len, value.size() - 112);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Group meta validation
// ---------------------------------------------------------------------------

bool Kademlia::validate_group_meta(std::span<const uint8_t> value, const crypto::Hash& key, bool skip_storage_lookup) {
    // Format: group_id(32) || owner_fp(32) || signer_fp(32) || version(4 BE) || member_count(2 BE) ||
    //         per-member[fp(32) + role(1) + kem_ciphertext(1568)] * member_count ||
    //         sig_len(2 BE) || ML-DSA-87 signature
    // Minimum header: 32 + 32 + 32 + 4 + 2 = 102 bytes
    constexpr size_t HEADER_SIZE = 102;
    if (value.size() < HEADER_SIZE) {
        spdlog::warn("Group meta validation: too short ({} bytes)", value.size());
        return false;
    }

    // Parse member_count at offset 100
    uint16_t member_count = (static_cast<uint16_t>(value[100]) << 8)
                          | static_cast<uint16_t>(value[101]);

    if (member_count == 0 || member_count > 512) {
        spdlog::warn("Group meta validation: invalid member_count {}", member_count);
        return false;
    }

    // Per-member entry size: fingerprint(32) + role(1) + kem_ciphertext(1568) = 1601
    constexpr size_t MEMBER_ENTRY_SIZE = 1601;
    size_t members_end = HEADER_SIZE + static_cast<size_t>(member_count) * MEMBER_ENTRY_SIZE;

    // Need at least members_end + sig_len(2)
    if (value.size() < members_end + 2) {
        spdlog::warn("Group meta validation: too short for {} members (need {}, have {})",
                      member_count, members_end + 2, value.size());
        return false;
    }

    // Parse sig_len
    uint16_t sig_len = (static_cast<uint16_t>(value[members_end]) << 8)
                     | static_cast<uint16_t>(value[members_end + 1]);

    if (value.size() != members_end + 2 + sig_len) {
        spdlog::warn("Group meta validation: sig_len {} doesn't match remaining data", sig_len);
        return false;
    }

    // Verify at least one member has role=0x02 (owner)
    bool has_owner = false;
    for (uint16_t i = 0; i < member_count; ++i) {
        size_t entry_offset = HEADER_SIZE + static_cast<size_t>(i) * MEMBER_ENTRY_SIZE;
        uint8_t role = value[entry_offset + 32]; // role byte after fingerprint
        if (role == 0x02) {
            has_owner = true;
            break;
        }
        if (role > 0x02) {
            spdlog::warn("Group meta validation: invalid role 0x{:02X} for member {}", role, i);
            return false;
        }
    }

    if (!has_owner) {
        spdlog::warn("Group meta validation: no owner in member list");
        return false;
    }

    // Verify key derivation: key must equal SHA3-256("group:" || group_id)
    std::span<const uint8_t> group_id(value.data(), 32);
    auto expected_key = crypto::sha3_256_prefixed("group:", group_id);
    if (key != expected_key) {
        spdlog::warn("Group meta validation: key mismatch (expected SHA3-256('group:' || group_id))");
        return false;
    }

    // Parse signer_fp (bytes 64-95)
    crypto::Hash signer_fp{};
    std::copy_n(value.data() + 64, 32, signer_fp.begin());

    // Version monotonicity: reject if incoming version <= existing version
    // Also verify signer authorization against EXISTING meta's member list
    if (!skip_storage_lookup) {
        uint32_t new_version = (static_cast<uint32_t>(value[96]) << 24)
                             | (static_cast<uint32_t>(value[97]) << 16)
                             | (static_cast<uint32_t>(value[98]) << 8)
                             |  static_cast<uint32_t>(value[99]);

        auto existing = storage_.get(storage::TABLE_GROUP_META, key);
        if (existing && existing->size() >= HEADER_SIZE) {
            uint32_t existing_version = (static_cast<uint32_t>((*existing)[96]) << 24)
                                      | (static_cast<uint32_t>((*existing)[97]) << 16)
                                      | (static_cast<uint32_t>((*existing)[98]) << 8)
                                      |  static_cast<uint32_t>((*existing)[99]);
            if (new_version <= existing_version) {
                spdlog::debug("Group meta rejected: version {} <= existing {}", new_version, existing_version);
                return false;
            }

            // Verify signer has role >= 0x01 (admin/owner) in EXISTING meta's member list
            uint16_t existing_mc = (static_cast<uint16_t>((*existing)[100]) << 8)
                                 | static_cast<uint16_t>((*existing)[101]);
            bool signer_authorized = false;
            for (uint16_t i = 0; i < existing_mc; ++i) {
                size_t eo = HEADER_SIZE + static_cast<size_t>(i) * MEMBER_ENTRY_SIZE;
                if (eo + 33 > existing->size()) break;
                crypto::Hash member_fp{};
                std::copy_n(existing->data() + eo, 32, member_fp.begin());
                if (member_fp == signer_fp && (*existing)[eo + 32] >= 0x01) {
                    signer_authorized = true;
                    break;
                }
            }
            if (!signer_authorized) {
                spdlog::warn("Group meta validation: signer not admin/owner in existing meta");
                return false;
            }
        } else {
            // No existing meta (creation): verify signer has role >= 0x01 in NEW meta
            bool signer_in_new = false;
            for (uint16_t i = 0; i < member_count; ++i) {
                size_t eo = HEADER_SIZE + static_cast<size_t>(i) * MEMBER_ENTRY_SIZE;
                crypto::Hash member_fp{};
                std::copy_n(value.data() + eo, 32, member_fp.begin());
                if (member_fp == signer_fp && value[eo + 32] >= 0x01) {
                    signer_in_new = true;
                    break;
                }
            }
            if (!signer_in_new) {
                spdlog::warn("Group meta validation: signer not admin/owner in new meta (creation)");
                return false;
            }
        }
    }

    // Verify ML-DSA-87 signature using signer's pubkey from local profile store
    if (skip_storage_lookup) {
        // Called from within a foreach/scan callback — cannot nest read transactions.
        // Integrity sweep will re-validate later.
        return true;
    }

    auto signer_profile_key = crypto::sha3_256_prefixed("profile:", signer_fp);

    auto profile_data = storage_.get(storage::TABLE_PROFILES, signer_profile_key);
    if (!profile_data || profile_data->empty()) {
        spdlog::warn("Group meta validation: rejected — signer profile not found locally");
        return false;
    }

    // Extract pubkey from profile: fingerprint(32) || pubkey_len(2 BE) || pubkey
    if (profile_data->size() < 34) return false;
    uint16_t pk_len = (static_cast<uint16_t>((*profile_data)[32]) << 8) | (*profile_data)[33];
    if (profile_data->size() < 34u + pk_len) return false;
    std::span<const uint8_t> signer_pubkey(profile_data->data() + 34, pk_len);

    // Verify signer pubkey hashes to signer_fp
    auto computed_signer_fp = crypto::sha3_256(signer_pubkey);
    if (computed_signer_fp != signer_fp) {
        spdlog::warn("Group meta validation: stored profile pubkey doesn't match signer_fp");
        return false;
    }

    // Signature covers everything before sig_len: value[0..members_end)
    std::span<const uint8_t> signed_data_span(value.data(), members_end);
    std::span<const uint8_t> signature(value.data() + members_end + 2, sig_len);

    if (!crypto::verify(signed_data_span, signature, signer_pubkey)) {
        spdlog::warn("Group meta validation: signature verification failed");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// SYNC_REQ / SYNC_RESP (section 2 of PROTOCOL-SPEC.md)
// ---------------------------------------------------------------------------

void Kademlia::handle_sync_req(const Message& msg, const std::string& from, uint16_t port) {
    const auto& data = msg.payload;

    // SYNC_REQ payload: [32 bytes: key][8 bytes BE: after_seq]
    if (data.size() < 40) {
        spdlog::warn("SYNC_REQ payload too short from {}:{}", from, port);
        return;
    }

    // Parse key (32 bytes)
    crypto::Hash key{};
    std::copy_n(data.data(), 32, key.begin());

    // Parse after_seq (8 bytes BE)
    uint64_t after_seq = 0;
    for (int i = 0; i < 8; ++i) {
        after_seq = (after_seq << 8) | data[32 + i];
    }

    spdlog::debug("Received SYNC_REQ from {}:{} after_seq={}", from, port, after_seq);

    // Get entries from replication log
    auto entries = repl_log_.entries_after(key, after_seq);

    // Build SYNC_RESP payload per spec:
    // [32 bytes: key]
    // [2 bytes BE: entry_count]
    // Per entry:
    //   [8 bytes BE: seq]
    //   [1 byte: op]
    //   [8 bytes BE: timestamp]
    //   [4 bytes BE: data_length]
    //   [data_length bytes: data]
    std::vector<uint8_t> payload;

    // key (32 bytes)
    payload.insert(payload.end(), key.begin(), key.end());

    // entry_count (2 bytes BE)
    uint16_t entry_count = static_cast<uint16_t>(entries.size());
    payload.push_back(static_cast<uint8_t>((entry_count >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(entry_count & 0xFF));

    // Serialize each entry
    for (const auto& entry : entries) {
        auto serialized = replication::serialize_entry(entry);
        payload.insert(payload.end(), serialized.begin(), serialized.end());
    }

    Message reply = make_message(MessageType::SYNC_RESP, payload);
    transport_.send(from, port, reply);

    spdlog::debug("Sent SYNC_RESP with {} entries to {}:{}", entry_count, from, port);
}

void Kademlia::handle_sync_resp(const Message& msg, const std::string& from, uint16_t port) {
    const auto& data = msg.payload;

    // SYNC_RESP minimum: [32 bytes: key][2 bytes BE: entry_count]
    if (data.size() < 34) {
        spdlog::warn("SYNC_RESP payload too short from {}:{}", from, port);
        return;
    }

    size_t offset = 0;

    // Parse key (32 bytes)
    crypto::Hash key{};
    std::copy_n(data.data(), 32, key.begin());
    offset += 32;

    // Parse entry_count (2 bytes BE)
    uint16_t entry_count = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    offset += 2;

    spdlog::debug("Received SYNC_RESP from {}:{} with {} entries", from, port, entry_count);

    // Parse and collect entries
    std::vector<replication::LogEntry> entries;
    entries.reserve(entry_count);

    for (uint16_t i = 0; i < entry_count; ++i) {
        // Minimum per entry: 8 (seq) + 1 (op) + 8 (timestamp) + 1 (data_type) + 4 (data_length) = 22 bytes
        if (offset + 22 > data.size()) {
            spdlog::warn("SYNC_RESP truncated at entry {} from {}:{}", i, from, port);
            return;
        }

        // Deserialize entry from the remaining payload
        std::span<const uint8_t> entry_data(data.data() + offset, data.size() - offset);
        try {
            auto entry = replication::deserialize_entry(entry_data);
            offset += 8 + 1 + 8 + 1 + 4 + entry.data.size(); // advance past this entry
            entries.push_back(std::move(entry));
        } catch (const std::exception& e) {
            spdlog::warn("SYNC_RESP: failed to deserialize entry {} from {}:{}: {}", i, from, port, e.what());
            return;
        }
    }

    // Check if sender is responsible for this key (required for DEL authorization).
    // A compromised peer could delete arbitrary data by sending SYNC_RESP with
    // DEL entries — only responsible peers are trusted to issue deletions.
    bool sender_is_responsible = false;
    {
        auto responsible = responsible_nodes(key);
        for (const auto& rn : responsible) {
            if (rn.id == msg.sender) {
                sender_is_responsible = true;
                break;
            }
        }
    }

    // Filter out DEL entries from non-responsible peers before they reach
    // repl_log or storage, preventing unauthorized deletions.
    if (!sender_is_responsible) {
        std::erase_if(entries, [](const auto& e) { return e.op == replication::Op::DEL; });
        if (entries.empty()) {
            spdlog::warn("SYNC_RESP: all entries were unauthorized DELs from {}:{}", from, port);
            return;
        }
        spdlog::debug("SYNC_RESP: filtered DEL entries from non-responsible peer {}:{}", from, port);
    }

    // Validate ADD entries before applying to repl_log to prevent invalid
    // entries from persisting and propagating via SYNC.
    std::vector<replication::LogEntry> valid_entries;
    valid_entries.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.op == replication::Op::ADD && !entry.data.empty()) {
            if (store_locally(key, entry.data_type, entry.data,
                              /*validate=*/true, /*log_and_notify=*/false)) {
                valid_entries.push_back(entry);
            } else {
                spdlog::debug("SYNC_RESP: rejected invalid ADD entry (data_type=0x{:02X}) from {}:{}",
                              entry.data_type, from, port);
            }
        } else {
            valid_entries.push_back(entry);  // DEL entries already authorized above
        }
    }

    // Apply only validated entries to replication log (idempotent)
    repl_log_.apply(key, valid_entries);

    // Route DEL entries to the correct storage table.
    // ADD entries were already stored by store_locally() above.
    for (const auto& entry : valid_entries) {
        if (entry.op == replication::Op::DEL) {
            if (entry.data_type == 0x02 && entry.data.size() >= 64) {
                // Inbox delete: delete_info = recipient_fp(32) || msg_id(32)
                std::span<const uint8_t> recipient_fp(entry.data.data(), 32);
                std::span<const uint8_t> msg_id(entry.data.data() + 32, 32);

                std::vector<uint8_t> idx_key;
                idx_key.reserve(64);
                idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
                idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
                storage_.del(storage::TABLE_INBOX_INDEX, idx_key);

                std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
                storage_.del(storage::TABLE_MESSAGE_BLOBS, blob_key);

                spdlog::debug("SYNC: applied DEL for inbox message");
            } else if (entry.data_type == 0x03 && entry.data.size() >= 64) {
                // Contact request delete: delete_info = recipient_fp(32) || sender_fp(32)
                std::vector<uint8_t> cr_key(entry.data.begin(), entry.data.begin() + 64);
                storage_.del(storage::TABLE_REQUESTS, cr_key);
                spdlog::debug("SYNC: applied DEL for contact request");
            } else if (entry.data_type == 0x05 && entry.data.size() >= 64) {
                // Group message delete: delete_info = group_id(32) || msg_id(32)
                std::vector<uint8_t> gm_key(entry.data.begin(), entry.data.begin() + 64);
                storage_.del(storage::TABLE_GROUP_INDEX, gm_key);
                storage_.del(storage::TABLE_GROUP_BLOBS, gm_key);
                spdlog::debug("SYNC: applied DEL for group message");
            } else if (entry.data_type == 0x04 && entry.data.size() >= 64) {
                // Allowlist REVOKE via legacy DEL: store entry instead of deleting
                store_locally(key, entry.data_type, entry.data,
                              /*validate=*/true, /*log_and_notify=*/false);
                spdlog::debug("SYNC: applied DEL->store for allowlist revoke entry");
            } else if (entry.data_type == 0x06) {
                // Group meta delete: stored by routing key (= SYNC key)
                storage_.del(storage::TABLE_GROUP_META, key);
                spdlog::debug("SYNC: applied DEL for group meta");
            } else {
                // For other types, delete by key from the appropriate table
                const char* table_name = nullptr;
                switch (entry.data_type) {
                case 0x00: table_name = storage::TABLE_PROFILES;   break;
                case 0x01: table_name = storage::TABLE_NAMES;      break;
                default: continue;
                }
                storage_.del(table_name, key);
                spdlog::debug("SYNC: applied DEL for data_type 0x{:02X}", entry.data_type);
            }
            continue;
        }

        // ADD entries were already validated and stored above (before repl_log apply)
    }

    spdlog::info("Applied {} SYNC entries for key from {}:{}", entries.size(), from, port);
}

// ---------------------------------------------------------------------------
// STORE_ACK (PROTOCOL-SPEC.md section 2)
// ---------------------------------------------------------------------------

void Kademlia::handle_store_ack(const Message& msg, const std::string& from, uint16_t port) {
    const auto& data = msg.payload;

    // STORE_ACK payload: [32 bytes: key][1 byte: status]
    if (data.size() < 33) {
        spdlog::warn("STORE_ACK payload too short from {}:{}", from, port);
        return;
    }

    crypto::Hash key{};
    std::copy_n(data.data(), 32, key.begin());
    uint8_t status = data[32];

    if (status != 0x00) {
        uint8_t reason = (msg.payload.size() > 33) ? msg.payload[33] : 0x00;
        spdlog::warn("STORE_ACK rejected (status=0x{:02X}, reason=0x{:02X}) from {}:{}",
                     status, reason, from, port);
        return;
    }

    std::lock_guard lock(pending_mutex_);
    auto it = pending_stores_.find(key);
    if (it == pending_stores_.end()) {
        spdlog::debug("STORE_ACK for unknown key from {}:{} (already completed or expired)", from, port);
        return;
    }

    it->second.acked++;
    size_t total_confirmed = it->second.acked + (it->second.local_stored ? 1 : 0);
    size_t w = write_quorum();

    spdlog::debug("STORE_ACK from {}:{} — {}/{} confirmed (W={})",
                  from, port, total_confirmed, it->second.expected + (it->second.local_stored ? 1 : 0), w);

    if (total_confirmed == w) {
        spdlog::info("Write quorum reached for key ({}/{} confirmed)", total_confirmed, w);
    }

    // Clean up if all expected ACKs received
    if (it->second.acked >= it->second.expected) {
        pending_stores_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// SEQ_REQ / SEQ_RESP — lightweight repl_log sequence query
// ---------------------------------------------------------------------------

void Kademlia::handle_seq_req(const Message& msg, const std::string& from, uint16_t port) {
    if (msg.payload.size() < 32) {
        spdlog::warn("SEQ_REQ payload too short from {}:{}", from, port);
        return;
    }

    crypto::Hash key{};
    std::copy_n(msg.payload.data(), 32, key.begin());

    uint64_t seq = repl_log_.current_seq(key);

    // SEQ_RESP payload: key(32) + seq(8 BE)
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), key.begin(), key.end());
    for (int i = 7; i >= 0; --i)
        payload.push_back(static_cast<uint8_t>((seq >> (i * 8)) & 0xFF));

    Message reply = make_message(MessageType::SEQ_RESP, payload);
    transport_.send(from, port, reply);
}

void Kademlia::handle_seq_resp(const Message& msg, const std::string& from, uint16_t port) {
    if (msg.payload.size() < 40) {
        spdlog::warn("SEQ_RESP payload too short from {}:{}", from, port);
        return;
    }

    crypto::Hash key{};
    std::copy_n(msg.payload.data(), 32, key.begin());

    uint64_t seq = 0;
    for (int i = 0; i < 8; ++i)
        seq = (seq << 8) | msg.payload[32 + i];

    {
        std::lock_guard lock(seq_query_mutex_);
        auto it = pending_seq_queries_.find(key);
        if (it != pending_seq_queries_.end()) {
            it->second.responses[msg.sender] = seq;
        }
    }
    seq_query_cv_.notify_all();
}

std::vector<Kademlia::NodeSeq> Kademlia::query_remote_seqs(
    const crypto::Hash& key,
    const std::vector<NodeInfo>& nodes,
    std::chrono::milliseconds timeout) {

    // Register pending query
    {
        std::lock_guard lock(seq_query_mutex_);
        PendingSeqQuery pq;
        pq.key = key;
        pq.expected = nodes.size();
        pending_seq_queries_[key] = std::move(pq);
    }

    // Send SEQ_REQ to all nodes
    std::vector<uint8_t> payload(key.begin(), key.end());
    for (const auto& node : nodes) {
        Message msg = make_message(MessageType::SEQ_REQ, payload);
        send_to_node(node, msg);
    }

    // Wait for responses with condition variable (no busy-wait)
    {
        std::unique_lock lock(seq_query_mutex_);
        seq_query_cv_.wait_for(lock, timeout, [&] {
            auto it = pending_seq_queries_.find(key);
            return it != pending_seq_queries_.end() &&
                   it->second.responses.size() >= it->second.expected;
        });
    }

    // Collect results
    std::vector<NodeSeq> result;
    {
        std::lock_guard lock(seq_query_mutex_);
        auto it = pending_seq_queries_.find(key);
        if (it != pending_seq_queries_.end()) {
            for (const auto& node : nodes) {
                auto resp_it = it->second.responses.find(node.id);
                uint64_t seq = (resp_it != it->second.responses.end()) ? resp_it->second : 0;
                result.push_back({node, seq});
            }
            pending_seq_queries_.erase(it);
        }
    }

    // Sort by seq descending (highest first)
    std::sort(result.begin(), result.end(),
              [](const NodeSeq& a, const NodeSeq& b) { return a.seq > b.seq; });

    return result;
}

// ---------------------------------------------------------------------------
// Write quorum + pending store status
// ---------------------------------------------------------------------------

size_t Kademlia::write_quorum() const {
    return std::min(static_cast<size_t>(2), replication_factor());
}

std::vector<Kademlia::NodeValue> Kademlia::query_remote_values(
    const crypto::Hash& key,
    const std::vector<NodeInfo>& nodes,
    std::chrono::milliseconds timeout) {

    // Count remote nodes (exclude self — we'll do a local lookup)
    size_t remote_count = 0;
    for (const auto& node : nodes) {
        if (node.id != self_.id) ++remote_count;
    }

    // Register pending query for remote nodes
    if (remote_count > 0) {
        std::lock_guard lock(value_query_mutex_);
        PendingValueQuery pq;
        pq.key = key;
        pq.expected = remote_count;
        pending_value_queries_[key] = std::move(pq);
    }

    // Send FIND_VALUE to remote nodes
    std::vector<uint8_t> payload(key.begin(), key.end());
    for (const auto& node : nodes) {
        if (node.id == self_.id) continue;
        Message msg = make_message(MessageType::FIND_VALUE, payload);
        send_to_node(node, msg);
    }

    // Wait for responses
    if (remote_count > 0) {
        std::unique_lock lock(value_query_mutex_);
        value_query_cv_.wait_for(lock, timeout, [&] {
            auto it = pending_value_queries_.find(key);
            return it != pending_value_queries_.end() &&
                   it->second.responses.size() >= it->second.expected;
        });
    }

    // Collect results
    std::vector<NodeValue> result;

    // Add local result (check all single-key tables, not just names)
    for (const auto& node : nodes) {
        if (node.id == self_.id) {
            std::optional<std::vector<uint8_t>> local;
            for (const char* table : {storage::TABLE_PROFILES, storage::TABLE_NAMES,
                                       storage::TABLE_GROUP_META}) {
                local = storage_.get(table, key);
                if (local) break;
            }
            result.push_back({node, local});
            break;
        }
    }

    // Add remote results
    if (remote_count > 0) {
        std::lock_guard lock(value_query_mutex_);
        auto it = pending_value_queries_.find(key);
        if (it != pending_value_queries_.end()) {
            for (const auto& node : nodes) {
                if (node.id == self_.id) continue;
                auto resp_it = it->second.responses.find(node.id);
                if (resp_it != it->second.responses.end()) {
                    result.push_back({node, resp_it->second});
                } else {
                    result.push_back({node, std::nullopt});
                }
            }
            pending_value_queries_.erase(it);
        }
    }

    return result;
}

std::optional<PendingStore> Kademlia::pending_store_status(const crypto::Hash& key) const {
    std::lock_guard lock(pending_mutex_);
    auto it = pending_stores_.find(key);
    if (it == pending_stores_.end()) return std::nullopt;
    return it->second;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<uint8_t> Kademlia::make_find_node_payload() const {
    // FIND_NODE payload:
    //   pubkey_len(2 BE) || pubkey(N)
    //   || addr_family(1) || addr(4 or 16) || ws_port(2 BE)
    //
    // Including our pubkey lets receivers immediately verify our identity
    // via SHA3-256(pubkey) == sender_id, enabling signed message exchange.
    // Including our self-reported address ensures receivers store us at
    // our external address rather than the TCP source IP, which may be a
    // LAN address when nodes share a network.
    std::vector<uint8_t> payload;
    uint16_t pk_len = static_cast<uint16_t>(self_.pubkey.size());
    payload.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    payload.insert(payload.end(), self_.pubkey.begin(), self_.pubkey.end());

    // Append self-reported address
    bool is_ipv6 = self_.address.find(':') != std::string::npos;
    if (is_ipv6) {
        payload.push_back(0x06);
        struct in6_addr addr6{};
        inet_pton(AF_INET6, self_.address.c_str(), &addr6);
        auto* bytes = reinterpret_cast<const uint8_t*>(&addr6);
        payload.insert(payload.end(), bytes, bytes + 16);
    } else {
        payload.push_back(0x04);
        struct in_addr addr4{};
        inet_pton(AF_INET, self_.address.c_str(), &addr4);
        auto* bytes = reinterpret_cast<const uint8_t*>(&addr4.s_addr);
        payload.insert(payload.end(), bytes, bytes + 4);
    }

    // ws_port (2 bytes BE)
    payload.push_back(static_cast<uint8_t>((self_.ws_port >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(self_.ws_port & 0xFF));

    return payload;
}

Message Kademlia::make_message(MessageType type, const std::vector<uint8_t>& payload) {
    Message msg;
    msg.type = type;
    msg.sender = self_.id;
    msg.sender_port = self_.tcp_port;
    msg.payload = payload;
    // Skip ML-DSA-87 signing for discovery messages that don't modify state
    // and are needed before a node's pubkey is known. All other types
    // (PONG, NODES, STORE, etc.) are signed and verified by receivers.
    if (type != MessageType::PING && type != MessageType::FIND_NODE) {
        sign_message(msg, keypair_.secret_key);
    }
    return msg;
}

void Kademlia::send_to_node(const NodeInfo& node, const Message& msg) {
    transport_.send(node.address, node.tcp_port, msg);
}

} // namespace chromatin::kademlia
