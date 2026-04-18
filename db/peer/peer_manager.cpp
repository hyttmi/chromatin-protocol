#include "db/peer/peer_manager.h"
#include "db/logging/logging.h"
#include "db/util/blob_helpers.h"
#include "db/util/endian.h"
#include "db/util/hex.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <csignal>

namespace chromatindb::peer {

namespace {

/// Convert 64-char hex string to 32-byte namespace ID.
std::array<uint8_t, 32> hex_to_namespace(const std::string& hex) {
    std::array<uint8_t, 32> result{};
    for (size_t i = 0; i < 32; ++i) {
        auto byte_str = hex.substr(i * 2, 2);
        result[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
    }
    return result;
}

} // anonymous namespace

PeerManager::PeerManager(const config::Config& config,
                         identity::NodeIdentity& identity,
                         engine::BlobEngine& engine,
                         storage::Storage& storage,
                         asio::io_context& ioc,
                         asio::thread_pool& pool,
                         acl::AccessControl& acl,
                         const std::filesystem::path& config_path)
    : config_(config)
    , identity_(identity)
    , engine_(engine)
    , storage_(storage)
    , ioc_(ioc)
    , pool_(pool)
    , acl_(acl)
    , server_(config, identity, ioc)
    , sighup_signal_(ioc)
    , sigusr1_signal_(ioc)
    , config_path_(config_path)
    , metrics_collector_(storage, ioc, config.metrics_bind, stopping_)
    , conn_mgr_(identity, acl, metrics_collector_.node_metrics(), server_, ioc,
                stopping_, sync_namespaces_, config.rate_limit_burst, config.max_peers, config.max_clients,
                config.strike_threshold,
                // MessageCallback: route to dispatcher
                [this](net::Connection::Ptr c, wire::TransportMsgType type,
                       std::vector<uint8_t> payload, uint32_t rid) {
                    dispatcher_.on_peer_message(c, type, std::move(payload), rid);
                },
                // SyncTrigger: run_sync_with_peer
                [this](net::Connection::Ptr c) -> asio::awaitable<void> {
                    co_await sync_.run_sync_with_peer(c);
                },
                // ConnectCallback: PEX tracking on connect
                [this](net::Connection::Ptr conn, const std::string& addr) {
                    pex_.known_addresses().insert(addr);
                    pex_.update_persisted_peer(addr, true);
                    auto pk_hash = crypto::sha3_256(conn->peer_pubkey());
                    auto pk_hex = chromatindb::util::to_hex(
                        std::span<const uint8_t>(pk_hash.data(), pk_hash.size()), 32);
                    auto it = std::find_if(pex_.persisted_peers().begin(), pex_.persisted_peers().end(),
                                           [&addr](const PersistedPeer& p) { return p.address == addr; });
                    if (it != pex_.persisted_peers().end()) {
                        it->pubkey_hash = pk_hex;
                    }
                },
                // DisconnectCallback: clean blob_push pending_fetches
                [this](net::Connection::Ptr conn) {
                    blob_push_.clean_pending_fetches(conn);
                })
    , sync_(ioc, pool, engine, storage, metrics_collector_.node_metrics(),
            stopping_, sync_namespaces_, conn_mgr_.peers(), conn_mgr_.disconnected_peers(),
            // OnBlobIngested callback
            [this](const std::array<uint8_t, 32>& ns, const std::array<uint8_t, 32>& hash,
                   uint64_t seq, uint32_t size, bool tombstone, uint64_t expiry,
                   net::Connection::Ptr source) {
                on_blob_ingested(ns, hash, seq, size, tombstone, expiry, std::move(source));
            },
            // StrikeCallback
            [this](net::Connection::Ptr conn, const std::string& reason) {
                conn_mgr_.record_strike(conn, reason);
            },
            // PexRequestCallback (initiator side: send PeerListRequest, receive response)
            [this](net::Connection::Ptr conn) -> asio::awaitable<void> {
                if (acl_.is_peer_closed_mode()) co_return;
                std::span<const uint8_t> empty{};
                if (!co_await conn->send_message(wire::TransportMsgType_PeerListRequest, empty)) {
                    co_return;
                }
                auto pex_msg = co_await sync_.recv_sync_msg(conn, std::chrono::seconds(5));
                if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListResponse) {
                    pex_.handle_peer_list_response(conn, std::move(pex_msg->payload));
                }
            },
            // PexRespondCallback (responder side: wait for PeerListRequest, send response)
            [this](net::Connection::Ptr conn) -> asio::awaitable<void> {
                if (acl_.is_peer_closed_mode()) co_return;
                auto pex_msg = co_await sync_.recv_sync_msg(conn, std::chrono::seconds(5));
                if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListRequest) {
                    auto addresses = pex_.build_peer_list_response(conn->remote_address());
                    auto payload = encode_peer_list(addresses);
                    spdlog::debug("PEX: sending {} peers to {}", addresses.size(), conn->remote_address());
                    co_await conn->send_message(wire::TransportMsgType_PeerListResponse,
                                                 std::span<const uint8_t>(payload));
                }
            },
            config.blob_transfer_timeout, config.sync_timeout)
    , pex_(ioc, stopping_, conn_mgr_.peers(), server_, acl,
           config.bind_address, config.data_dir, config.max_peers, config.pex_interval,
           conn_mgr_.bootstrap_addresses(),
           [](const std::vector<std::string>& addrs) { return PeerManager::encode_peer_list(addrs); },
           [](std::span<const uint8_t> payload) { return PeerManager::decode_peer_list(payload); })
    , blob_push_(ioc, engine, storage, metrics_collector_.node_metrics(),
                 stopping_, sync_namespaces_, conn_mgr_.peers(),
                 // rearm_expiry callback
                 [this](uint64_t expiry_time) {
                     sync_.rearm_expiry_timer(expiry_time);
                 })
    , dispatcher_(ioc, pool, engine, storage, metrics_collector_.node_metrics(),
                  stopping_, sync_namespaces_, conn_mgr_.peers(),
                  sync_, pex_, blob_push_, conn_mgr_,
                  // StrikeCallback
                  [this](net::Connection::Ptr conn, const std::string& reason) {
                      conn_mgr_.record_strike(conn, reason);
                  },
                  // OnBlobIngested callback
                  [this](const std::array<uint8_t, 32>& ns, const std::array<uint8_t, 32>& hash,
                         uint64_t seq, uint32_t size, bool tombstone, uint64_t expiry,
                         net::Connection::Ptr source) {
                      on_blob_ingested(ns, hash, seq, size, tombstone, expiry, std::move(source));
                  },
                  // UptimeCallback
                  [this]() -> uint64_t { return metrics_collector_.compute_uptime_seconds(); },
                  // MaxStorageCallback
                  [this]() -> uint64_t { return config_.max_storage_bytes; }) {
    // Wire peers reference to MetricsCollector (deferred: conn_mgr_ owns peers_)
    metrics_collector_.set_peers(conn_mgr_.peers());

    // Initialize sync config parameters
    sync_.set_sync_config(config.sync_cooldown_seconds, config.max_sync_sessions,
                          config.safety_net_interval_seconds);
    sync_.set_rate_limit(config.rate_limit_bytes_per_sec);
    sync_.set_compaction_interval(config.compaction_interval_hours);
    sync_.set_cursor_config(config.full_resync_interval, config.cursor_stale_seconds);

    // Initialize dispatcher rate limits and subscription limit
    dispatcher_.set_rate_limits(config.rate_limit_bytes_per_sec, config.rate_limit_burst);
    dispatcher_.set_max_subscriptions(config.max_subscriptions_per_connection);

    // Initialize namespace filter from config
    for (const auto& hex : config.sync_namespaces) {
        sync_namespaces_.insert(hex_to_namespace(hex));
    }

    // Initialize trusted peers from config (canonicalize via Asio parsing)
    {
        std::set<std::string> trusted;
        for (const auto& ip_str : config.trusted_peers) {
            asio::error_code ec;
            auto addr = asio::ip::make_address(ip_str, ec);
            if (!ec) {
                trusted.insert(addr.to_string());
            }
        }
        conn_mgr_.set_trusted_peers(std::move(trusted));
    }

    // Track bootstrap addresses
    for (const auto& addr : config.bootstrap_peers) {
        conn_mgr_.bootstrap_addresses().insert(addr);
        pex_.known_addresses().insert(addr);
    }

    // Wire trust check for lightweight handshake
    server_.set_trust_check([this](const asio::ip::address& addr) {
        return conn_mgr_.is_trusted_address(addr);
    });

    // Wire thread pool for crypto offload
    server_.set_pool(pool);

    // Wire server callbacks -- delegate to ConnectionManager
    server_.set_accept_filter([this]() { return conn_mgr_.should_accept_connection(); });

    server_.set_on_connected([this](net::Connection::Ptr conn) {
        conn_mgr_.on_peer_connected(conn);
    });

    server_.set_on_disconnected([this](net::Connection::Ptr conn) {
        conn_mgr_.on_peer_disconnected(conn);
    });

    // UDS acceptor setup (only if uds_path is configured)
    if (!config.uds_path.empty()) {
        uds_acceptor_ = std::make_unique<net::UdsAcceptor>(
            config.uds_path, identity, ioc);

        uds_acceptor_->set_accept_filter([this]() { return conn_mgr_.should_accept_connection(); });
        uds_acceptor_->set_on_connected([this](net::Connection::Ptr conn) {
            conn_mgr_.on_peer_connected(conn);
        });
        uds_acceptor_->set_on_disconnected([this](net::Connection::Ptr conn) {
            conn_mgr_.on_peer_disconnected(conn);
        });
        uds_acceptor_->set_pool(pool);
    }

    // Set up sync-received blob notification callback (unified fan-out)
    sync_.sync_proto().set_on_blob_ingested(
        [this](const std::array<uint8_t, 32>& ns, const std::array<uint8_t, 32>& hash,
               uint64_t seq, uint32_t size, bool tombstone, uint64_t expiry_time,
               net::Connection::Ptr source) {
            on_blob_ingested(ns, hash, seq, size, tombstone, expiry_time, std::move(source));
        });
}

