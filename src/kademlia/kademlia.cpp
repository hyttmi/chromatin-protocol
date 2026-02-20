#include "kademlia/kademlia.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <regex>
#include <thread>

#include <arpa/inet.h>

#include <spdlog/spdlog.h>

namespace chromatin::kademlia {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Kademlia::Kademlia(NodeInfo self, TcpTransport& transport, RoutingTable& table,
                   storage::Storage& storage, replication::ReplLog& repl_log,
                   const crypto::KeyPair& keypair)
    : self_(std::move(self))
    , transport_(transport)
    , table_(table)
    , storage_(storage)
    , repl_log_(repl_log)
    , keypair_(keypair) {}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

void Kademlia::bootstrap(const std::vector<std::pair<std::string, uint16_t>>& addrs) {
    // Send FIND_NODE (empty payload) to each bootstrap address
    Message msg = make_message(MessageType::FIND_NODE, {});
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

// ---------------------------------------------------------------------------
// Periodic maintenance (self-healing)
// ---------------------------------------------------------------------------

static constexpr auto REFRESH_INTERVAL        = std::chrono::seconds(30);
static constexpr auto REFRESH_INTERVAL_SPARSE  = std::chrono::seconds(5);
static constexpr auto PING_SWEEP_INTERVAL      = std::chrono::seconds(10);
static constexpr auto STALE_THRESHOLD          = std::chrono::seconds(60);
static constexpr auto EVICT_THRESHOLD          = std::chrono::seconds(120);
static constexpr size_t SPARSE_TABLE_SIZE      = 3;
static constexpr auto TTL_DURATION             = std::chrono::hours(7 * 24);  // 7 days
static constexpr auto TTL_SWEEP_INTERVAL       = std::chrono::minutes(5);
static constexpr auto PENDING_STORE_TIMEOUT    = std::chrono::seconds(30);
static constexpr auto TRANSFER_CHECK_INTERVAL  = std::chrono::seconds(60);

void Kademlia::tick() {
    auto now = std::chrono::steady_clock::now();

    // 1. Re-bootstrap / refresh
    auto refresh_interval = (table_.size() < SPARSE_TABLE_SIZE)
                                ? REFRESH_INTERVAL_SPARSE
                                : REFRESH_INTERVAL;

    if (now - last_refresh_ >= refresh_interval) {
        last_refresh_ = now;

        // Send FIND_NODE to bootstrap peers
        if (!bootstrap_addrs_.empty()) {
            bootstrap(bootstrap_addrs_);
        }

        // Also query all known nodes (iterative discovery)
        auto known = table_.all_nodes();
        for (const auto& node : known) {
            Message msg = make_message(MessageType::FIND_NODE, {});
            send_to_node(node, msg);
        }
    }

    // 2. Ping stale nodes + evict dead nodes
    if (now - last_ping_sweep_ >= PING_SWEEP_INTERVAL) {
        last_ping_sweep_ = now;

        auto stale_cutoff = now - STALE_THRESHOLD;
        auto evict_cutoff = now - EVICT_THRESHOLD;

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
    if (now - last_ttl_sweep_ >= TTL_SWEEP_INTERVAL) {
        last_ttl_sweep_ = now;
        expire_ttl();
    }

    // 4. Pending stores cleanup
    cleanup_pending_stores();

    // 5. Responsibility transfer when routing table changes
    size_t current_size = table_.size();
    if (current_size != last_table_size_ &&
        now - last_transfer_check_ >= TRANSFER_CHECK_INTERVAL) {
        last_transfer_check_ = now;
        last_table_size_ = current_size;
        transfer_responsibility();
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
        std::chrono::duration_cast<std::chrono::milliseconds>(TTL_DURATION).count());
    uint64_t cutoff = (now_ms > ttl_ms) ? (now_ms - ttl_ms) : 0;

    // Convert cutoff to seconds for comparison with stored timestamps
    uint64_t cutoff_sec = cutoff / 1000;

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

            if (ts < cutoff_sec) {
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
    // Contact request value: sender_fp(32) || pow_nonce(8) || blob_len(4) || blob
    // No timestamp in the value itself — use repl_log timestamp instead.
    // For simplicity, we skip contact request expiry here since the repl_log
    // timestamp requires correlating with the repl_log entries. This can be
    // added when contact requests include a timestamp field.
}

// ---------------------------------------------------------------------------
// Pending store cleanup — remove entries that never received all ACKs
// ---------------------------------------------------------------------------

void Kademlia::cleanup_pending_stores() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(pending_mutex_);
    for (auto it = pending_stores_.begin(); it != pending_stores_.end();) {
        if (now - it->second.created > PENDING_STORE_TIMEOUT) {
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
    // For each key-value table, scan all keys and check if any new node
    // in the routing table is now among the R closest but doesn't have the data.
    // If so, send a STORE to that node.

    struct TableInfo {
        const char* table;
        uint8_t data_type;
    };

    static constexpr TableInfo tables[] = {
        {storage::TABLE_PROFILES,   0x00},
        {storage::TABLE_NAMES,      0x01},
        {storage::TABLE_REQUESTS,   0x03},
        {storage::TABLE_ALLOWLISTS, 0x04},
    };

    size_t pushed = 0;

    for (const auto& ti : tables) {
        storage_.foreach(ti.table,
            [&](std::span<const uint8_t> key, std::span<const uint8_t> value) -> bool {
                if (key.size() < 32) return true;

                // Use first 32 bytes as routing key
                crypto::Hash routing_key{};
                std::copy_n(key.data(), 32, routing_key.begin());

                auto nodes = responsible_nodes(routing_key);
                bool self_responsible = false;
                for (const auto& n : nodes) {
                    if (n.id == self_.id) {
                        self_responsible = true;
                        break;
                    }
                }

                // Push to all responsible remote nodes (they'll dedup/validate)
                for (const auto& node : nodes) {
                    if (node.id == self_.id) continue;

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
    }

    if (pushed > 0) {
        spdlog::info("Responsibility transfer: pushed {} entries to responsible nodes", pushed);
    }
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void Kademlia::handle_message(const Message& msg, const std::string& from_addr, uint16_t from_port) {
    // Verify ML-DSA-87 signature for messages that modify state or carry data.
    // Exempt: PING/PONG (identity establishment), FIND_NODE (bootstrap discovery),
    // NODES (routing info — sender may not be in our routing table yet).
    // All other message types (STORE, FIND_VALUE, SYNC_REQ, SYNC_RESP, STORE_ACK,
    // SEQ_REQ, SEQ_RESP, VALUE) require the sender to have a verified pubkey.
    if (msg.type != MessageType::PING && msg.type != MessageType::PONG &&
        msg.type != MessageType::FIND_NODE && msg.type != MessageType::NODES) {
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

    // Update routing table — the sender is alive
    NodeInfo sender_info;
    sender_info.id = msg.sender;
    sender_info.address = from;
    sender_info.tcp_port = port;
    sender_info.ws_port = 0;
    sender_info.last_seen = std::chrono::steady_clock::now();
    table_.add_or_update(sender_info);

    Message reply = make_message(MessageType::PONG, {});
    transport_.send(from, port, reply);
}

void Kademlia::handle_pong(const Message& msg, const std::string& from, uint16_t port) {
    spdlog::debug("Received PONG from {}:{}", from, port);

    // Update routing table — the sender is alive
    NodeInfo sender_info;
    sender_info.id = msg.sender;
    sender_info.address = from;
    sender_info.tcp_port = port;
    sender_info.ws_port = 0;
    sender_info.last_seen = std::chrono::steady_clock::now();
    table_.add_or_update(sender_info);
}

// ---------------------------------------------------------------------------
// FIND_NODE / NODES  (section 2 of PROTOCOL-SPEC.md)
// ---------------------------------------------------------------------------

void Kademlia::handle_find_node(const Message& msg, const std::string& from, uint16_t port) {
    spdlog::debug("Received FIND_NODE from {}:{}", from, port);

    // Add the requesting node to our routing table if we can identify it.
    // We know its node_id from the message header. We don't know its pubkey
    // or ws_port yet, but we store what we have so it becomes reachable.
    {
        NodeInfo sender_info;
        sender_info.id = msg.sender;
        sender_info.address = from;
        sender_info.tcp_port = port;
        sender_info.ws_port = 0;
        sender_info.last_seen = std::chrono::steady_clock::now();
        table_.add_or_update(sender_info);
    }

    // Build NODES payload: all known nodes (from table) + self
    std::vector<NodeInfo> all;
    all.push_back(self_);
    auto table_nodes = table_.all_nodes();
    for (auto& n : table_nodes) {
        // Don't include the requester back to itself
        if (n.id == msg.sender) continue;
        // Don't duplicate self
        if (n.id == self_.id) continue;
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

        spdlog::info("Discovered node {} at {}:{}", i, info.address, info.tcp_port);
        table_.add_or_update(info);
    }
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
        return;
    }

    // 2. Data-type-specific validation
    bool valid = true;
    switch (data_type) {
    case 0x00: valid = validate_profile(value, key);     break;
    case 0x01: valid = validate_name_record(value, key); break;
    case 0x02: valid = validate_inbox_message(value);    break;
    case 0x03: valid = validate_contact_request(value);  break;
    case 0x04: valid = validate_allowlist_entry(value);  break;
    default: break; // unknown types are rejected in the switch below
    }
    if (!valid) {
        spdlog::warn("STORE rejected: validation failed for data_type 0x{:02X}", data_type);
        return;
    }

    // 3. Store in appropriate table
    if (data_type == 0x02) {
        // Inbox: two-table write
        // Value: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
        std::span<const uint8_t> recipient_fp(value.data(), 32);
        std::span<const uint8_t> msg_id(value.data() + 32, 32);
        std::span<const uint8_t> sender_fp(value.data() + 64, 32);
        // timestamp at offset 96, blob_len at offset 104
        uint32_t blob_len = (static_cast<uint32_t>(value[104]) << 24)
                          | (static_cast<uint32_t>(value[105]) << 16)
                          | (static_cast<uint32_t>(value[106]) << 8)
                          | static_cast<uint32_t>(value[107]);

        // Dedup: reject if msg_id already exists
        crypto::Hash mid{};
        std::copy_n(msg_id.data(), 32, mid.begin());
        if (storage_.get(storage::TABLE_MESSAGE_BLOBS, mid)) {
            spdlog::debug("STORE: duplicate inbox message rejected (msg_id already exists)");
            return;
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

        storage_.put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);
        storage_.put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob_value);
    } else if (data_type == 0x04) {
        // Allowlist: composite key storage
        // Value: allowed_fp(32) || action(1) || sequence(8 BE) || signature
        std::span<const uint8_t> allowed_fp(value.data(), 32);
        uint8_t action = value[32];
        std::span<const uint8_t> entry_data(value.data() + 32, value.size() - 32);

        // Build composite storage key: routing_key(32) || allowed_fp(32)
        std::vector<uint8_t> composite_key;
        composite_key.reserve(64);
        composite_key.insert(composite_key.end(), key.begin(), key.end());
        composite_key.insert(composite_key.end(), allowed_fp.begin(), allowed_fp.end());

        if (action == 0x01) {
            // ALLOW: store entry
            std::vector<uint8_t> entry_vec(entry_data.begin(), entry_data.end());
            storage_.put(storage::TABLE_ALLOWLISTS, composite_key, entry_vec);
        } else {
            // REVOKE: delete entry
            storage_.del(storage::TABLE_ALLOWLISTS, composite_key);
        }
    } else {
        const char* table_name = nullptr;
        switch (data_type) {
        case 0x00: table_name = storage::TABLE_PROFILES;   break;
        case 0x01: table_name = storage::TABLE_NAMES;      break;
        case 0x03: table_name = storage::TABLE_REQUESTS;   break;
        default:
            spdlog::warn("STORE rejected: unknown data_type 0x{:02X}", data_type);
            return;
        }

        std::vector<uint8_t> value_vec(value.begin(), value.end());
        storage_.put(table_name, key, value_vec);
    }

    // Record mutation in the replication log
    repl_log_.append(key, replication::Op::ADD, data_type, value);

    spdlog::info("Stored data_type=0x{:02X} for key from {}:{}", data_type, from, port);

    if (on_store_) {
        on_store_(key, data_type, value);
    }

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

    // Try relevant tables for the key. Inbox data uses SYNC (not FIND_VALUE)
    // because its routing key is SHA3-256("inbox:"||fp) but TABLE_INBOX_INDEX
    // uses composite key fp(32)||msg_id(32) — can't derive fp from the hash.
    const char* tables[] = {
        storage::TABLE_PROFILES, storage::TABLE_NAMES,
        storage::TABLE_REQUESTS, storage::TABLE_ALLOWLISTS,
    };

    std::vector<uint8_t> payload;
    // key (32 bytes)
    payload.insert(payload.end(), key.begin(), key.end());

    for (const char* table : tables) {
        auto result = storage_.get(table, key);
        if (result) {
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
    // VALUE responses are handled by find_value() via a callback mechanism.
    // For now, just log.
    spdlog::debug("Received VALUE from {}:{} ({} bytes payload)", from, port, msg.payload.size());
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
            // Store locally — perform same validation as handle_store
            bool valid = true;
            switch (data_type) {
            case 0x00: valid = validate_profile(value, key);     break;
            case 0x01: valid = validate_name_record(value, key); break;
            case 0x02: valid = validate_inbox_message(value);    break;
            case 0x03: valid = validate_contact_request(value);  break;
            case 0x04: valid = validate_allowlist_entry(value);  break;
            default: break;
            }
            if (!valid) {
                spdlog::warn("Local store rejected: validation failed for data_type 0x{:02X}", data_type);
                continue;
            }

            if (data_type == 0x02) {
                // Inbox: two-table write
                // Value: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4) || blob
                std::span<const uint8_t> recip(value.data(), 32);
                std::span<const uint8_t> mid(value.data() + 32, 32);
                std::span<const uint8_t> sfp(value.data() + 64, 32);
                uint32_t blen = (static_cast<uint32_t>(value[104]) << 24)
                              | (static_cast<uint32_t>(value[105]) << 16)
                              | (static_cast<uint32_t>(value[106]) << 8)
                              | static_cast<uint32_t>(value[107]);

                // Dedup: reject if msg_id already exists
                crypto::Hash mid_hash{};
                std::copy_n(mid.data(), 32, mid_hash.begin());
                if (storage_.get(storage::TABLE_MESSAGE_BLOBS, mid_hash)) {
                    spdlog::debug("store(): duplicate inbox message rejected");
                    continue;
                }

                std::vector<uint8_t> idx_key;
                idx_key.reserve(64);
                idx_key.insert(idx_key.end(), recip.begin(), recip.end());
                idx_key.insert(idx_key.end(), mid.begin(), mid.end());

                std::vector<uint8_t> idx_value;
                idx_value.reserve(44);
                idx_value.insert(idx_value.end(), sfp.begin(), sfp.end());
                idx_value.insert(idx_value.end(), value.data() + 96, value.data() + 104);
                idx_value.insert(idx_value.end(), value.data() + 104, value.data() + 108);

                std::vector<uint8_t> blob_key(mid.begin(), mid.end());
                std::vector<uint8_t> blob_val(value.data() + 108, value.data() + 108 + blen);

                storage_.put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);
                storage_.put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob_val);
            } else if (data_type == 0x04) {
                // Allowlist: composite key storage
                // Value: allowed_fp(32) || action(1) || sequence(8 BE) || signature
                std::span<const uint8_t> afp(value.data(), 32);
                uint8_t action = value[32];
                std::span<const uint8_t> edata(value.data() + 32, value.size() - 32);

                std::vector<uint8_t> comp_key;
                comp_key.reserve(64);
                comp_key.insert(comp_key.end(), key.begin(), key.end());
                comp_key.insert(comp_key.end(), afp.begin(), afp.end());

                if (action == 0x01) {
                    std::vector<uint8_t> eval(edata.begin(), edata.end());
                    storage_.put(storage::TABLE_ALLOWLISTS, comp_key, eval);
                } else {
                    storage_.del(storage::TABLE_ALLOWLISTS, comp_key);
                }
            } else {
                const char* table_name = nullptr;
                switch (data_type) {
                case 0x00: table_name = storage::TABLE_PROFILES;   break;
                case 0x01: table_name = storage::TABLE_NAMES;      break;
                case 0x03: table_name = storage::TABLE_REQUESTS;   break;
                default:
                    spdlog::warn("store(): unknown data_type 0x{:02X}", data_type);
                    continue;
                }

                std::vector<uint8_t> val_vec(value.begin(), value.end());
                storage_.put(table_name, key, val_vec);
            }

            // Record mutation in the replication log
            repl_log_.append(key, replication::Op::ADD, data_type, value);

            if (on_store_) {
                on_store_(key, data_type, value);
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

// ---------------------------------------------------------------------------
// High-level: find_value()
// ---------------------------------------------------------------------------

std::optional<std::vector<uint8_t>> Kademlia::find_value(const crypto::Hash& key) {
    // First check locally (inbox uses SYNC, not FIND_VALUE)
    const char* tables[] = {
        storage::TABLE_PROFILES, storage::TABLE_NAMES,
        storage::TABLE_REQUESTS, storage::TABLE_ALLOWLISTS,
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
    return std::min(static_cast<size_t>(3), network_size);
}

std::vector<NodeInfo> Kademlia::responsible_nodes(const crypto::Hash& key) const {
    // Collect self + all nodes, sort by XOR distance to key, take top R
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

    std::sort(all.begin(), all.end(), [&target](const NodeInfo& a, const NodeInfo& b) {
        return a.id.distance_to(target) < b.id.distance_to(target);
    });

    size_t r = replication_factor();
    if (all.size() > r) {
        all.resize(r);
    }

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
// Name record validation (PROTOCOL-SPEC.md section 5)
// ---------------------------------------------------------------------------

bool Kademlia::validate_name_record(std::span<const uint8_t> value, const crypto::Hash& key) {
    // Parse name record:
    // [1 byte: name_length]
    // [name_length bytes: name (ASCII, lowercase)]
    // [32 bytes: fingerprint]
    // [8 bytes BE: pow_nonce]
    // [8 bytes BE: sequence]
    // [2 bytes BE: signature_length]
    // [signature_length bytes: ML-DSA-87 signature]

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

    // Need at least: 32 (fingerprint) + 8 (nonce) + 8 (sequence) + 2 (sig_length) = 50
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
    // signed_data = name_length + name + fingerprint + pow_nonce + sequence
    // (everything before the sig_length field)
    size_t pre_sig_len = 1 + name_length + 32 + 8 + 8;
    std::span<const uint8_t> signed_data(value.data(), pre_sig_len);

    // We need the public key to verify. The fingerprint is SHA3-256 of the pubkey.
    // But we don't have the pubkey in the name record — we need to look it up from
    // stored profiles. For now, if we can't verify the signature (no pubkey available),
    // we skip signature verification. In a full implementation, the STORE would need
    // the pubkey or we'd look it up from the profile table.
    //
    // NOTE: For the initial prototype, we verify the signature if we can find
    // the pubkey in the profiles table. If the profile isn't stored yet, we
    // trust the PoW alone. This matches the bootstrap scenario where names
    // can be registered before profiles are widely replicated.
    //
    // Check if we have a profile for this fingerprint
    crypto::Hash profile_key = crypto::sha3_256_prefixed("dna:", fingerprint);
    auto profile_data = storage_.get(storage::TABLE_PROFILES, profile_key);
    if (profile_data) {
        // Extract pubkey from profile: [32 bytes fingerprint][2 bytes BE pubkey_length][pubkey...]
        if (profile_data->size() >= 34) {
            uint16_t pk_len = static_cast<uint16_t>(
                (static_cast<uint16_t>((*profile_data)[32]) << 8) | (*profile_data)[33]);
            if (profile_data->size() >= 34u + pk_len) {
                std::span<const uint8_t> pubkey(profile_data->data() + 34, pk_len);
                if (!crypto::verify(signed_data, signature, pubkey)) {
                    spdlog::warn("Name record validation: signature verification failed for '{}'", name);
                    return false;
                }
            }
        }
    }
    // If no profile found, skip signature verification (PoW is sufficient for initial registration)

    // 4. First-claim-wins: if name already registered to different fingerprint, reject
    // Storage key for the name: SHA3-256("name:" || name)
    auto existing = storage_.get(storage::TABLE_NAMES, key);
    if (existing) {
        // Parse existing record to get its fingerprint and sequence
        if (existing->size() >= 1) {
            uint8_t existing_name_len = (*existing)[0];
            size_t fp_offset = 1 + existing_name_len;
            if (existing->size() >= fp_offset + 32) {
                crypto::Hash existing_fp{};
                std::copy_n(existing->data() + fp_offset, 32, existing_fp.begin());

                if (existing_fp != fingerprint) {
                    spdlog::warn("Name record validation: '{}' already registered to different fingerprint", name);
                    return false;
                }

                // 5. Same fingerprint: sequence must be higher
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

bool Kademlia::validate_profile(std::span<const uint8_t> value, const crypto::Hash& key) {
    static constexpr size_t MAX_PROFILE_SIZE = 1024 * 1024; // 1 MiB

    if (value.size() > MAX_PROFILE_SIZE) {
        spdlog::warn("Profile validation: size {} exceeds 1 MiB limit", value.size());
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

    // Verify storage key == SHA3-256("dna:" || fingerprint)
    auto expected_key = crypto::sha3_256_prefixed("dna:", fingerprint);
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

    // Skip bio
    if (offset + 2 > value.size()) return false;
    uint16_t bio_len = (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1];
    offset += 2;
    if (offset + bio_len > value.size()) return false;
    offset += bio_len;

    // Skip avatar
    if (offset + 4 > value.size()) return false;
    uint32_t avatar_len = (static_cast<uint32_t>(value[offset]) << 24)
                        | (static_cast<uint32_t>(value[offset + 1]) << 16)
                        | (static_cast<uint32_t>(value[offset + 2]) << 8)
                        | static_cast<uint32_t>(value[offset + 3]);
    offset += 4;
    if (offset + avatar_len > value.size()) return false;
    offset += avatar_len;

    // Skip social links
    if (offset + 1 > value.size()) return false;
    uint8_t social_count = value[offset];
    offset += 1;
    for (uint8_t i = 0; i < social_count; ++i) {
        if (offset + 1 > value.size()) return false;
        uint8_t platform_len = value[offset]; offset += 1;
        if (offset + platform_len > value.size()) return false;
        offset += platform_len;
        if (offset + 1 > value.size()) return false;
        uint8_t handle_len = value[offset]; offset += 1;
        if (offset + handle_len > value.size()) return false;
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
    auto existing = storage_.get(storage::TABLE_PROFILES, key);
    if (existing && !existing->empty()) {
        auto existing_seq = extract_profile_sequence(*existing);
        if (existing_seq && sequence <= *existing_seq) {
            spdlog::debug("Profile rejected: sequence {} <= existing {}", sequence, *existing_seq);
            return false;
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
    static constexpr size_t MAX_MESSAGE_SIZE = 50ULL * 1024 * 1024; // 50 MiB

    if (value.size() < 108) {
        spdlog::warn("Inbox validation: too short ({} bytes)", value.size());
        return false;
    }

    if (value.size() > MAX_MESSAGE_SIZE) {
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

    // Trust boundary: allowlist check requires recipient_fp (hashed in key),
    // so Kademlia can't verify it. WS server validates before calling store().
    return true;
}

// ---------------------------------------------------------------------------
// Contact request validation
// ---------------------------------------------------------------------------

bool Kademlia::validate_contact_request(std::span<const uint8_t> value) {
    // Format: sender_fp(32) || pow_nonce(8 BE) || blob_length(4 BE) || blob
    // Minimum: 32 + 8 + 4 = 44 bytes
    static constexpr size_t MAX_REQUEST_BLOB = 64 * 1024; // 64 KiB

    if (value.size() < 44) {
        spdlog::warn("Contact request validation: too short ({} bytes)", value.size());
        return false;
    }

    // Parse blob_length and verify it matches remaining data
    size_t offset = 32 + 8; // skip sender_fp + pow_nonce
    uint32_t blob_len = (static_cast<uint32_t>(value[offset]) << 24)
                      | (static_cast<uint32_t>(value[offset + 1]) << 16)
                      | (static_cast<uint32_t>(value[offset + 2]) << 8)
                      | static_cast<uint32_t>(value[offset + 3]);
    offset += 4;

    if (blob_len > MAX_REQUEST_BLOB) {
        spdlog::warn("Contact request validation: blob_len {} exceeds 64 KiB", blob_len);
        return false;
    }

    if (offset + blob_len != value.size()) {
        spdlog::warn("Contact request validation: blob_len {} doesn't match remaining data", blob_len);
        return false;
    }

    // Trust boundary: PoW verification requires recipient_fp (hashed in key),
    // so Kademlia can't verify it. WS server validates before calling store().
    return true;
}

// ---------------------------------------------------------------------------
// Allowlist entry validation
// ---------------------------------------------------------------------------

bool Kademlia::validate_allowlist_entry(std::span<const uint8_t> value) {
    // Format: allowed_fp(32) || action(1) || sequence(8 BE) || signature
    // Minimum: 32 + 1 + 8 = 41 bytes
    if (value.size() < 41) {
        spdlog::warn("Allowlist validation: too short ({} bytes)", value.size());
        return false;
    }

    uint8_t action = value[32];  // action is after allowed_fp
    if (action != 0x00 && action != 0x01) {
        spdlog::warn("Allowlist validation: invalid action byte 0x{:02X}", action);
        return false;
    }

    // Trust boundary: signature verification requires owner's pubkey (hashed in key),
    // so Kademlia can't verify it. WS server validates before calling store().
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

    // Apply entries to replication log (idempotent)
    repl_log_.apply(key, entries);

    // Route entries to the correct storage table using data_type and op.
    for (const auto& entry : entries) {
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
            } else {
                // For other types, delete by key from the appropriate table
                const char* table_name = nullptr;
                switch (entry.data_type) {
                case 0x00: table_name = storage::TABLE_PROFILES;   break;
                case 0x01: table_name = storage::TABLE_NAMES;      break;
                case 0x03: table_name = storage::TABLE_REQUESTS;   break;
                default: continue;
                }
                storage_.del(table_name, key);
                spdlog::debug("SYNC: applied DEL for data_type 0x{:02X}", entry.data_type);
            }
            continue;
        }

        if (entry.op == replication::Op::ADD && !entry.data.empty()) {
            if (entry.data_type == 0x02) {
                // Inbox: two-table write
                // Data: recipient_fp(32) || msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4) || blob
                if (entry.data.size() < 108) {
                    spdlog::warn("SYNC: inbox entry too short ({} bytes)", entry.data.size());
                    continue;
                }
                std::span<const uint8_t> d(entry.data);
                std::span<const uint8_t> recipient_fp(d.data(), 32);
                std::span<const uint8_t> msg_id(d.data() + 32, 32);
                std::span<const uint8_t> sender_fp(d.data() + 64, 32);
                uint32_t blob_len = (static_cast<uint32_t>(d[104]) << 24)
                                  | (static_cast<uint32_t>(d[105]) << 16)
                                  | (static_cast<uint32_t>(d[106]) << 8)
                                  | static_cast<uint32_t>(d[107]);

                std::vector<uint8_t> idx_key;
                idx_key.reserve(64);
                idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
                idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

                std::vector<uint8_t> idx_value;
                idx_value.reserve(44);
                idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
                idx_value.insert(idx_value.end(), d.data() + 96, d.data() + 104);
                idx_value.insert(idx_value.end(), d.data() + 104, d.data() + 108);

                std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
                std::vector<uint8_t> blob_value(d.data() + 108, d.data() + 108 + blob_len);

                storage_.put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);
                storage_.put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob_value);
            } else if (entry.data_type == 0x04) {
                // Allowlist: composite key storage
                // Data: allowed_fp(32) || action(1) || sequence(8 BE) || signature
                if (entry.data.size() < 41) {
                    spdlog::warn("SYNC: allowlist entry too short ({} bytes)", entry.data.size());
                    continue;
                }
                std::span<const uint8_t> allowed_fp(entry.data.data(), 32);
                uint8_t action = entry.data[32];
                std::vector<uint8_t> composite_key;
                composite_key.reserve(64);
                composite_key.insert(composite_key.end(), key.begin(), key.end());
                composite_key.insert(composite_key.end(), allowed_fp.begin(), allowed_fp.end());
                if (action == 0x01) {
                    std::vector<uint8_t> entry_val(entry.data.begin() + 32, entry.data.end());
                    storage_.put(storage::TABLE_ALLOWLISTS, composite_key, entry_val);
                } else {
                    storage_.del(storage::TABLE_ALLOWLISTS, composite_key);
                }
            } else {
                const char* table_name = nullptr;
                switch (entry.data_type) {
                case 0x00: table_name = storage::TABLE_PROFILES;   break;
                case 0x01: table_name = storage::TABLE_NAMES;      break;
                case 0x03: table_name = storage::TABLE_REQUESTS;   break;
                default:
                    spdlog::warn("SYNC: unknown data_type 0x{:02X}, skipping", entry.data_type);
                    continue;
                }
                storage_.put(table_name, key, entry.data);
            }
        }
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
        spdlog::warn("STORE_ACK rejected (status=0x{:02X}) from {}:{}", status, from, port);
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

    std::lock_guard lock(seq_query_mutex_);
    auto it = pending_seq_queries_.find(key);
    if (it != pending_seq_queries_.end()) {
        it->second.responses[msg.sender] = seq;
    }
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

    // Poll for responses with timeout
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(seq_query_mutex_);
            auto it = pending_seq_queries_.find(key);
            if (it != pending_seq_queries_.end() &&
                it->second.responses.size() >= it->second.expected) {
                break; // All responses received
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

std::optional<PendingStore> Kademlia::pending_store_status(const crypto::Hash& key) const {
    std::lock_guard lock(pending_mutex_);
    auto it = pending_stores_.find(key);
    if (it == pending_stores_.end()) return std::nullopt;
    return it->second;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Message Kademlia::make_message(MessageType type, const std::vector<uint8_t>& payload) {
    Message msg;
    msg.type = type;
    msg.sender = self_.id;
    msg.sender_port = self_.tcp_port;
    msg.payload = payload;
    sign_message(msg, keypair_.secret_key);
    return msg;
}

void Kademlia::send_to_node(const NodeInfo& node, const Message& msg) {
    transport_.send(node.address, node.tcp_port, msg);
}

} // namespace chromatin::kademlia