void PeerManager::start() {
    metrics_collector_.record_start_time();

    // Log access control mode
    if (acl_.is_client_closed_mode()) {
        spdlog::info("client access control: closed mode ({} allowed keys)", acl_.client_allowed_count());
    } else {
        spdlog::info("client access control: open mode");
    }
    if (acl_.is_peer_closed_mode()) {
        spdlog::info("peer access control: closed mode ({} allowed keys)", acl_.peer_allowed_count());
    } else {
        spdlog::info("peer access control: open mode");
    }

    // Set up SIGHUP handler for config reload
    if (!config_path_.empty()) {
        setup_sighup_handler();
        spdlog::info("SIGHUP reload: enabled (config: {})", config_path_.string());
    } else {
        spdlog::info("SIGHUP reload: disabled (no --config provided)");
    }

    // Set up SIGUSR1 handler for metrics dump
    setup_sigusr1_handler();

    // Load persisted peers before starting server
    pex_.load_persisted_peers();

    // Startup cursor cleanup: remove cursors for peers no longer known
    {
        std::vector<std::array<uint8_t, 32>> known_hashes;
        for (const auto& pp : pex_.persisted_peers()) {
            if (!pp.pubkey_hash.empty() && pp.pubkey_hash.size() == 64) {
                known_hashes.push_back(hex_to_namespace(pp.pubkey_hash));
            }
        }
        if (!known_hashes.empty()) {
            auto removed = storage_.cleanup_stale_cursors(known_hashes);
            if (removed > 0) {
                spdlog::info("startup: removed {} cursor entries for unknown peers", removed);
            }
        }
    }

    server_.start();

    // Start UDS acceptor if configured
    if (uds_acceptor_) {
        uds_acceptor_->start();
    }

    // Register shutdown callback
    server_.set_on_shutdown([this]() {
        stopping_ = true;
        pex_.save_persisted_peers();
        if (uds_acceptor_) uds_acceptor_->stop();
        sighup_signal_.cancel();
        sigusr1_signal_.cancel();
        cancel_all_timers();
    });

    // Connect to persisted peers
    for (const auto& pp : pex_.persisted_peers()) {
        if (!conn_mgr_.bootstrap_addresses().count(pp.address)) {
            pex_.known_addresses().insert(pp.address);
            server_.connect_once(pp.address);
        }
    }

    // Start periodic sync timer
    asio::co_spawn(ioc_, sync_.sync_timer_loop(), asio::detached);

    // Start periodic peer exchange timer
    asio::co_spawn(ioc_, pex_.pex_timer_loop(), asio::detached);

    // Start periodic peer list flush timer
    asio::co_spawn(ioc_, pex_.peer_flush_timer_loop(), asio::detached);

    // Start event-driven expiry timer
    auto earliest = storage_.get_earliest_expiry();
    if (earliest.has_value()) {
        sync_.rearm_expiry_timer(*earliest);
    }

    // Start periodic metrics log timer
    asio::co_spawn(ioc_, metrics_collector_.metrics_timer_loop(), asio::detached);

    // Start cursor compaction timer
    asio::co_spawn(ioc_, sync_.cursor_compaction_loop(), asio::detached);

    // Storage compaction: only spawn when enabled
    if (sync_.compaction_interval_hours() > 0) {
        asio::co_spawn(ioc_, sync_.compaction_loop(), asio::detached);
    }

    // Keepalive: send Ping to all TCP peers every 30s
    asio::co_spawn(ioc_, conn_mgr_.keepalive_loop(), asio::detached);

    // Set up dump extra callback (UDS + compaction stats)
    metrics_collector_.set_dump_extra([this]() -> std::string {
        std::string extra;
        if (uds_acceptor_) {
            extra += "  uds_connections: " + std::to_string(uds_acceptor_->connection_count()) + "\n";
        }
        extra += "  last_compaction_time: " + std::to_string(sync_.last_compaction_time()) + "\n";
        extra += "  compaction_count: " + std::to_string(sync_.compaction_count());
        return extra;
    });

    // Start Prometheus /metrics HTTP listener if configured
    metrics_collector_.start_metrics_listener();
}

void PeerManager::cancel_all_timers() {
    sync_.cancel_timers();
    conn_mgr_.cancel_timers();
    pex_.cancel_timers();
    metrics_collector_.cancel_timers();
}

void PeerManager::stop() {
    stopping_ = true;
    if (uds_acceptor_) uds_acceptor_->stop();
    sighup_signal_.cancel();
    sigusr1_signal_.cancel();
    cancel_all_timers();
    server_.stop();
}

int PeerManager::exit_code() const {
    return server_.exit_code();
}

size_t PeerManager::bootstrap_peer_count() const {
    size_t count = 0;
    for (const auto& p : conn_mgr_.peers()) {
        if (p->is_bootstrap) ++count;
    }
    return count;
}

// =============================================================================
// Facade delegation: on_blob_ingested -> blob_push_
// =============================================================================

void PeerManager::on_blob_ingested(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone,
    uint64_t expiry_time,
    net::Connection::Ptr source) {
    blob_push_.on_blob_ingested(namespace_id, blob_hash, seq_num, blob_size,
                                 is_tombstone, expiry_time, std::move(source));
}

// =============================================================================
// PEX wire encoding (static)
// =============================================================================

std::vector<uint8_t> PeerManager::encode_peer_list(const std::vector<std::string>& addresses) {
    std::vector<uint8_t> result;
    uint16_t count = static_cast<uint16_t>(addresses.size());
    result.push_back(static_cast<uint8_t>(count >> 8));
    result.push_back(static_cast<uint8_t>(count & 0xFF));
    for (const auto& addr : addresses) {
        uint16_t len = static_cast<uint16_t>(addr.size());
        result.push_back(static_cast<uint8_t>(len >> 8));
        result.push_back(static_cast<uint8_t>(len & 0xFF));
        result.insert(result.end(), addr.begin(), addr.end());
    }
    return result;
}

std::vector<std::string> PeerManager::decode_peer_list(std::span<const uint8_t> payload) {
    std::vector<std::string> result;
    if (payload.size() < 2) return result;
    size_t offset = 0;
    uint16_t count = (static_cast<uint16_t>(payload[offset]) << 8) |
                      static_cast<uint16_t>(payload[offset + 1]);
    offset += 2;
    for (uint16_t i = 0; i < count && offset + 2 <= payload.size(); ++i) {
        uint16_t len = (static_cast<uint16_t>(payload[offset]) << 8) |
                        static_cast<uint16_t>(payload[offset + 1]);
        offset += 2;
        if (offset + len > payload.size()) break;
        result.emplace_back(reinterpret_cast<const char*>(payload.data() + offset), len);
        offset += len;
    }
    return result;
}

// =============================================================================
// Pub/Sub wire encoding (static)
// =============================================================================

std::vector<uint8_t> PeerManager::encode_namespace_list(
    const std::vector<std::array<uint8_t, 32>>& namespaces) {
    std::vector<uint8_t> result;
    result.reserve(2 + namespaces.size() * 32);
    chromatindb::util::write_u16_be(result, static_cast<uint16_t>(namespaces.size()));
    for (const auto& ns : namespaces) {
        result.insert(result.end(), ns.begin(), ns.end());
    }
    return result;
}

std::vector<std::array<uint8_t, 32>> PeerManager::decode_namespace_list(
    std::span<const uint8_t> payload) {
    std::vector<std::array<uint8_t, 32>> result;
    if (payload.size() < 2) return result;
    uint16_t count = chromatindb::util::read_u16_be(payload);
    auto product = chromatindb::util::checked_mul(static_cast<size_t>(count), size_t{32});
    if (!product) return result;
    auto expected = chromatindb::util::checked_add(size_t{2}, *product);
    if (!expected || payload.size() != *expected) return result;
    for (uint16_t i = 0; i < count; ++i) {
        result.push_back(chromatindb::util::extract_namespace(
            payload.subspan(2 + static_cast<size_t>(i) * 32)));
    }
    return result;
}

std::vector<uint8_t> PeerManager::encode_notification(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t, 32> blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone) {
    return chromatindb::util::encode_blob_ref(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);
}

// =============================================================================
// Prometheus facade
// =============================================================================

std::string PeerManager::prometheus_metrics_text() {
    return metrics_collector_.prometheus_metrics_text();
}

// =============================================================================
// SIGHUP config reload
// =============================================================================

void PeerManager::setup_sighup_handler() {
    sighup_signal_.add(SIGHUP);
    asio::co_spawn(ioc_, sighup_loop(), asio::detached);
}

asio::awaitable<void> PeerManager::sighup_loop() {
    while (!stopping_) {
        auto [ec, sig] = co_await sighup_signal_.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        handle_sighup();
    }
}

void PeerManager::handle_sighup() {
    spdlog::info("SIGHUP received, reloading config...");
    reload_config();
    server_.clear_reconnect_state();
    spdlog::info("reconnect state cleared (ACL rejection counters reset)");
}

void PeerManager::reload_config() {
    spdlog::info("reloading access control from {}...", config_path_.string());

    config::Config new_cfg;
    try {
        new_cfg = config::load_config(config_path_);
    } catch (const std::exception& e) {
        spdlog::error("config reload failed (invalid JSON): {} (keeping current config)", e.what());
        return;
    }

    // WR-01/WR-02: Validate everything upfront before mutating any state.
    // This prevents partial application on late-discovered validation errors
    // and ensures out-of-range values (e.g. pex_interval=1) are rejected on
    // SIGHUP, not just at daemon startup.  bind_address is not reloaded at
    // runtime (server is already bound), so skip that specific check.
    try {
        config::validate_config(new_cfg, /*check_bind_address=*/false);
        config::validate_allowed_keys(new_cfg.allowed_client_keys);
        config::validate_allowed_keys(new_cfg.allowed_peer_keys);
        config::validate_allowed_keys(new_cfg.sync_namespaces);
        config::validate_trusted_peers(new_cfg.trusted_peers);
    } catch (const std::exception& e) {
        spdlog::error("config reload rejected (validation failed): {} (keeping current config)", e.what());
        return;
    }

    auto result = acl_.reload(new_cfg.allowed_client_keys, new_cfg.allowed_peer_keys);

    spdlog::info("config reload: client keys +{} -{}, peer keys +{} -{}",
                 result.client_added, result.client_removed,
                 result.peer_added, result.peer_removed);

    if (acl_.is_client_closed_mode()) {
        spdlog::info("client access control: closed mode ({} allowed keys)", acl_.client_allowed_count());
    } else {
        spdlog::info("client access control: open mode");
    }
    if (acl_.is_peer_closed_mode()) {
        spdlog::info("peer access control: closed mode ({} allowed keys)", acl_.peer_allowed_count());
    } else {
        spdlog::info("peer access control: open mode");
    }

    if (result.client_removed > 0 || result.peer_removed > 0 ||
        acl_.is_client_closed_mode() || acl_.is_peer_closed_mode()) {
        conn_mgr_.disconnect_unauthorized_peers();
    }

    // Reload rate limits -> dispatcher + sync_
    dispatcher_.set_rate_limits(new_cfg.rate_limit_bytes_per_sec, new_cfg.rate_limit_burst);
    sync_.set_rate_limit(new_cfg.rate_limit_bytes_per_sec);
    if (new_cfg.rate_limit_bytes_per_sec > 0) {
        spdlog::info("config reload: rate_limit={}B/s burst={}B",
                     new_cfg.rate_limit_bytes_per_sec, new_cfg.rate_limit_burst);
    } else {
        spdlog::info("config reload: rate_limit=disabled");
    }

    // Reload subscription limit -> dispatcher
    dispatcher_.set_max_subscriptions(new_cfg.max_subscriptions_per_connection);
    spdlog::info("config reload: max_subscriptions_per_connection={}",
                 new_cfg.max_subscriptions_per_connection == 0 ? std::string("unlimited") :
                 std::to_string(new_cfg.max_subscriptions_per_connection));

    // Reload sync config -> sync_
    sync_.set_sync_config(new_cfg.sync_cooldown_seconds, new_cfg.max_sync_sessions,
                          new_cfg.safety_net_interval_seconds);
    spdlog::info("config reload: sync_cooldown={}s safety_net_interval={}s",
                 new_cfg.sync_cooldown_seconds, new_cfg.safety_net_interval_seconds);

    // Reload max_peers -> conn_mgr_ + pex_
    conn_mgr_.set_max_peers(new_cfg.max_peers);
    conn_mgr_.set_max_clients(new_cfg.max_clients);
    pex_.set_max_peers(new_cfg.max_peers);
    if (conn_mgr_.peer_count() > new_cfg.max_peers) {
        spdlog::warn("config reload: max_peers={} but {} peers connected "
                     "(excess will drain naturally, new connections refused)",
                     new_cfg.max_peers, conn_mgr_.peer_count());
    } else {
        spdlog::info("config reload: max_peers={}", new_cfg.max_peers);
    }

    // Reload sync_namespaces (already validated upfront above)
    sync_namespaces_.clear();
    for (const auto& hex : new_cfg.sync_namespaces) {
        sync_namespaces_.insert(hex_to_namespace(hex));
    }
    if (sync_namespaces_.empty()) {
        spdlog::info("config reload: sync_namespaces=all (unrestricted)");
    } else {
        spdlog::info("config reload: sync_namespaces={} namespaces", sync_namespaces_.size());
    }

    // Phase 86: Re-announce sync_namespaces to all connected peers
    {
        auto ns_list = std::vector<std::array<uint8_t, 32>>(
            sync_namespaces_.begin(), sync_namespaces_.end());
        auto announce_payload = encode_namespace_list(ns_list);
        for (auto& peer : conn_mgr_.peers()) {
            if (peer->is_client) continue;
            auto conn = peer->connection;
            auto payload_copy = announce_payload;
            asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
                co_await conn->send_message(wire::TransportMsgType_SyncNamespaceAnnounce,
                                             std::span<const uint8_t>(p));
            }, asio::detached);
        }
        spdlog::info("config reload: re-announced sync_namespaces to {} peers",
                     std::count_if(conn_mgr_.peers().begin(), conn_mgr_.peers().end(),
                                   [](const std::unique_ptr<PeerInfo>& p) { return !p->is_client; }));
    }

    // Reload cursor config -> sync_
    sync_.set_cursor_config(new_cfg.full_resync_interval, new_cfg.cursor_stale_seconds);
    {
        auto reset_count = storage_.reset_all_round_counters();
        spdlog::warn("SIGHUP: reset {} cursor round counters (next sync will be full resync)",
                     reset_count);
    }

    // Reload max_storage_bytes + max_ttl
    engine_.set_max_storage_bytes(new_cfg.max_storage_bytes);
    engine_.set_max_ttl_seconds(new_cfg.max_ttl_seconds);
    if (new_cfg.max_storage_bytes > 0) {
        spdlog::info("config reload: max_storage_bytes={}", new_cfg.max_storage_bytes);
    } else {
        spdlog::info("config reload: max_storage_bytes=unlimited");
    }

    // Reload namespace quota config
    engine_.set_quota_config(new_cfg.namespace_quota_bytes,
                             new_cfg.namespace_quota_count,
                             new_cfg.namespace_quotas);
    if (new_cfg.namespace_quota_bytes > 0 || new_cfg.namespace_quota_count > 0) {
        spdlog::info("config reload: namespace_quota_bytes={} namespace_quota_count={}",
                     new_cfg.namespace_quota_bytes, new_cfg.namespace_quota_count);
    } else {
        spdlog::info("config reload: namespace quotas=disabled");
    }
    if (!new_cfg.namespace_quotas.empty()) {
        spdlog::info("config reload: {} per-namespace quota overrides",
                     new_cfg.namespace_quotas.size());
    }

    // Reload log level
    chromatindb::logging::set_level(new_cfg.log_level);
    spdlog::info("config reload: log_level={}", new_cfg.log_level);

    // Reload trusted_peers (already validated upfront above)
    {
        std::set<std::string> new_trusted;
        for (const auto& ip_str : new_cfg.trusted_peers) {
            asio::error_code ec;
            auto addr = asio::ip::make_address(ip_str, ec);
            if (!ec) {
                new_trusted.insert(addr.to_string());
            }
        }
        conn_mgr_.set_trusted_peers(std::move(new_trusted));
    }
    spdlog::info("config reload: trusted_peers={} addresses", conn_mgr_.trusted_peers().size());

    // Reload compaction interval -> sync_
    auto old_compaction = sync_.compaction_interval_hours();
    sync_.set_compaction_interval(new_cfg.compaction_interval_hours);
    if (new_cfg.compaction_interval_hours > 0) {
        spdlog::info("config reload: compaction_interval={}h", new_cfg.compaction_interval_hours);
    } else {
        spdlog::info("config reload: compaction=disabled");
    }
    // CR-01: Cancel only the compaction timer to restart with new interval.
    // Previously called cancel_timers() which also cancelled sync_timer_loop
    // and cursor_compaction_loop -- those loops exit permanently on cancel
    // (they co_return on ec), so SIGHUP would kill periodic sync and cursor
    // compaction until daemon restart.
    sync_.cancel_compaction_timer();
    if (old_compaction == 0 && new_cfg.compaction_interval_hours > 0) {
        asio::co_spawn(ioc_, sync_.compaction_loop(), asio::detached);
    }

    // Reload 3 SIGHUP-reloadable constants (D-04)
    sync_.set_blob_transfer_timeout(new_cfg.blob_transfer_timeout);
    sync_.set_sync_timeout(new_cfg.sync_timeout);
    pex_.set_pex_interval(new_cfg.pex_interval);
    spdlog::info("config reload: blob_transfer_timeout={}s sync_timeout={}s pex_interval={}s",
                 new_cfg.blob_transfer_timeout, new_cfg.sync_timeout, new_cfg.pex_interval);
    // NOTE: strike_threshold and strike_cooldown are NOT reloaded (D-05: restart required)

    // Reload bootstrap_peers: detect newly-added entries, register them, and
    // dial them immediately. Without this, `add-peer` + SIGHUP would update
    // config.json but the new peer would never be connected until restart.
    // Removals require daemon restart (active reconnect loops are not torn
    // down here to keep the logic simple and correct).
    {
        auto& known = conn_mgr_.bootstrap_addresses();
        std::vector<std::string> added;
        for (const auto& addr : new_cfg.bootstrap_peers) {
            if (known.insert(addr).second) {
                pex_.known_addresses().insert(addr);
                added.push_back(addr);
            }
        }
        for (const auto& addr : added) {
            server_.connect_once(addr);
        }
        if (!added.empty()) {
            std::string joined;
            for (size_t i = 0; i < added.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += added[i];
            }
            spdlog::info("config reload: bootstrap_peers +{} ({})", added.size(), joined);
        }
    }

    // Reload metrics_bind
    metrics_collector_.set_metrics_bind(new_cfg.metrics_bind);
}

// =============================================================================
// SIGUSR1 metrics dump
// =============================================================================

void PeerManager::setup_sigusr1_handler() {
    sigusr1_signal_.add(SIGUSR1);
    asio::co_spawn(ioc_, sigusr1_loop(), asio::detached);
}

asio::awaitable<void> PeerManager::sigusr1_loop() {
    while (!stopping_) {
        auto [ec, sig] = co_await sigusr1_signal_.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        metrics_collector_.dump_metrics();
    }
}

} // namespace chromatindb::peer
