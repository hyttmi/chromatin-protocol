#include "db/peer/peer_manager.h"
#include "db/logging/logging.h"
#include "db/peer/sync_reject.h"
#include "db/sync/reconciliation.h"
#include "db/util/hex.h"
#include "db/version.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fcntl.h>    // O_WRONLY, O_CREAT, O_TRUNC, O_RDONLY, O_DIRECTORY
#include <unistd.h>   // ::write, ::fsync, ::close, ::open

#include <algorithm>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_set>

namespace chromatindb::peer {

namespace {

using chromatindb::util::to_hex;

/// Token bucket rate limiter: returns true if tokens consumed, false if rate exceeded.
/// Caller must check rate_bytes_per_sec > 0 before calling (0 = disabled).
bool try_consume_tokens(PeerInfo& peer, uint64_t bytes,
                        uint64_t rate_bytes_per_sec, uint64_t burst_bytes) {
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    uint64_t elapsed_ms = now_ms - peer.bucket_last_refill;
    // Cap elapsed to prevent intermediate overflow in multiplication
    uint64_t max_elapsed = (burst_bytes * 1000) / rate_bytes_per_sec + 1;
    elapsed_ms = std::min(elapsed_ms, max_elapsed);
    uint64_t refill = (rate_bytes_per_sec * elapsed_ms) / 1000;
    peer.bucket_tokens = std::min(peer.bucket_tokens + refill, burst_bytes);
    peer.bucket_last_refill = now_ms;
    if (bytes > peer.bucket_tokens) {
        return false;
    }
    peer.bucket_tokens -= bytes;
    return true;
}

/// Convert 64-char hex string to 32-byte namespace ID.
/// Caller must ensure hex.size() == 64 and all chars are valid hex.
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
    , sync_proto_(engine, storage, pool)
    , sighup_signal_(ioc)
    , sigusr1_signal_(ioc)
    , config_path_(config_path) {
    // Initialize rate limit parameters from config
    rate_limit_bytes_per_sec_ = config.rate_limit_bytes_per_sec;
    rate_limit_burst_ = config.rate_limit_burst;

    // Initialize sync rate limit parameters from config
    sync_cooldown_seconds_ = config.sync_cooldown_seconds;
    max_sync_sessions_ = config.max_sync_sessions;

    // Initialize expiry scan interval from config
    expiry_scan_interval_seconds_ = config.expiry_scan_interval_seconds;

    // Initialize compaction interval from config
    compaction_interval_hours_ = config.compaction_interval_hours;

    // Initialize cursor config parameters
    full_resync_interval_ = config.full_resync_interval;
    cursor_stale_seconds_ = config.cursor_stale_seconds;

    // Initialize namespace filter from config
    for (const auto& hex : config.sync_namespaces) {
        sync_namespaces_.insert(hex_to_namespace(hex));
    }

    // Initialize trusted peers from config (canonicalize via Asio parsing)
    for (const auto& ip_str : config.trusted_peers) {
        asio::error_code ec;
        auto addr = asio::ip::make_address(ip_str, ec);
        if (!ec) {
            trusted_peers_.insert(addr.to_string());
        }
    }

    // Track bootstrap addresses
    for (const auto& addr : config.bootstrap_peers) {
        bootstrap_addresses_.insert(addr);
        known_addresses_.insert(addr);
    }

    // Wire trust check for lightweight handshake
    server_.set_trust_check([this](const asio::ip::address& addr) {
        return is_trusted_address(addr);
    });

    // Wire thread pool for crypto offload
    server_.set_pool(pool);

    // Wire server callbacks
    server_.set_accept_filter([this]() { return should_accept_connection(); });

    server_.set_on_connected([this](net::Connection::Ptr conn) {
        on_peer_connected(conn);
    });

    server_.set_on_disconnected([this](net::Connection::Ptr conn) {
        on_peer_disconnected(conn);
    });

    // UDS acceptor setup (only if uds_path is configured)
    if (!config.uds_path.empty()) {
        uds_acceptor_ = std::make_unique<net::UdsAcceptor>(
            config.uds_path, identity, ioc);

        // Wire same callbacks as TCP server — UDS connections get identical treatment
        uds_acceptor_->set_accept_filter([this]() { return should_accept_connection(); });
        uds_acceptor_->set_on_connected([this](net::Connection::Ptr conn) {
            on_peer_connected(conn);
        });
        uds_acceptor_->set_on_disconnected([this](net::Connection::Ptr conn) {
            on_peer_disconnected(conn);
        });
        uds_acceptor_->set_pool(pool);
    }

    // Set up sync-received blob notification callback (unified fan-out)
    sync_proto_.set_on_blob_ingested(
        [this](const std::array<uint8_t, 32>& ns, const std::array<uint8_t, 32>& hash,
               uint64_t seq, uint32_t size, bool tombstone,
               net::Connection::Ptr source) {
            on_blob_ingested(ns, hash, seq, size, tombstone, std::move(source));
        });
}

void PeerManager::start() {
    start_time_ = std::chrono::steady_clock::now();

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

    // Set up SIGHUP handler for config reload (only if config path was provided)
    if (!config_path_.empty()) {
        setup_sighup_handler();
        spdlog::info("SIGHUP reload: enabled (config: {})", config_path_.string());
    } else {
        spdlog::info("SIGHUP reload: disabled (no --config provided)");
    }

    // Set up SIGUSR1 handler for metrics dump (always enabled, no config dependency)
    setup_sigusr1_handler();

    // Load persisted peers before starting server
    load_persisted_peers();

    // Startup cursor cleanup: remove cursors for peers no longer known
    {
        std::vector<std::array<uint8_t, 32>> known_hashes;
        for (const auto& pp : persisted_peers_) {
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

    // Register shutdown callback (save peers before drain)
    server_.set_on_shutdown([this]() {
        stopping_ = true;
        save_persisted_peers();  // Save while connection list is still accurate
        if (uds_acceptor_) uds_acceptor_->stop();
        sighup_signal_.cancel();
        sigusr1_signal_.cancel();
        cancel_all_timers();
    });

    // Connect to persisted peers (in addition to bootstrap peers from server_.start())
    for (const auto& pp : persisted_peers_) {
        if (!bootstrap_addresses_.count(pp.address)) {
            known_addresses_.insert(pp.address);
            server_.connect_once(pp.address);
        }
    }

    // Start periodic sync timer
    asio::co_spawn(ioc_, sync_timer_loop(), asio::detached);

    // Start periodic peer exchange timer
    asio::co_spawn(ioc_, pex_timer_loop(), asio::detached);

    // Start periodic peer list flush timer (30s)
    asio::co_spawn(ioc_, peer_flush_timer_loop(), asio::detached);

    // Start cancellable expiry scan coroutine
    asio::co_spawn(ioc_, expiry_scan_loop(), asio::detached);

    // Start periodic metrics log timer (60s)
    asio::co_spawn(ioc_, metrics_timer_loop(), asio::detached);

    // Start cursor compaction timer (6h)
    asio::co_spawn(ioc_, cursor_compaction_loop(), asio::detached);

    // Storage compaction: only spawn when enabled (non-zero interval)
    if (compaction_interval_hours_ > 0) {
        asio::co_spawn(ioc_, compaction_loop(), asio::detached);
    }

    // Inactivity sweep: only spawn when enabled (non-zero timeout)
    if (config_.inactivity_timeout_seconds > 0) {
        asio::co_spawn(ioc_, inactivity_check_loop(), asio::detached);
    }
}

void PeerManager::cancel_all_timers() {
    if (expiry_timer_) expiry_timer_->cancel();
    if (sync_timer_) sync_timer_->cancel();
    if (pex_timer_) pex_timer_->cancel();
    if (flush_timer_) flush_timer_->cancel();
    if (metrics_timer_) metrics_timer_->cancel();
    if (cursor_compaction_timer_) cursor_compaction_timer_->cancel();
    if (compaction_timer_) compaction_timer_->cancel();
    if (inactivity_timer_) inactivity_timer_->cancel();
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

size_t PeerManager::peer_count() const {
    return peers_.size();
}

size_t PeerManager::bootstrap_peer_count() const {
    size_t count = 0;
    for (const auto& p : peers_) {
        if (p.is_bootstrap) ++count;
    }
    return count;
}

// =============================================================================
// Connection callbacks
// =============================================================================

bool PeerManager::should_accept_connection() {
    return peers_.size() < config_.max_peers;
}

void PeerManager::on_peer_connected(net::Connection::Ptr conn) {
    // ACL check: derive peer namespace hash and verify against allowed list.
    // Branch on connection type: UDS = client, TCP = peer.
    // Must happen BEFORE adding to peers_ so unauthorized peers never see data.
    auto peer_ns = crypto::sha3_256(conn->peer_pubkey());

    bool allowed = conn->is_uds()
        ? acl_.is_client_allowed(std::span<const uint8_t, 32>(peer_ns))
        : acl_.is_peer_allowed(std::span<const uint8_t, 32>(peer_ns));

    if (!allowed) {
        auto full_hex = to_hex(std::span<const uint8_t>(peer_ns.data(), peer_ns.size()), 32);
        spdlog::warn("access denied ({}): namespace={} ip={}",
                     conn->is_uds() ? "client" : "peer",
                     full_hex, conn->remote_address());
        // Signal ACL rejection to Server for backoff tracking (outbound only)
        if (!conn->connect_address().empty()) {
            server_.notify_acl_rejected(conn->connect_address());
        }
        ++metrics_.peers_disconnected_total;
        conn->close();  // Silent close, no goodbye
        return;
    }

    PeerInfo info;
    info.connection = conn;
    info.address = conn->remote_address();
    info.is_bootstrap = false;
    info.strike_count = 0;
    info.syncing = false;

    // Initialize token bucket for rate limiting (full burst capacity on connect)
    info.bucket_tokens = rate_limit_burst_;
    info.bucket_last_refill = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Initialize last_message_time for inactivity detection (CONN-03)
    info.last_message_time = info.bucket_last_refill;

    // Check if this connection is to a bootstrap peer
    // Note: remote_address includes port, bootstrap_addresses may not match exactly
    // We check if any bootstrap address is a prefix of the remote address
    for (const auto& bp : bootstrap_addresses_) {
        // Simple check: bootstrap is "host:port", remote is "host:port"
        // They may differ in port (bootstrap is listen port, remote is ephemeral)
        // So we check host part only
        auto bp_colon = bp.rfind(':');
        auto ra_colon = info.address.rfind(':');
        if (bp_colon != std::string::npos && ra_colon != std::string::npos) {
            auto bp_host = bp.substr(0, bp_colon);
            auto ra_host = info.address.substr(0, ra_colon);
            if (bp_host == ra_host) {
                info.is_bootstrap = true;
                break;
            }
        }
    }

    auto ns_hex = to_hex(conn->peer_pubkey(), 8);

    // Connection dedup: check if we already have a connection from this peer namespace.
    // When two nodes are mutual bootstrap peers, both initiate connections simultaneously.
    // We use a deterministic tie-break so both sides independently close the same connection.
    {
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            auto existing_ns = crypto::sha3_256(it->connection->peer_pubkey());
            if (existing_ns == peer_ns) {
                // Deterministic tie-break: the node with the lower namespace_id keeps
                // its initiated (outbound) connection. Since both sides see the same
                // pair of namespace IDs, they will independently close the same connection.
                auto own_ns = identity_.namespace_id();
                bool own_ns_lower = std::lexicographical_compare(
                    own_ns.begin(), own_ns.end(),
                    peer_ns.data(), peer_ns.data() + 32);

                bool keep_new;
                if (own_ns_lower) {
                    // We have the lower namespace: keep whichever connection we initiated
                    keep_new = conn->is_initiator();
                } else {
                    // They have the lower namespace: keep whichever they initiated (= we received)
                    keep_new = !conn->is_initiator();
                }

                if (keep_new) {
                    spdlog::info("duplicate connection from peer {}: closing existing, keeping new",
                                 ns_hex);
                    // Delay reconnect for the closed connection's bootstrap address
                    // to dampen reconnect-dedup cycles without permanently losing
                    // connectivity (stop_reconnect would prevent recovery if the
                    // kept connection later fails).
                    auto closed_addr = it->connection->connect_address();
                    if (!closed_addr.empty()) {
                        server_.delay_reconnect(closed_addr);
                    }
                    it->connection->close();
                    peers_.erase(it);
                } else {
                    spdlog::info("duplicate connection from peer {}: closing new, keeping existing",
                                 ns_hex);
                    // Delay reconnect for the closed connection's bootstrap address
                    auto closed_addr = conn->connect_address();
                    if (!closed_addr.empty()) {
                        server_.delay_reconnect(closed_addr);
                    }
                    conn->close();
                    return;  // Don't add to peers_
                }
                break;
            }
        }
    }

    spdlog::info("Connected to peer {}@{}", ns_hex, info.address);

    // Set up message routing
    conn->on_message([this](net::Connection::Ptr c, wire::TransportMsgType type,
                            std::vector<uint8_t> payload, uint32_t request_id) {
        on_peer_message(c, type, std::move(payload), request_id);
    });

    peers_.push_back(info);
    peers_.back().sync_inbox.clear();
    ++metrics_.peers_connected_total;

    // Track this peer's address
    known_addresses_.insert(info.address);

    // Persist successful connection and store pubkey hash for cursor cleanup
    update_persisted_peer(info.address, true);
    {
        auto pk_hash = crypto::sha3_256(conn->peer_pubkey());
        auto pk_hex = to_hex(std::span<const uint8_t>(pk_hash.data(), pk_hash.size()), 32);
        auto it = std::find_if(persisted_peers_.begin(), persisted_peers_.end(),
                               [&info](const PersistedPeer& p) { return p.address == info.address; });
        if (it != persisted_peers_.end()) {
            it->pubkey_hash = pk_hex;
        }
    }

    // Only the initiator (outbound) side triggers sync on connect.
    // The responder (inbound) side waits for SyncRequest from the peer.
    // This avoids both sides sending SyncRequest simultaneously.
    //
    // PEX exchange happens inline after sync completes (within the same coroutine)
    // to avoid concurrent sends that would desync AEAD nonces.
    if (conn->is_initiator()) {
        asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
            co_await run_sync_with_peer(conn);
        }, asio::detached);
    }
}

void PeerManager::on_peer_disconnected(net::Connection::Ptr conn) {
    auto ns_hex = to_hex(conn->peer_pubkey(), 8);
    bool graceful = conn->received_goodbye();
    spdlog::info("Peer {} disconnected ({})", ns_hex,
                 graceful ? "graceful" : "timeout");

    // Phase 80: Clean pending BlobFetch entries for this peer (D-09 fire-and-forget cleanup)
    for (auto it = pending_fetches_.begin(); it != pending_fetches_.end(); ) {
        if (it->second == conn) {
            it = pending_fetches_.erase(it);
        } else {
            ++it;
        }
    }

    // Remove from peers
    peers_.erase(
        std::remove_if(peers_.begin(), peers_.end(),
                       [&conn](const PeerInfo& p) {
                           return p.connection == conn;
                       }),
        peers_.end());
    ++metrics_.peers_disconnected_total;
}

// =============================================================================
// Message routing
// =============================================================================

void PeerManager::on_peer_message(net::Connection::Ptr conn,
                                   wire::TransportMsgType type,
                                   std::vector<uint8_t> payload,
                                   uint32_t request_id) {
    // Track last message time for inactivity detection (CONN-03).
    // Placed at the very top so ALL messages (even rate-limited ones) update the timestamp.
    {
        auto* peer = find_peer(conn);
        if (peer) {
            peer->last_message_time = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }

    // Universal byte accounting: all message types consume token bucket bytes (Phase 40).
    // Placed BEFORE message-type dispatch so every message is metered.
    // Ping/Pong are handled in Connection::message_loop and never reach here.
    if (rate_limit_bytes_per_sec_ > 0) {
        auto* peer = find_peer(conn);
        if (peer && !try_consume_tokens(*peer, payload.size(),
                                         rate_limit_bytes_per_sec_, rate_limit_burst_)) {
            // Bucket exhausted -- behavior depends on message type
            if (type == wire::TransportMsgType_Data || type == wire::TransportMsgType_Delete) {
                // Phase 18 behavior preserved: disconnect on Data/Delete exceed
                ++metrics_.rate_limited;
                spdlog::warn("rate limit exceeded by peer {} ({} bytes, limit {}B/s), disconnecting",
                             conn->remote_address(), payload.size(), rate_limit_bytes_per_sec_);
                asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
                return;
            }
            if (type == wire::TransportMsgType_SyncRequest) {
                // Reject new sync initiation when byte budget exceeded.
                // If peer is already syncing, silently drop to avoid concurrent write.
                auto* sync_peer = find_peer(conn);
                if (sync_peer && sync_peer->syncing) {
                    ++metrics_.sync_rejections;
                    spdlog::debug("sync request from {} dropped: byte rate exceeded + session active",
                                  conn->remote_address());
                    return;
                }
                send_sync_rejected(conn, SYNC_REJECT_BYTE_RATE);
                ++metrics_.sync_rejections;
                spdlog::debug("sync request from {} rejected: {}",
                              conn->remote_address(),
                              sync_reject_reason_string(SYNC_REJECT_BYTE_RATE));
                return;
            }
            // All other messages (sync in-progress, control): route normally.
            // Mid-sync cutoff handled at namespace boundary by initiator/responder.
            spdlog::debug("byte budget exceeded for peer {} (type={}), routing anyway",
                          conn->remote_address(), static_cast<int>(type));
        }
    }

    // ===========================================================================
    // Dispatch model (Phase 62 — CONC-03/CONC-04):
    //
    // INLINE (IO thread, no co_spawn):
    //   Subscribe, Unsubscribe, StorageFull, QuotaExceeded — fast in-memory ops
    //
    // COROUTINE (co_spawn on ioc_, stays on IO thread):
    //   ReadRequest, ListRequest, StatsRequest, ExistsRequest, NodeInfoRequest — synchronous storage calls
    //
    // COROUTINE with OFFLOAD (co_spawn on ioc_, engine offloads to pool, transfer back):
    //   Data, Delete — engine_.ingest()/delete_blob() offload crypto to pool,
    //   then co_await asio::post(ioc_) transfers back before send_message/metrics
    //
    // Sync messages: own concurrency model (sync_inbox queue + timer-cancel)
    // Ping/Pong/Goodbye: handled in Connection::message_loop, never reach here
    // ===========================================================================

    // Handle sync messages
    if (type == wire::TransportMsgType_SyncRequest) {
        // Peer wants to sync with us -- handle as responder
        auto* peer = find_peer(conn);
        if (!peer) return;

        // Step 0: Session limit check -- must be first.
        // When peer->syncing, the sync initiator coroutine owns the connection's
        // send path. Sending SyncRejected via a detached coroutine would race with
        // the initiator's writes, causing AEAD nonce desync. Silently drop instead;
        // the remote initiator will timeout (5s) and retry next interval.
        if (peer->syncing) {
            ++metrics_.sync_rejections;
            spdlog::debug("sync request from {} dropped: session active (avoiding concurrent write)",
                          conn->remote_address());
            return;
        }

        // Step 1: Cooldown check
        if (sync_cooldown_seconds_ > 0) {
            auto now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            uint64_t elapsed_ms = now_ms - peer->last_sync_initiated;
            uint64_t cooldown_ms = static_cast<uint64_t>(sync_cooldown_seconds_) * 1000;
            if (peer->last_sync_initiated > 0 && elapsed_ms < cooldown_ms) {
                send_sync_rejected(conn, SYNC_REJECT_COOLDOWN);
                ++metrics_.sync_rejections;
                spdlog::debug("sync request from {} rejected: {} ({} ms remaining)",
                              conn->remote_address(),
                              sync_reject_reason_string(SYNC_REJECT_COOLDOWN),
                              cooldown_ms - elapsed_ms);
                return;
            }
        }

        // Both checks passed -- accept sync and update timestamp
        peer->last_sync_initiated = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        peer->sync_inbox.clear();  // Fresh sync session
        asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
            co_await handle_sync_as_responder(conn);
        }, asio::detached);
        return;
    }

    // PeerListRequest: if peer is not syncing, start a PEX responder coroutine.
    // If syncing, route through the inbox (handled inline by the sync coroutine).
    if (type == wire::TransportMsgType_PeerListRequest) {
        auto* peer = find_peer(conn);
        if (peer) {
            if (peer->syncing) {
                route_sync_message(peer, type, std::move(payload));
            } else {
                // Start PEX responder: push the message into inbox first, then spawn handler
                route_sync_message(peer, type, std::move(payload));
                asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
                    co_await handle_pex_as_responder(conn);
                }, asio::detached);
            }
        }
        return;
    }

    if (type == wire::TransportMsgType_SyncAccept ||
        type == wire::TransportMsgType_SyncRejected ||
        type == wire::TransportMsgType_NamespaceList ||
        type == wire::TransportMsgType_ReconcileInit ||
        type == wire::TransportMsgType_ReconcileRanges ||
        type == wire::TransportMsgType_ReconcileItems ||
        type == wire::TransportMsgType_BlobRequest ||
        type == wire::TransportMsgType_BlobTransfer ||
        type == wire::TransportMsgType_SyncComplete ||
        type == wire::TransportMsgType_PeerListResponse) {
        auto* peer = find_peer(conn);
        if (peer) {
            route_sync_message(peer, type, std::move(payload));
        }
        return;
    }

    if (type == wire::TransportMsgType_Subscribe) {
        auto* peer = find_peer(conn);
        if (peer) {
            auto namespaces = decode_namespace_list(payload);
            for (const auto& ns : namespaces) {
                peer->subscribed_namespaces.insert(ns);
            }
            spdlog::debug("Peer {} subscribed to {} namespaces (total: {})",
                         peer_display_name(conn), namespaces.size(),
                         peer->subscribed_namespaces.size());
        }
        return;
    }

    if (type == wire::TransportMsgType_Unsubscribe) {
        auto* peer = find_peer(conn);
        if (peer) {
            auto namespaces = decode_namespace_list(payload);
            for (const auto& ns : namespaces) {
                peer->subscribed_namespaces.erase(ns);
            }
            spdlog::debug("Peer {} unsubscribed from {} namespaces (total: {})",
                         peer_display_name(conn), namespaces.size(),
                         peer->subscribed_namespaces.size());
        }
        return;
    }

    // Phase 80: Targeted blob fetch (PUSH-05, PUSH-06)
    if (type == wire::TransportMsgType_BlobNotify) {
        on_blob_notify(conn, std::move(payload));
        return;
    }

    if (type == wire::TransportMsgType_BlobFetch) {
        handle_blob_fetch(conn, std::move(payload));
        return;
    }

    if (type == wire::TransportMsgType_BlobFetchResponse) {
        handle_blob_fetch_response(conn, std::move(payload));
        return;
    }

    if (type == wire::TransportMsgType_Delete) {
        // Delete message -- process as blob deletion via coroutine (engine is async)
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                auto blob = wire::decode_blob(payload);
                // Namespace filter: drop blobs for filtered namespaces (silent, no strike)
                if (!sync_namespaces_.empty() &&
                    sync_namespaces_.find(blob.namespace_id) == sync_namespaces_.end()) {
                    spdlog::debug("dropping delete for filtered namespace from {}",
                                  conn->remote_address());
                    co_return;
                }
                auto result = co_await engine_.delete_blob(blob);
                // Transfer back to IO thread after engine offload (CONC-03).
                co_await asio::post(ioc_, asio::use_awaitable);
                if (result.accepted && result.ack.has_value()) {
                    // Build DeleteAck payload: [blob_hash:32][seq_num_be:8][status:1]
                    auto ack = result.ack.value();
                    std::vector<uint8_t> ack_payload(41);
                    std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
                    for (int i = 7; i >= 0; --i) {
                        ack_payload[32 + (7 - i)] = static_cast<uint8_t>(
                            ack.seq_num >> (i * 8));
                    }
                    ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
                    co_await conn->send_message(wire::TransportMsgType_DeleteAck,
                                                 std::span<const uint8_t>(ack_payload), request_id);
                    // Notify all peers about tombstone
                    if (ack.status == engine::IngestStatus::stored) {
                        on_blob_ingested(
                            blob.namespace_id,
                            ack.blob_hash,
                            ack.seq_num,
                            static_cast<uint32_t>(blob.data.size()),
                            true,      // tombstone
                            nullptr);  // Client write: notify ALL peers
                    }
                } else if (result.error.has_value()) {
                    spdlog::warn("delete rejected from {}: {}",
                                 conn->remote_address(), result.error_detail);
                    record_strike(conn, result.error_detail);
                }
            } catch (const std::exception& e) {
                spdlog::warn("malformed delete from {}: {}",
                             conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_StorageFull) {
        auto* peer = find_peer(conn);
        if (peer) {
            peer->peer_is_full = true;
            spdlog::info("Peer {} reported storage full, suppressing sync pushes",
                         peer_display_name(conn));
        }
        return;
    }

    if (type == wire::TransportMsgType_QuotaExceeded) {
        spdlog::info("Peer {} reported namespace quota exceeded",
                     peer_display_name(conn));
        // Informational only -- no action needed (peer will reject our blobs for that namespace)
        return;
    }

    if (type == wire::TransportMsgType_ReadRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 64) {
                    record_strike(conn, "ReadRequest too short");
                    co_return;
                }
                std::array<uint8_t, 32> ns{};
                std::array<uint8_t, 32> hash{};
                std::memcpy(ns.data(), payload.data(), 32);
                std::memcpy(hash.data(), payload.data() + 32, 32);

                auto blob = engine_.get_blob(ns, hash);
                if (blob.has_value()) {
                    auto encoded = wire::encode_blob(*blob);
                    std::vector<uint8_t> response(1 + encoded.size());
                    response[0] = 0x01;  // found
                    std::memcpy(response.data() + 1, encoded.data(), encoded.size());
                    co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                                 std::span<const uint8_t>(response), request_id);
                } else {
                    std::vector<uint8_t> response = {0x00};  // not found
                    co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                                 std::span<const uint8_t>(response), request_id);
                }
            } catch (const std::exception& e) {
                spdlog::warn("malformed ReadRequest from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_ListRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 44) {
                    record_strike(conn, "ListRequest too short");
                    co_return;
                }
                std::array<uint8_t, 32> ns{};
                std::memcpy(ns.data(), payload.data(), 32);
                uint64_t since_seq = 0;
                for (int i = 0; i < 8; ++i)
                    since_seq = (since_seq << 8) | payload[32 + i];
                uint32_t limit = 0;
                for (int i = 0; i < 4; ++i)
                    limit = (limit << 8) | payload[40 + i];

                constexpr uint32_t MAX_LIST_LIMIT = 100;
                if (limit == 0 || limit > MAX_LIST_LIMIT)
                    limit = MAX_LIST_LIMIT;

                // Fetch limit+1 to detect has_more
                auto refs = storage_.get_blob_refs_since(ns, since_seq, limit + 1);
                bool has_more = (refs.size() > limit);
                if (has_more)
                    refs.resize(limit);

                uint32_t count = static_cast<uint32_t>(refs.size());
                std::vector<uint8_t> response(4 + count * 40 + 1);
                // count (big-endian 4 bytes)
                response[0] = static_cast<uint8_t>(count >> 24);
                response[1] = static_cast<uint8_t>(count >> 16);
                response[2] = static_cast<uint8_t>(count >> 8);
                response[3] = static_cast<uint8_t>(count);
                // hash+seq pairs
                for (uint32_t i = 0; i < count; ++i) {
                    size_t off = 4 + i * 40;
                    std::memcpy(response.data() + off, refs[i].blob_hash.data(), 32);
                    uint64_t seq = refs[i].seq_num;
                    for (int b = 7; b >= 0; --b)
                        response[off + 32 + (7 - b)] = static_cast<uint8_t>(seq >> (b * 8));
                }
                // has_more flag
                response[4 + count * 40] = has_more ? 1 : 0;

                co_await conn->send_message(wire::TransportMsgType_ListResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("malformed ListRequest from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_StatsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 32) {
                    record_strike(conn, "StatsRequest too short");
                    co_return;
                }
                std::array<uint8_t, 32> ns{};
                std::memcpy(ns.data(), payload.data(), 32);

                auto quota = storage_.get_namespace_quota(ns);
                auto [byte_limit, count_limit] = engine_.effective_quota(ns);

                // Response: [blob_count_be:8][total_bytes_be:8][quota_bytes_be:8] = 24 bytes
                std::vector<uint8_t> response(24);
                for (int i = 7; i >= 0; --i)
                    response[7 - i] = static_cast<uint8_t>(quota.blob_count >> (i * 8));
                for (int i = 7; i >= 0; --i)
                    response[8 + 7 - i] = static_cast<uint8_t>(quota.total_bytes >> (i * 8));
                for (int i = 7; i >= 0; --i)
                    response[16 + 7 - i] = static_cast<uint8_t>(byte_limit >> (i * 8));

                co_await conn->send_message(wire::TransportMsgType_StatsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("malformed StatsRequest from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_ExistsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 64) {
                    record_strike(conn, "ExistsRequest too short");
                    co_return;
                }
                std::array<uint8_t, 32> ns{};
                std::array<uint8_t, 32> hash{};
                std::memcpy(ns.data(), payload.data(), 32);
                std::memcpy(hash.data(), payload.data() + 32, 32);

                bool exists = storage_.has_blob(ns, hash);

                // Response: [exists:1][blob_hash:32] = 33 bytes
                std::vector<uint8_t> response(33);
                response[0] = exists ? 0x01 : 0x00;
                std::memcpy(response.data() + 1, hash.data(), 32);

                co_await conn->send_message(wire::TransportMsgType_ExistsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("malformed ExistsRequest from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_NodeInfoRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id]() -> asio::awaitable<void> {
            try {
                // Gather node state
                std::string version = CHROMATINDB_VERSION;
                std::string git_hash = CHROMATINDB_GIT_HASH;
                uint64_t uptime = compute_uptime_seconds();
                uint32_t peers = static_cast<uint32_t>(peer_count());
                auto namespaces = storage_.list_namespaces();
                uint32_t ns_count = static_cast<uint32_t>(namespaces.size());

                // Total blobs: sum latest_seq_num across namespaces
                // (Note: this overcounts if blobs are deleted, but matches existing benchmark pattern)
                uint64_t total_blobs = 0;
                for (const auto& ns_info : namespaces)
                    total_blobs += ns_info.latest_seq_num;

                uint64_t storage_used = storage_.used_data_bytes();
                uint64_t storage_max = config_.max_storage_bytes;

                // Client-facing message types (all types relay allows through)
                static constexpr uint8_t supported[] = {
                    5, 6, 7, 8,                     // Ping, Pong, Goodbye, Data
                    17, 18, 19, 20, 21,             // Delete, DeleteAck, Subscribe, Unsubscribe, Notification
                    30, 31, 32, 33, 34, 35, 36,     // WriteAck, Read*, List*, Stats*
                    37, 38, 39, 40,                  // Exists*, NodeInfo*
                    41, 42, 43, 44, 45, 46,         // NamespaceList*, StorageStatus*, NamespaceStats*
                    47, 48, 49, 50, 51, 52,         // Metadata*, BatchExists*, DelegationList*
                    53, 54, 55, 56, 57, 58          // BatchRead*, PeerInfo*, TimeRange*
                };
                uint8_t types_count = static_cast<uint8_t>(sizeof(supported));

                // Build response
                size_t resp_size = 1 + version.size()
                                 + 1 + git_hash.size()
                                 + 8 + 4 + 4 + 8 + 8 + 8
                                 + 1 + types_count;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                // Version string (length-prefixed)
                response[off++] = static_cast<uint8_t>(version.size());
                std::memcpy(response.data() + off, version.data(), version.size());
                off += version.size();

                // Git hash string (length-prefixed)
                response[off++] = static_cast<uint8_t>(git_hash.size());
                std::memcpy(response.data() + off, git_hash.data(), git_hash.size());
                off += git_hash.size();

                // Uptime (big-endian uint64)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(uptime >> (i * 8));

                // Peer count (big-endian uint32)
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(peers >> (i * 8));

                // Namespace count (big-endian uint32)
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(ns_count >> (i * 8));

                // Total blobs (big-endian uint64)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(total_blobs >> (i * 8));

                // Storage bytes used (big-endian uint64)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(storage_used >> (i * 8));

                // Storage bytes max (big-endian uint64, 0=unlimited)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(storage_max >> (i * 8));

                // Supported types
                response[off++] = types_count;
                std::memcpy(response.data() + off, supported, types_count);

                co_await conn->send_message(wire::TransportMsgType_NodeInfoResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("NodeInfoRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_NamespaceListRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 36) {
                    record_strike(conn, "NamespaceListRequest too short");
                    co_return;
                }

                // Parse cursor and limit
                std::array<uint8_t, 32> after_ns{};
                std::memcpy(after_ns.data(), payload.data(), 32);
                uint32_t limit = 0;
                for (int i = 0; i < 4; ++i)
                    limit = (limit << 8) | payload[32 + i];
                if (limit == 0 || limit > 1000) limit = 100;

                // Get all namespaces and filter/paginate
                auto all_ns = storage_.list_namespaces();

                // Sort by namespace_id for deterministic cursor pagination
                std::sort(all_ns.begin(), all_ns.end(),
                    [](const auto& a, const auto& b) {
                        return a.namespace_id < b.namespace_id;
                    });

                // Find cursor position: skip entries <= after_ns
                static const std::array<uint8_t, 32> zero_ns{};
                auto it = all_ns.begin();
                if (after_ns != zero_ns) {
                    it = std::upper_bound(all_ns.begin(), all_ns.end(), after_ns,
                        [](const auto& cursor, const auto& info) {
                            return cursor < info.namespace_id;
                        });
                }

                // Collect up to limit entries
                std::vector<std::pair<std::array<uint8_t, 32>, uint64_t>> entries;
                uint32_t count = 0;
                for (; it != all_ns.end() && count < limit; ++it, ++count) {
                    auto quota = storage_.get_namespace_quota(it->namespace_id);
                    entries.emplace_back(it->namespace_id, quota.blob_count);
                }
                bool has_more = (it != all_ns.end());

                // Build response: [count:4][has_more:1][entries...]
                size_t resp_size = 4 + 1 + entries.size() * 40;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                // Count (big-endian uint32)
                uint32_t entry_count = static_cast<uint32_t>(entries.size());
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(entry_count >> (i * 8));

                // has_more flag
                response[off++] = has_more ? 0x01 : 0x00;

                // Entries: [namespace_id:32][blob_count:8]
                for (const auto& [ns_id, blob_count] : entries) {
                    std::memcpy(response.data() + off, ns_id.data(), 32);
                    off += 32;
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(blob_count >> (i * 8));
                }

                co_await conn->send_message(wire::TransportMsgType_NamespaceListResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("NamespaceListRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_StorageStatusRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id]() -> asio::awaitable<void> {
            try {
                uint64_t used_data = storage_.used_data_bytes();
                uint64_t max_storage = config_.max_storage_bytes;
                uint64_t tombstones = storage_.count_tombstones();
                auto namespaces = storage_.list_namespaces();
                uint32_t ns_count = static_cast<uint32_t>(namespaces.size());
                uint64_t total_blobs = 0;
                for (const auto& ns_info : namespaces)
                    total_blobs += ns_info.latest_seq_num;
                uint64_t mmap_bytes = storage_.used_bytes();

                // Build 44-byte response
                std::vector<uint8_t> response(44);
                size_t off = 0;

                // used_data_bytes (8)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(used_data >> (i * 8));
                // max_storage_bytes (8)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(max_storage >> (i * 8));
                // tombstone_count (8)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(tombstones >> (i * 8));
                // namespace_count (4)
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(ns_count >> (i * 8));
                // total_blobs (8)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(total_blobs >> (i * 8));
                // mmap_bytes (8)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(mmap_bytes >> (i * 8));

                co_await conn->send_message(wire::TransportMsgType_StorageStatusResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("StorageStatusRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_NamespaceStatsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 32) {
                    record_strike(conn, "NamespaceStatsRequest too short");
                    co_return;
                }

                std::array<uint8_t, 32> ns{};
                std::memcpy(ns.data(), payload.data(), 32);

                // Check if namespace exists
                auto all_ns = storage_.list_namespaces();
                bool found = false;
                for (const auto& info : all_ns) {
                    if (info.namespace_id == ns) {
                        found = true;
                        break;
                    }
                }

                // Build 41-byte response
                std::vector<uint8_t> response(41, 0);
                size_t off = 0;

                response[off++] = found ? 0x01 : 0x00;

                if (found) {
                    auto quota = storage_.get_namespace_quota(ns);
                    uint64_t delegation_count = storage_.count_delegations(ns);
                    auto [quota_bytes_limit, quota_count_limit] = engine_.effective_quota(ns);

                    // blob_count (8)
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(quota.blob_count >> (i * 8));
                    // total_bytes (8)
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(quota.total_bytes >> (i * 8));
                    // delegation_count (8)
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(delegation_count >> (i * 8));
                    // quota_bytes_limit (8)
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(quota_bytes_limit >> (i * 8));
                    // quota_count_limit (8)
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(quota_count_limit >> (i * 8));
                }
                // If not found, bytes 1-40 remain zero (already initialized)

                co_await conn->send_message(wire::TransportMsgType_NamespaceStatsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("NamespaceStatsRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_MetadataRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 64) {
                    record_strike(conn, "MetadataRequest too short");
                    co_return;
                }
                std::array<uint8_t, 32> ns{};
                std::array<uint8_t, 32> hash{};
                std::memcpy(ns.data(), payload.data(), 32);
                std::memcpy(hash.data(), payload.data() + 32, 32);

                auto blob_opt = storage_.get_blob(ns, hash);
                if (!blob_opt) {
                    // Not found: 1-byte response
                    std::vector<uint8_t> response(1, 0x00);
                    co_await conn->send_message(wire::TransportMsgType_MetadataResponse,
                                                 std::span<const uint8_t>(response), request_id);
                    co_return;
                }

                const auto& blob = *blob_opt;
                uint64_t data_size = blob.data.size();
                uint16_t pubkey_len = static_cast<uint16_t>(blob.pubkey.size());

                // Get seq_num from seq_map
                uint64_t seq_num = 0;
                auto refs = storage_.get_blob_refs_since(ns, 0, UINT32_MAX);
                for (const auto& ref : refs) {
                    if (ref.blob_hash == hash) {
                        seq_num = ref.seq_num;
                        break;
                    }
                }

                // Response: status(1) + hash(32) + timestamp(8) + ttl(4) + size(8) + seq_num(8) + pubkey_len(2) + pubkey(N)
                size_t resp_size = 1 + 32 + 8 + 4 + 8 + 8 + 2 + pubkey_len;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                response[off++] = 0x01;  // found

                // blob_hash (32)
                std::memcpy(response.data() + off, hash.data(), 32);
                off += 32;

                // timestamp (8, big-endian)
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(blob.timestamp >> (i * 8));

                // ttl (4, big-endian)
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(blob.ttl >> (i * 8));

                // size (8, big-endian) -- raw data size per D-03
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(data_size >> (i * 8));

                // seq_num (8, big-endian) -- per D-02
                for (int i = 7; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(seq_num >> (i * 8));

                // pubkey_len (2, big-endian)
                response[off++] = static_cast<uint8_t>(pubkey_len >> 8);
                response[off++] = static_cast<uint8_t>(pubkey_len & 0xFF);

                // pubkey (N bytes)
                std::memcpy(response.data() + off, blob.pubkey.data(), pubkey_len);
                off += pubkey_len;

                co_await conn->send_message(wire::TransportMsgType_MetadataResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("MetadataRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_BatchExistsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 36) {
                    record_strike(conn, "BatchExistsRequest too short");
                    co_return;
                }
                std::array<uint8_t, 32> ns{};
                std::memcpy(ns.data(), payload.data(), 32);

                uint32_t count = 0;
                for (int i = 0; i < 4; ++i)
                    count = (count << 8) | payload[32 + i];

                if (count == 0 || count > 1024) {
                    record_strike(conn, "BatchExistsRequest invalid count");
                    co_return;
                }

                size_t expected_size = 36 + static_cast<size_t>(count) * 32;
                if (payload.size() < expected_size) {
                    record_strike(conn, "BatchExistsRequest payload too short for count");
                    co_return;
                }

                // Check each hash
                std::vector<uint8_t> response(count);
                for (uint32_t i = 0; i < count; ++i) {
                    std::array<uint8_t, 32> hash{};
                    std::memcpy(hash.data(), payload.data() + 36 + i * 32, 32);
                    response[i] = storage_.has_blob(ns, hash) ? 0x01 : 0x00;
                }

                co_await conn->send_message(wire::TransportMsgType_BatchExistsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("BatchExistsRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_DelegationListRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                if (payload.size() < 32) {
                    record_strike(conn, "DelegationListRequest too short");
                    co_return;
                }
                std::array<uint8_t, 32> ns{};
                std::memcpy(ns.data(), payload.data(), 32);

                auto entries = storage_.list_delegations(ns);

                // Build response: [count:4 BE] + count * [pk_hash:32][blob_hash:32]
                uint32_t entry_count = static_cast<uint32_t>(entries.size());
                size_t resp_size = 4 + entry_count * 64;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                // count (4, big-endian)
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(entry_count >> (i * 8));

                // entries
                for (const auto& entry : entries) {
                    std::memcpy(response.data() + off, entry.delegate_pk_hash.data(), 32);
                    off += 32;
                    std::memcpy(response.data() + off, entry.delegation_blob_hash.data(), 32);
                    off += 32;
                }

                co_await conn->send_message(wire::TransportMsgType_DelegationListResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("DelegationListRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_BatchReadRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                // Step 0: minimum payload 40 bytes = namespace(32) + cap_bytes(4) + count(4)
                if (payload.size() < 40) {
                    record_strike(conn, "BatchReadRequest too short");
                    co_return;
                }

                std::array<uint8_t, 32> ns{};
                std::memcpy(ns.data(), payload.data(), 32);

                // cap_bytes (4 bytes big-endian)
                uint32_t cap_bytes = 0;
                for (int i = 0; i < 4; ++i)
                    cap_bytes = (cap_bytes << 8) | payload[32 + i];

                // Default/clamp: 0 or >4MiB -> 4MiB
                static constexpr uint32_t MAX_CAP = 4194304;  // 4 MiB
                if (cap_bytes == 0 || cap_bytes > MAX_CAP)
                    cap_bytes = MAX_CAP;

                // count (4 bytes big-endian)
                uint32_t count = 0;
                for (int i = 0; i < 4; ++i)
                    count = (count << 8) | payload[36 + i];

                if (count == 0 || count > 256) {
                    record_strike(conn, "BatchReadRequest invalid count");
                    co_return;
                }

                size_t expected_size = 40 + static_cast<size_t>(count) * 32;
                if (payload.size() < expected_size) {
                    record_strike(conn, "BatchReadRequest payload too short for count");
                    co_return;
                }

                // Fetch blobs with cumulative size tracking
                uint8_t truncated = 0x00;
                uint32_t result_count = 0;
                uint64_t cumulative_size = 0;
                // Pre-build entries: vector of (status, hash, encoded data)
                struct Entry {
                    uint8_t status;
                    std::array<uint8_t, 32> hash;
                    std::vector<uint8_t> encoded;  // empty if not found
                };
                std::vector<Entry> entries;
                entries.reserve(count);

                for (uint32_t i = 0; i < count; ++i) {
                    std::array<uint8_t, 32> hash{};
                    std::memcpy(hash.data(), payload.data() + 40 + i * 32, 32);

                    auto blob = storage_.get_blob(ns, hash);
                    if (!blob) {
                        entries.push_back({0x00, hash, {}});
                        ++result_count;
                        continue;
                    }

                    auto encoded = wire::encode_blob(*blob);
                    uint64_t blob_size = encoded.size();

                    // Check if adding this blob exceeds cap
                    // Per D-05: include the blob that crosses the cap, then stop
                    cumulative_size += blob_size;
                    entries.push_back({0x01, hash, std::move(encoded)});
                    ++result_count;

                    if (cumulative_size >= cap_bytes) {
                        // Mark truncated if there are more hashes remaining
                        if (i + 1 < count)
                            truncated = 0x01;
                        break;
                    }
                }

                // Build response: [truncated:1][count:4 BE] + entries
                // Per-entry found:    [status:1(0x01)][hash:32][size:8 BE][data:size bytes]
                // Per-entry not found: [status:1(0x00)][hash:32]
                size_t resp_size = 5;  // truncated + count
                for (const auto& e : entries) {
                    resp_size += 1 + 32;  // status + hash
                    if (e.status == 0x01)
                        resp_size += 8 + e.encoded.size();  // size + data
                }

                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                response[off++] = truncated;

                // result_count (4 bytes big-endian)
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(result_count >> (i * 8));

                for (const auto& e : entries) {
                    response[off++] = e.status;
                    std::memcpy(response.data() + off, e.hash.data(), 32);
                    off += 32;

                    if (e.status == 0x01) {
                        uint64_t sz = e.encoded.size();
                        for (int i = 7; i >= 0; --i)
                            response[off++] = static_cast<uint8_t>(sz >> (i * 8));
                        std::memcpy(response.data() + off, e.encoded.data(), e.encoded.size());
                        off += e.encoded.size();
                    }
                }

                co_await conn->send_message(wire::TransportMsgType_BatchReadResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("BatchReadRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_PeerInfoRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                // D-08: Empty payload is valid (0 bytes). No minimum size check needed.

                // D-10: Trust detection -- UDS connections or trusted IP addresses get full detail
                bool trusted = conn->is_uds();
                if (!trusted) {
                    // Parse IP from remote_address (format: "ip:port")
                    auto addr_str = conn->remote_address();
                    auto colon_pos = addr_str.rfind(':');
                    if (colon_pos != std::string::npos) {
                        asio::error_code ec;
                        auto addr = asio::ip::make_address(addr_str.substr(0, colon_pos), ec);
                        if (!ec)
                            trusted = is_trusted_address(addr);
                    }
                }

                uint32_t pc = static_cast<uint32_t>(peer_count());
                uint32_t bc = static_cast<uint32_t>(bootstrap_peer_count());

                if (!trusted) {
                    // D-09 untrusted: [peer_count:4 BE][bootstrap_count:4 BE]
                    std::vector<uint8_t> response(8);
                    size_t off = 0;
                    for (int i = 3; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(pc >> (i * 8));
                    for (int i = 3; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(bc >> (i * 8));
                    co_await conn->send_message(wire::TransportMsgType_PeerInfoResponse,
                                                 std::span<const uint8_t>(response), request_id);
                    co_return;
                }

                // D-09 trusted/UDS: full detail
                // Build: [peer_count:4 BE][bootstrap_count:4 BE] + peer_count entries
                // Per entry: [addr_len:2 BE][addr:N][is_bootstrap:1][syncing:1][peer_is_full:1][connected_duration_ms:8 BE]

                auto now_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());

                // Pre-compute response size
                size_t resp_size = 8;  // peer_count + bootstrap_count
                for (const auto& peer : peers_) {
                    resp_size += 2 + peer.address.size() + 1 + 1 + 1 + 8;
                }

                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(pc >> (i * 8));
                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(bc >> (i * 8));

                for (const auto& peer : peers_) {
                    // address length (2 bytes big-endian)
                    uint16_t addr_len = static_cast<uint16_t>(peer.address.size());
                    response[off++] = static_cast<uint8_t>(addr_len >> 8);
                    response[off++] = static_cast<uint8_t>(addr_len & 0xFF);
                    // address bytes
                    std::memcpy(response.data() + off, peer.address.data(), addr_len);
                    off += addr_len;
                    // flags
                    response[off++] = peer.is_bootstrap ? 0x01 : 0x00;
                    response[off++] = peer.syncing ? 0x01 : 0x00;
                    response[off++] = peer.peer_is_full ? 0x01 : 0x00;
                    // connected_duration_ms (8 bytes big-endian)
                    uint64_t duration_ms = (peer.last_message_time > 0) ? (now_ms - peer.last_message_time) : 0;
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(duration_ms >> (i * 8));
                }

                co_await conn->send_message(wire::TransportMsgType_PeerInfoResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("PeerInfoRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_TimeRangeRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                // Step 0: minimum payload 52 bytes = namespace(32) + start(8) + end(8) + limit(4)
                if (payload.size() < 52) {
                    record_strike(conn, "TimeRangeRequest too short");
                    co_return;
                }

                std::array<uint8_t, 32> ns{};
                std::memcpy(ns.data(), payload.data(), 32);

                // start_timestamp (8 bytes big-endian, seconds)
                uint64_t start_ts = 0;
                for (int i = 0; i < 8; ++i)
                    start_ts = (start_ts << 8) | payload[32 + i];

                // end_timestamp (8 bytes big-endian, seconds)
                uint64_t end_ts = 0;
                for (int i = 0; i < 8; ++i)
                    end_ts = (end_ts << 8) | payload[40 + i];

                // D-17: strike if start > end
                if (start_ts > end_ts) {
                    record_strike(conn, "TimeRangeRequest invalid range (start > end)");
                    co_return;
                }

                // limit (4 bytes big-endian)
                uint32_t limit = 0;
                for (int i = 0; i < 4; ++i)
                    limit = (limit << 8) | payload[48 + i];

                // D-14: cap at 100, default 100 if 0 or >100
                static constexpr uint32_t MAX_LIMIT = 100;
                if (limit == 0 || limit > MAX_LIMIT)
                    limit = MAX_LIMIT;

                // D-13/D-15: Scan seq_map, read timestamps, filter by range
                // Scan limit: 10,000 entries max
                static constexpr uint32_t SCAN_LIMIT = 10000;
                auto refs = storage_.get_blob_refs_since(ns, 0, SCAN_LIMIT);

                struct ResultEntry {
                    std::array<uint8_t, 32> blob_hash;
                    uint64_t seq_num;
                    uint64_t timestamp;
                };
                std::vector<ResultEntry> results;
                results.reserve(limit);

                uint8_t truncated = 0x00;

                for (const auto& ref : refs) {
                    // Read blob to get timestamp
                    auto blob = storage_.get_blob(ns, ref.blob_hash);
                    if (!blob) continue;

                    if (blob->timestamp >= start_ts && blob->timestamp <= end_ts) {
                        results.push_back({ref.blob_hash, ref.seq_num, blob->timestamp});
                        if (results.size() >= limit) {
                            truncated = 0x01;
                            break;
                        }
                    }
                }

                // If we consumed all SCAN_LIMIT refs but didn't hit result limit, also truncated
                if (refs.size() >= SCAN_LIMIT && results.size() < limit)
                    truncated = 0x01;

                // D-16: Response: [truncated:1][count:4 BE] + count * [blob_hash:32][seq_num:8 BE][timestamp:8 BE]
                uint32_t result_count = static_cast<uint32_t>(results.size());
                size_t resp_size = 5 + result_count * 48;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                response[off++] = truncated;

                for (int i = 3; i >= 0; --i)
                    response[off++] = static_cast<uint8_t>(result_count >> (i * 8));

                for (const auto& r : results) {
                    std::memcpy(response.data() + off, r.blob_hash.data(), 32);
                    off += 32;
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(r.seq_num >> (i * 8));
                    for (int i = 7; i >= 0; --i)
                        response[off++] = static_cast<uint8_t>(r.timestamp >> (i * 8));
                }

                co_await conn->send_message(wire::TransportMsgType_TimeRangeResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("TimeRangeRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_Data) {
        // Data message -- try to ingest as a blob via coroutine (engine is async)
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            try {
                auto blob = wire::decode_blob(payload);
                // Namespace filter: drop blobs for filtered namespaces (silent, no strike)
                if (!sync_namespaces_.empty() &&
                    sync_namespaces_.find(blob.namespace_id) == sync_namespaces_.end()) {
                    spdlog::debug("dropping data for filtered namespace from {}",
                                  conn->remote_address());
                    co_return;
                }
                auto result = co_await engine_.ingest(blob);
                // Transfer back to IO thread after engine offload (CONC-03).
                // engine_.ingest() internally calls crypto::offload(pool_, ...) which
                // resumes this coroutine on the thread pool. All subsequent code must
                // run on the IO thread: send_message (AEAD nonce safety),
                // on_blob_ingested (peers_ access), metrics_, record_strike.
                co_await asio::post(ioc_, asio::use_awaitable);
                // Send WriteAck for all accepted ingests (stored + duplicate)
                if (result.accepted && result.ack.has_value()) {
                    auto ack = result.ack.value();
                    std::vector<uint8_t> ack_payload(41);
                    std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
                    for (int i = 7; i >= 0; --i) {
                        ack_payload[32 + (7 - i)] = static_cast<uint8_t>(
                            ack.seq_num >> (i * 8));
                    }
                    ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
                    co_await conn->send_message(wire::TransportMsgType_WriteAck,
                                                 std::span<const uint8_t>(ack_payload), request_id);
                }
                if (result.accepted && result.ack.has_value() &&
                    result.ack->status == engine::IngestStatus::stored) {
                    on_blob_ingested(
                        blob.namespace_id,
                        result.ack->blob_hash,
                        result.ack->seq_num,
                        static_cast<uint32_t>(blob.data.size()),
                        wire::is_tombstone(blob.data),
                        nullptr);  // Client write: notify ALL peers
                    ++metrics_.ingests;
                } else if (result.accepted) {
                    ++metrics_.ingests;  // Duplicate or already-known blob
                } else if (!result.accepted && result.error.has_value()) {
                    ++metrics_.rejections;
                    if (*result.error == engine::IngestError::storage_full) {
                        // Send StorageFull to inform peer we cannot accept blobs
                        spdlog::warn("Storage full, notifying peer {}", peer_display_name(conn));
                        std::span<const uint8_t> empty{};
                        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty, request_id);
                    } else if (*result.error == engine::IngestError::quota_exceeded) {
                        // Send QuotaExceeded to inform peer namespace is over quota
                        spdlog::warn("Namespace quota exceeded, notifying peer {}",
                                     peer_display_name(conn));
                        ++metrics_.quota_rejections;
                        std::span<const uint8_t> empty{};
                        co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty, request_id);
                    } else if (*result.error == engine::IngestError::timestamp_rejected) {
                        // Timestamp too far in future/past -- receiver's decision, debug log only
                        spdlog::debug("Data from {}: timestamp rejected ({})",
                                      conn->remote_address(),
                                      result.error_detail.empty() ? "unknown" : result.error_detail);
                    } else {
                        spdlog::warn("invalid blob from peer {}: {}",
                                     conn->remote_address(),
                                     result.error_detail.empty() ? "validation failed" : result.error_detail);
                        record_strike(conn, result.error_detail);
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("malformed data message from peer {}: {}",
                             conn->remote_address(), e.what());
                record_strike(conn, e.what());
            }
        }, asio::detached);
        return;
    }

    // Other message types ignored (Ping/Pong/Goodbye handled by Connection)
}

// =============================================================================
// Sync message queue
// =============================================================================

void PeerManager::route_sync_message(PeerInfo* peer, wire::TransportMsgType type,
                                      std::vector<uint8_t> payload) {
    peer->sync_inbox.push_back({type, std::move(payload)});
    if (peer->sync_notify) {
        peer->sync_notify->cancel();  // Wake up waiting coroutine
    }
}

asio::awaitable<std::optional<SyncMessage>>
PeerManager::recv_sync_msg(PeerInfo* peer, std::chrono::seconds timeout) {
    // Ensure we're on the io_context thread before accessing sync_inbox.
    // After co_await offload() (crypto thread pool), the coroutine may be
    // running on the pool thread. The timer co_await below will transfer us
    // back, but the fast path (inbox non-empty) must also be on the right thread.
    co_await asio::post(ioc_, asio::use_awaitable);

    if (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        co_return msg;
    }
    asio::steady_timer timer(ioc_);
    peer->sync_notify = &timer;
    timer.expires_after(timeout);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    peer->sync_notify = nullptr;
    if (peer->sync_inbox.empty()) {
        co_return std::nullopt;  // Timeout
    }
    auto msg = std::move(peer->sync_inbox.front());
    peer->sync_inbox.pop_front();
    co_return msg;
}

// =============================================================================
// Sync orchestration
// =============================================================================

PeerManager::FullResyncReason PeerManager::check_full_resync(
    const storage::SyncCursor& cursor, uint64_t now) const {
    // Periodic: every Nth round (round_count 0 always triggers -- covers SIGHUP reset)
    if (full_resync_interval_ > 0 &&
        cursor.round_count % full_resync_interval_ == 0)
        return FullResyncReason::Periodic;
    // Time gap: too long since last sync
    if (cursor_stale_seconds_ > 0 &&
        cursor.last_sync_timestamp > 0 &&
        now - cursor.last_sync_timestamp > cursor_stale_seconds_)
        return FullResyncReason::TimeGap;
    return FullResyncReason::None;
}

asio::awaitable<void> PeerManager::run_sync_with_peer(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;
    peer->syncing = true;
    peer->sync_inbox.clear();  // Fresh sync session -- set up BEFORE sending

    sync::SyncStats total_stats;
    constexpr auto SYNC_TIMEOUT = std::chrono::seconds(30);

    // Send SyncRequest
    std::span<const uint8_t> empty{};
    if (!co_await conn->send_message(wire::TransportMsgType_SyncRequest, empty)) {
        peer->syncing = false;
        co_return;
    }

    // Wait for SyncAccept (or SyncRejected)
    auto accept_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
    if (!accept_msg || accept_msg->type != wire::TransportMsgType_SyncAccept) {
        if (accept_msg && accept_msg->type == wire::TransportMsgType_SyncRejected) {
            uint8_t reason = accept_msg->payload.empty() ? 0 : accept_msg->payload[0];
            auto reason_str = sync_reject_reason_string(reason);
            spdlog::info("sync with {}: rejected ({})", conn->remote_address(), reason_str);
        } else {
            spdlog::warn("sync with {}: no SyncAccept received", conn->remote_address());
        }
        peer->syncing = false;
        co_return;
    }

    // Phase A: Send our data (filtered by sync_namespaces)
    auto our_namespaces = engine_.list_namespaces();
    if (!sync_namespaces_.empty()) {
        std::erase_if(our_namespaces, [this](const storage::NamespaceInfo& ns) {
            return sync_namespaces_.find(ns.namespace_id) == sync_namespaces_.end();
        });
    }
    auto ns_payload = sync::SyncProtocol::encode_namespace_list(our_namespaces);
    if (!co_await conn->send_message(wire::TransportMsgType_NamespaceList, ns_payload)) {
        peer->syncing = false;
        co_return;
    }

    // Phase A (continued): Receive peer's NamespaceList
    auto ns_msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
    if (!ns_msg || ns_msg->type != wire::TransportMsgType_NamespaceList) {
        spdlog::warn("sync with {}: expected NamespaceList", conn->remote_address());
        peer->syncing = false;
        co_return;
    }
    auto peer_namespaces = sync::SyncProtocol::decode_namespace_list(ns_msg->payload);

    // Cursor decision: determine which namespaces to skip
    auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
    auto now_ts = static_cast<uint64_t>(std::time(nullptr));
    std::set<std::array<uint8_t, 32>> cursor_skip_namespaces;
    bool sync_is_full_resync = false;
    uint64_t cursor_hits_this_round = 0;
    uint64_t cursor_misses_this_round = 0;

    for (const auto& pns : peer_namespaces) {
        auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        if (cursor.has_value()) {
            auto reason = check_full_resync(*cursor, now_ts);
            if (reason != FullResyncReason::None) {
                sync_is_full_resync = true;
                if (reason == FullResyncReason::Periodic) {
                    spdlog::info("sync with {}: full resync (periodic, round {})",
                                 conn->remote_address(), cursor->round_count);
                } else {
                    spdlog::warn("sync with {}: full resync (time gap, last={}s ago)",
                                 conn->remote_address(),
                                 now_ts - cursor->last_sync_timestamp);
                }
                break;
            }
        }
    }

    if (!sync_is_full_resync) {
        for (const auto& pns : peer_namespaces) {
            auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
            if (cursor.has_value()) {
                if (cursor->seq_num == pns.latest_seq_num) {
                    cursor_skip_namespaces.insert(pns.namespace_id);
                    cursor_hits_this_round++;
                    spdlog::debug("sync cursor hit: ns={} seq={}", to_hex(pns.namespace_id, 8), pns.latest_seq_num);
                } else if (pns.latest_seq_num < cursor->seq_num) {
                    spdlog::warn("sync cursor mismatch: ns={} remote_seq={} stored_seq={}, resetting",
                                 to_hex(pns.namespace_id, 8), pns.latest_seq_num, cursor->seq_num);
                    storage_.delete_sync_cursor(peer_hash, pns.namespace_id);
                    cursor_misses_this_round++;
                } else {
                    cursor_misses_this_round++;
                    spdlog::debug("sync cursor miss: ns={} remote_seq={} stored_seq={}",
                                  to_hex(pns.namespace_id, 8), pns.latest_seq_num, cursor->seq_num);
                }
            } else {
                cursor_misses_this_round++;
            }
        }
    } else {
        cursor_misses_this_round = peer_namespaces.size();
        ++metrics_.full_resyncs;
    }

    // Phase B: Set Reconciliation (initiator drives)
    // Build union of namespaces from both sides for reconciliation
    std::set<std::array<uint8_t, 32>> all_namespaces;
    for (const auto& ns_info : our_namespaces) all_namespaces.insert(ns_info.namespace_id);
    for (const auto& pns : peer_namespaces) all_namespaces.insert(pns.namespace_id);

    // Collect missing hashes per namespace (what WE need from peer)
    std::map<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>> missing_per_ns;

    sync::Hash32 max_hash;
    max_hash.fill(0xFF);

    for (const auto& ns : all_namespaces) {
        // Byte budget check: if token bucket is exhausted, stop starting new namespaces
        if (rate_limit_bytes_per_sec_ > 0 && peer->bucket_tokens == 0) {
            spdlog::info("sync with {}: byte budget exhausted after {} namespaces, sending SyncComplete early",
                         conn->remote_address(), missing_per_ns.size());
            break;  // Fall through to SyncComplete
        }

        // Namespace filter
        if (!sync_namespaces_.empty() &&
            sync_namespaces_.find(ns) == sync_namespaces_.end()) {
            continue;
        }

        bool cursor_hit = cursor_skip_namespaces.count(ns) > 0;
        if (cursor_hit) {
            ++metrics_.cursor_hits;
        } else {
            ++metrics_.cursor_misses;
        }

        // Snapshot our hashes ONCE (Pitfall 6: stale hash vector)
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        std::sort(our_hashes.begin(), our_hashes.end());

        auto our_fp = sync::xor_fingerprint(our_hashes, 0, our_hashes.size());

        // Send ReconcileInit for this namespace
        sync::ReconcileInit init;
        init.version = sync::RECONCILE_VERSION;
        std::memcpy(init.namespace_id.data(), ns.data(), 32);
        init.count = static_cast<uint32_t>(our_hashes.size());
        init.fingerprint = our_fp;

        auto init_payload = sync::encode_reconcile_init(init);
        if (!co_await conn->send_message(wire::TransportMsgType_ReconcileInit, init_payload)) {
            peer->syncing = false;
            co_return;
        }

        // Multi-round reconciliation loop
        // Collect all items the peer reveals via ItemList ranges
        std::vector<sync::Hash32> peer_items;

        for (uint32_t round = 0; round < sync::MAX_RECONCILE_ROUNDS; ++round) {
            auto msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
            if (!msg) {
                spdlog::warn("sync with {}: timeout during reconciliation for ns={}",
                             conn->remote_address(), to_hex(ns, 8));
                peer->syncing = false;
                co_return;
            }

            if (msg->type == wire::TransportMsgType_ReconcileRanges) {
                auto decoded = sync::decode_reconcile_ranges(msg->payload);
                if (!decoded) {
                    spdlog::warn("sync with {}: invalid ReconcileRanges", conn->remote_address());
                    peer->syncing = false;
                    co_return;
                }

                // Empty ranges = reconciliation done for this namespace
                if (decoded->ranges.empty()) break;

                // Check if all ranges are resolved (no Fingerprint ranges).
                // If so, this is the responder's final exchange -- extract items
                // and send our items back via ReconcileItems, then done.
                bool has_fingerprint = false;
                for (const auto& r : decoded->ranges) {
                    if (r.mode == sync::RangeMode::Fingerprint) {
                        has_fingerprint = true;
                        break;
                    }
                }

                if (!has_fingerprint) {
                    // Final exchange: collect peer items from ItemList ranges
                    for (const auto& r : decoded->ranges) {
                        if (r.mode == sync::RangeMode::ItemList) {
                            peer_items.insert(peer_items.end(),
                                              r.items.begin(), r.items.end());
                        }
                    }
                    // Send our items for the resolved ranges so the peer can diff
                    // Collect all our items across the resolved ranges
                    sync::Hash32 lower = sync::Hash32{};
                    std::vector<sync::Hash32> our_range_items;
                    for (const auto& r : decoded->ranges) {
                        if (r.mode == sync::RangeMode::ItemList) {
                            auto [b, e] = sync::range_indices(our_hashes, lower, r.upper_bound);
                            for (size_t idx = b; idx < e; ++idx) {
                                our_range_items.push_back(our_hashes[idx]);
                            }
                        }
                        lower = r.upper_bound;
                    }
                    auto items_payload = sync::encode_reconcile_items(ns, our_range_items);
                    if (!co_await conn->send_message(wire::TransportMsgType_ReconcileItems,
                                                     items_payload)) {
                        peer->syncing = false;
                        co_return;
                    }
                    break;
                }

                auto result = sync::process_ranges(our_hashes, decoded->ranges);
                peer_items.insert(peer_items.end(),
                                  result.have_items.begin(), result.have_items.end());

                // Send our response ranges
                auto resp_payload = sync::encode_reconcile_ranges(ns, result.response_ranges);
                if (!co_await conn->send_message(wire::TransportMsgType_ReconcileRanges,
                                                 resp_payload)) {
                    peer->syncing = false;
                    co_return;
                }
            } else if (msg->type == wire::TransportMsgType_ReconcileItems) {
                // ReconcileItems = responder's final item exchange for this namespace
                auto decoded = sync::decode_reconcile_items(msg->payload);
                if (decoded) {
                    peer_items.insert(peer_items.end(),
                                      decoded->items.begin(), decoded->items.end());
                }
                break;  // Namespace reconciliation done
            } else {
                spdlog::warn("sync with {}: unexpected message type {} during reconciliation",
                             conn->remote_address(), static_cast<int>(msg->type));
                break;
            }
        }

        // Compute diff: peer items we don't have (only request if not cursor-hit)
        if (!cursor_hit) {
            std::vector<std::array<uint8_t, 32>> missing;
            {
                std::unordered_set<std::string> our_set;
                our_set.reserve(our_hashes.size());
                for (const auto& h : our_hashes) {
                    our_set.insert(std::string(reinterpret_cast<const char*>(h.data()), h.size()));
                }
                for (const auto& h : peer_items) {
                    std::string key(reinterpret_cast<const char*>(h.data()), h.size());
                    if (our_set.find(key) == our_set.end()) {
                        missing.push_back(h);
                    }
                }
            }
            if (!missing.empty()) {
                missing_per_ns[ns] = std::move(missing);
            }
        }
        total_stats.namespaces_synced++;
    }

    // Signal end of Phase B: all namespaces reconciled
    if (!co_await conn->send_message(wire::TransportMsgType_SyncComplete, empty)) {
        peer->syncing = false;
        co_return;
    }

    // Phase C: Exchange blobs one at a time using existing BlobRequest/BlobTransfer
    spdlog::debug("sync initiator {}: Phase C start, missing {} namespaces",
                  conn->remote_address(), missing_per_ns.size());
    for (const auto& [ns, missing] : missing_per_ns) {
        spdlog::debug("sync initiator {}: requesting {} blobs for ns {}",
                      conn->remote_address(), missing.size(), to_hex(ns, 4));
        for (size_t i = 0; i < missing.size(); i += MAX_HASHES_PER_REQUEST) {
            size_t batch_end = std::min(i + static_cast<size_t>(MAX_HASHES_PER_REQUEST),
                                        missing.size());
            std::vector<std::array<uint8_t, 32>> batch(
                missing.begin() + static_cast<ptrdiff_t>(i),
                missing.begin() + static_cast<ptrdiff_t>(batch_end));

            auto req_payload = sync::SyncProtocol::encode_blob_request(ns, batch);
            if (!co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload)) {
                peer->syncing = false;
                co_return;
            }

            uint32_t expected = static_cast<uint32_t>(batch.size());
            uint32_t received = 0;
            while (received < expected) {
                auto msg = co_await recv_sync_msg(peer, BLOB_TRANSFER_TIMEOUT);
                if (!msg) {
                    spdlog::warn("sync: timeout waiting for blob transfer from {}",
                                 conn->remote_address());
                    break;
                }

                spdlog::debug("sync initiator {}: Phase C got msg type={}",
                              conn->remote_address(), static_cast<int>(msg->type));
                if (msg->type == wire::TransportMsgType_BlobTransfer) {
                    auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
                    auto s = co_await sync_proto_.ingest_blobs(blobs, conn);
                    total_stats.blobs_received += s.blobs_received;
                    total_stats.storage_full_count += s.storage_full_count;
                    total_stats.quota_exceeded_count += s.quota_exceeded_count;
                    received++;
                } else if (msg->type == wire::TransportMsgType_BlobRequest) {
                    if (peer->peer_is_full) {
                        spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                        continue;
                    }
                    auto [req_ns, requested_hashes] =
                        sync::SyncProtocol::decode_blob_request(msg->payload);
                    for (const auto& hash : requested_hashes) {
                        auto blob = engine_.get_blob(req_ns, hash);
                        if (blob.has_value()) {
                            auto bt_payload =
                                sync::SyncProtocol::encode_single_blob_transfer(*blob);
                            co_await conn->send_message(
                                wire::TransportMsgType_BlobTransfer, bt_payload);
                            total_stats.blobs_sent++;
                        }
                    }
                }
            }
        }
    }

    // Handle remaining BlobRequests from peer (they may still need our blobs)
    while (true) {
        auto msg = co_await recv_sync_msg(peer, std::chrono::seconds(2));
        if (!msg) break;
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            if (peer->peer_is_full) {
                spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                continue;
            }
            auto [req_ns, requested_hashes] =
                sync::SyncProtocol::decode_blob_request(msg->payload);
            for (const auto& hash : requested_hashes) {
                auto blob = engine_.get_blob(req_ns, hash);
                if (blob.has_value()) {
                    auto bt_payload =
                        sync::SyncProtocol::encode_single_blob_transfer(*blob);
                    co_await conn->send_message(
                        wire::TransportMsgType_BlobTransfer, bt_payload);
                    total_stats.blobs_sent++;
                }
            }
        } else {
            break;
        }
    }

    // Post-sync cursor update: only update after successful sync completion (Pitfall 1)
    for (const auto& pns : peer_namespaces) {
        // Get existing cursor to preserve round_count continuity
        auto old_cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        storage::SyncCursor updated;
        updated.seq_num = pns.latest_seq_num;
        updated.round_count = (old_cursor ? old_cursor->round_count : 0) + 1;
        updated.last_sync_timestamp = now_ts;
        storage_.set_sync_cursor(peer_hash, pns.namespace_id, updated);
    }

    spdlog::info("Synced with peer {}: received {} blobs, sent {} blobs, {} namespaces "
                 "(cursor: {} hits, {} misses{})",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced,
                 cursor_hits_this_round, cursor_misses_this_round,
                 sync_is_full_resync ? ", full resync" : "");

    // Post-sync StorageFull signal: inform peer if we rejected blobs for capacity
    if (total_stats.storage_full_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty);
        spdlog::warn("Sent StorageFull to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.storage_full_count);
    }

    // Post-sync QuotaExceeded signal: inform peer if we rejected blobs for namespace quota
    if (total_stats.quota_exceeded_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty);
        spdlog::warn("Sent QuotaExceeded to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.quota_exceeded_count);
        metrics_.quota_rejections += total_stats.quota_exceeded_count;
    }

    // PEX exchange: send PeerListRequest and wait for response (inline, no concurrent send)
    // Skip in closed mode -- don't advertise or discover peers
    if (!acl_.is_peer_closed_mode()) {
        std::span<const uint8_t> empty{};
        if (!co_await conn->send_message(wire::TransportMsgType_PeerListRequest, empty)) {
            spdlog::debug("PEX: failed to send PeerListRequest to {}", conn->remote_address());
            peer->syncing = false;
            co_return;
        }

        // Wait for PeerListResponse (with timeout)
        auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
        if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListResponse) {
            handle_peer_list_response(conn, std::move(pex_msg->payload));
        }
    }

    ++metrics_.syncs;
    peer->syncing = false;
}

asio::awaitable<void> PeerManager::handle_sync_as_responder(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer) co_return;
    peer->syncing = true;

    sync::SyncStats total_stats;
    constexpr auto SYNC_TIMEOUT = std::chrono::seconds(30);

    // Send SyncAccept
    std::span<const uint8_t> empty{};
    co_await conn->send_message(wire::TransportMsgType_SyncAccept, empty);

    // Phase A: Send our NamespaceList (filtered by sync_namespaces)
    auto our_namespaces = engine_.list_namespaces();
    if (!sync_namespaces_.empty()) {
        std::erase_if(our_namespaces, [this](const storage::NamespaceInfo& ns) {
            return sync_namespaces_.find(ns.namespace_id) == sync_namespaces_.end();
        });
    }
    auto ns_payload = sync::SyncProtocol::encode_namespace_list(our_namespaces);
    co_await conn->send_message(wire::TransportMsgType_NamespaceList, ns_payload);

    // Phase A (continued): Receive peer's NamespaceList
    auto ns_msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
    if (!ns_msg || ns_msg->type != wire::TransportMsgType_NamespaceList) {
        spdlog::warn("sync responder {}: expected NamespaceList", conn->remote_address());
        peer->syncing = false;
        co_return;
    }
    auto peer_namespaces = sync::SyncProtocol::decode_namespace_list(ns_msg->payload);

    // Cursor decision (same logic as initiator, independently applied)
    auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
    auto now_ts = static_cast<uint64_t>(std::time(nullptr));
    std::set<std::array<uint8_t, 32>> cursor_skip_namespaces;
    bool sync_is_full_resync = false;
    uint64_t cursor_hits_this_round = 0;
    uint64_t cursor_misses_this_round = 0;

    for (const auto& pns : peer_namespaces) {
        auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        if (cursor.has_value()) {
            auto reason = check_full_resync(*cursor, now_ts);
            if (reason != FullResyncReason::None) {
                sync_is_full_resync = true;
                if (reason == FullResyncReason::Periodic) {
                    spdlog::debug("sync responder {}: full resync (periodic, round {})",
                                 conn->remote_address(), cursor->round_count);
                } else {
                    spdlog::warn("sync responder {}: full resync (time gap, last={}s ago)",
                                 conn->remote_address(),
                                 now_ts - cursor->last_sync_timestamp);
                }
                break;
            }
        }
    }

    if (!sync_is_full_resync) {
        for (const auto& pns : peer_namespaces) {
            auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
            if (cursor.has_value()) {
                if (cursor->seq_num == pns.latest_seq_num) {
                    cursor_skip_namespaces.insert(pns.namespace_id);
                    cursor_hits_this_round++;
                    spdlog::debug("sync responder cursor hit: ns={} seq={}", to_hex(pns.namespace_id, 8), pns.latest_seq_num);
                } else if (pns.latest_seq_num < cursor->seq_num) {
                    spdlog::warn("sync responder cursor mismatch: ns={} remote_seq={} stored_seq={}, resetting",
                                 to_hex(pns.namespace_id, 8), pns.latest_seq_num, cursor->seq_num);
                    storage_.delete_sync_cursor(peer_hash, pns.namespace_id);
                    cursor_misses_this_round++;
                } else {
                    cursor_misses_this_round++;
                }
            } else {
                cursor_misses_this_round++;
            }
        }
    } else {
        cursor_misses_this_round = peer_namespaces.size();
        ++metrics_.full_resyncs;
    }

    // Phase B: Respond to initiator-driven reconciliation
    // The initiator sends ReconcileInit for each namespace, or SyncComplete when done.
    // For each namespace, we exchange ReconcileRanges until resolved.
    std::map<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>> missing_per_ns;

    // Cache sorted hash vectors per namespace (Pitfall 6: snapshot once)
    std::map<std::array<uint8_t, 32>, std::vector<sync::Hash32>> ns_hash_cache;

    while (true) {
        // Byte budget check (responder): silently stop if budget exhausted
        // Per CONTEXT.md: "stop responding silently, let initiator hit SYNC_TIMEOUT (30s)"
        if (rate_limit_bytes_per_sec_ > 0 && peer->bucket_tokens == 0) {
            spdlog::debug("sync responder {}: byte budget exhausted, stopping silently",
                         conn->remote_address());
            peer->syncing = false;
            co_return;  // Initiator will timeout after 30s
        }

        auto msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
        if (!msg) {
            spdlog::warn("sync responder {}: timeout during Phase B", conn->remote_address());
            peer->syncing = false;
            co_return;
        }

        if (msg->type == wire::TransportMsgType_SyncComplete) break;

        if (msg->type == wire::TransportMsgType_ReconcileInit) {
            auto init = sync::decode_reconcile_init(msg->payload);
            if (!init) {
                spdlog::warn("sync responder {}: invalid ReconcileInit", conn->remote_address());
                continue;
            }

            std::array<uint8_t, 32> ns = init->namespace_id;

            bool cursor_hit = cursor_skip_namespaces.count(ns) > 0;
            if (cursor_hit) {
                ++metrics_.cursor_hits;
            } else {
                ++metrics_.cursor_misses;
            }

            // Snapshot our hashes for this namespace (sorted)
            auto& our_hashes = ns_hash_cache[ns];
            if (our_hashes.empty()) {
                auto raw = sync_proto_.collect_namespace_hashes(ns);
                our_hashes.assign(raw.begin(), raw.end());
                std::sort(our_hashes.begin(), our_hashes.end());
            }

            // Build initial full-range from the init message
            sync::Hash32 max_hash;
            max_hash.fill(0xFF);

            sync::RangeEntry init_range;
            init_range.upper_bound = max_hash;
            init_range.mode = sync::RangeMode::Fingerprint;
            init_range.count = init->count;
            init_range.fingerprint = init->fingerprint;

            auto result = sync::process_ranges(our_hashes, {init_range});

            // Collect peer items revealed in the init
            std::vector<sync::Hash32> peer_items;
            peer_items.insert(peer_items.end(),
                              result.have_items.begin(), result.have_items.end());

            if (result.complete) {
                // Fingerprint+count matched -- send empty ranges to signal done
                auto done_payload = sync::encode_reconcile_ranges(ns, {});
                co_await conn->send_message(wire::TransportMsgType_ReconcileRanges, done_payload);
            } else {
                // Send our response ranges
                auto resp_payload = sync::encode_reconcile_ranges(ns, result.response_ranges);
                co_await conn->send_message(wire::TransportMsgType_ReconcileRanges, resp_payload);

                // Multi-round loop within this namespace
                for (uint32_t round = 0; round < sync::MAX_RECONCILE_ROUNDS; ++round) {
                    auto rmsg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
                    if (!rmsg) {
                        spdlog::warn("sync responder {}: timeout during reconciliation for ns={}",
                                     conn->remote_address(), to_hex(ns, 8));
                        peer->syncing = false;
                        co_return;
                    }

                    if (rmsg->type == wire::TransportMsgType_ReconcileRanges) {
                        auto decoded = sync::decode_reconcile_ranges(rmsg->payload);
                        if (!decoded) {
                            spdlog::warn("sync responder {}: invalid ReconcileRanges",
                                         conn->remote_address());
                            break;
                        }

                        // Empty ranges = namespace done
                        if (decoded->ranges.empty()) break;

                        // Check if all ranges are resolved (no Fingerprint)
                        bool has_fingerprint = false;
                        for (const auto& r : decoded->ranges) {
                            if (r.mode == sync::RangeMode::Fingerprint) {
                                has_fingerprint = true;
                                break;
                            }
                        }

                        if (!has_fingerprint) {
                            // Final exchange: collect peer items, send ours, done
                            for (const auto& r : decoded->ranges) {
                                if (r.mode == sync::RangeMode::ItemList) {
                                    peer_items.insert(peer_items.end(),
                                                      r.items.begin(), r.items.end());
                                }
                            }
                            sync::Hash32 lower = sync::Hash32{};
                            std::vector<sync::Hash32> our_range_items;
                            for (const auto& r : decoded->ranges) {
                                if (r.mode == sync::RangeMode::ItemList) {
                                    auto [b, e] = sync::range_indices(our_hashes, lower,
                                                                      r.upper_bound);
                                    for (size_t idx = b; idx < e; ++idx) {
                                        our_range_items.push_back(our_hashes[idx]);
                                    }
                                }
                                lower = r.upper_bound;
                            }
                            auto items_payload = sync::encode_reconcile_items(ns,
                                                                              our_range_items);
                            co_await conn->send_message(wire::TransportMsgType_ReconcileItems,
                                                        items_payload);
                            break;
                        }

                        auto rresult = sync::process_ranges(our_hashes, decoded->ranges);
                        peer_items.insert(peer_items.end(),
                                          rresult.have_items.begin(), rresult.have_items.end());

                        auto rp = sync::encode_reconcile_ranges(ns, rresult.response_ranges);
                        co_await conn->send_message(wire::TransportMsgType_ReconcileRanges, rp);
                    } else if (rmsg->type == wire::TransportMsgType_ReconcileItems) {
                        // ReconcileItems = initiator's final item exchange for this namespace
                        auto decoded = sync::decode_reconcile_items(rmsg->payload);
                        if (decoded) {
                            peer_items.insert(peer_items.end(),
                                              decoded->items.begin(), decoded->items.end());
                        }
                        break;  // Namespace reconciliation done
                    } else {
                        break;
                    }
                }
            }

            // Compute diff: peer items we don't have (only request if not cursor-hit)
            if (!cursor_hit) {
                std::vector<std::array<uint8_t, 32>> missing;
                {
                    std::unordered_set<std::string> our_set;
                    our_set.reserve(our_hashes.size());
                    for (const auto& h : our_hashes) {
                        our_set.insert(std::string(reinterpret_cast<const char*>(h.data()), h.size()));
                    }
                    for (const auto& h : peer_items) {
                        std::string key(reinterpret_cast<const char*>(h.data()), h.size());
                        if (our_set.find(key) == our_set.end()) {
                            missing.push_back(h);
                        }
                    }
                }
                if (!missing.empty()) {
                    missing_per_ns[ns] = std::move(missing);
                }
            }
            total_stats.namespaces_synced++;
        }
    }

    // Phase C: Exchange blobs one at a time
    spdlog::debug("sync responder {}: Phase C start, missing {} namespaces",
                  conn->remote_address(), missing_per_ns.size());
    for (const auto& [ns, missing] : missing_per_ns) {
        spdlog::debug("sync responder {}: requesting {} blobs for ns {}",
                      conn->remote_address(), missing.size(), to_hex(ns, 4));
        for (size_t i = 0; i < missing.size(); i += MAX_HASHES_PER_REQUEST) {
            size_t batch_end = std::min(i + static_cast<size_t>(MAX_HASHES_PER_REQUEST),
                                        missing.size());
            std::vector<std::array<uint8_t, 32>> batch(
                missing.begin() + static_cast<ptrdiff_t>(i),
                missing.begin() + static_cast<ptrdiff_t>(batch_end));

            auto req_payload = sync::SyncProtocol::encode_blob_request(ns, batch);
            co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload);

            uint32_t expected = static_cast<uint32_t>(batch.size());
            uint32_t received = 0;
            while (received < expected) {
                auto msg = co_await recv_sync_msg(peer, BLOB_TRANSFER_TIMEOUT);
                if (!msg) {
                    spdlog::warn("sync responder: timeout waiting for blob transfer from {}",
                                 conn->remote_address());
                    break;
                }

                if (msg->type == wire::TransportMsgType_BlobTransfer) {
                    auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
                    auto s = co_await sync_proto_.ingest_blobs(blobs, conn);
                    total_stats.blobs_received += s.blobs_received;
                    total_stats.storage_full_count += s.storage_full_count;
                    total_stats.quota_exceeded_count += s.quota_exceeded_count;
                    received++;
                } else if (msg->type == wire::TransportMsgType_BlobRequest) {
                    if (peer->peer_is_full) {
                        spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                        continue;
                    }
                    auto [req_ns, requested_hashes] =
                        sync::SyncProtocol::decode_blob_request(msg->payload);
                    for (const auto& hash : requested_hashes) {
                        auto blob = engine_.get_blob(req_ns, hash);
                        if (blob.has_value()) {
                            auto bt_payload =
                                sync::SyncProtocol::encode_single_blob_transfer(*blob);
                            co_await conn->send_message(
                                wire::TransportMsgType_BlobTransfer, bt_payload);
                            total_stats.blobs_sent++;
                        }
                    }
                }
            }
        }
    }

    // Handle remaining BlobRequests from peer
    spdlog::debug("sync responder {}: entering remaining BlobRequests handler", conn->remote_address());
    while (true) {
        auto msg = co_await recv_sync_msg(peer, std::chrono::seconds(2));
        if (!msg) {
            spdlog::debug("sync responder {}: remaining handler timeout, done", conn->remote_address());
            break;
        }
        spdlog::debug("sync responder {}: remaining handler got type={}", conn->remote_address(), static_cast<int>(msg->type));
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            if (peer->peer_is_full) {
                spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                continue;
            }
            auto [req_ns, requested_hashes] =
                sync::SyncProtocol::decode_blob_request(msg->payload);
            spdlog::debug("sync responder {}: remaining handler serving {} hashes", conn->remote_address(), requested_hashes.size());
            for (const auto& hash : requested_hashes) {
                auto blob = engine_.get_blob(req_ns, hash);
                if (blob.has_value()) {
                    auto bt_payload =
                        sync::SyncProtocol::encode_single_blob_transfer(*blob);
                    co_await conn->send_message(
                        wire::TransportMsgType_BlobTransfer, bt_payload);
                    total_stats.blobs_sent++;
                }
            }
        } else {
            break;
        }
    }

    // Post-sync cursor update (responder side, same as initiator)
    for (const auto& pns : peer_namespaces) {
        auto old_cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        storage::SyncCursor updated;
        updated.seq_num = pns.latest_seq_num;
        updated.round_count = (old_cursor ? old_cursor->round_count : 0) + 1;
        updated.last_sync_timestamp = now_ts;
        storage_.set_sync_cursor(peer_hash, pns.namespace_id, updated);
    }

    spdlog::info("Sync responder {}: received {} blobs, sent {} blobs, {} namespaces "
                 "(cursor: {} hits, {} misses{})",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced,
                 cursor_hits_this_round, cursor_misses_this_round,
                 sync_is_full_resync ? ", full resync" : "");

    // Post-sync StorageFull signal: inform peer if we rejected blobs for capacity
    if (total_stats.storage_full_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty);
        spdlog::warn("Sent StorageFull to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.storage_full_count);
    }

    // Post-sync QuotaExceeded signal: inform peer if we rejected blobs for namespace quota
    if (total_stats.quota_exceeded_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty);
        spdlog::warn("Sent QuotaExceeded to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.quota_exceeded_count);
        metrics_.quota_rejections += total_stats.quota_exceeded_count;
    }

    // PEX exchange: wait for PeerListRequest from initiator, then respond (inline, no concurrent send)
    // Skip in closed mode -- don't share peer addresses
    if (!acl_.is_peer_closed_mode()) {
        auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
        if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListRequest) {
            auto addresses = build_peer_list_response(conn->remote_address());
            auto payload = encode_peer_list(addresses);
            spdlog::debug("PEX: sending {} peers to {}", addresses.size(), conn->remote_address());
            co_await conn->send_message(wire::TransportMsgType_PeerListResponse,
                                         std::span<const uint8_t>(payload));
        }
    }

    ++metrics_.syncs;
    peer->syncing = false;
}

asio::awaitable<void> PeerManager::sync_all_peers() {
    // Take a snapshot of connection pointers to avoid iterator invalidation
    // (peers_ may be modified during co_await when new peers connect/disconnect)
    std::vector<net::Connection::Ptr> connections;
    for (const auto& peer : peers_) {
        connections.push_back(peer.connection);
    }
    for (const auto& conn : connections) {
        auto* peer = find_peer(conn);
        if (peer && peer->connection->is_authenticated() && !peer->syncing) {
            co_await run_sync_with_peer(peer->connection);
        }
    }
}

asio::awaitable<void> PeerManager::sync_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        sync_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(config_.sync_interval_seconds));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        sync_timer_ = nullptr;
        if (ec || stopping_) co_return;

        co_await sync_all_peers();
    }
}

// =============================================================================
// Strike system
// =============================================================================

void PeerManager::record_strike(net::Connection::Ptr conn, const std::string& reason) {
    auto* peer = find_peer(conn);
    if (!peer) return;

    peer->strike_count++;
    spdlog::warn("strike {}/{} for peer {} ({})",
                 peer->strike_count, STRIKE_THRESHOLD,
                 conn->remote_address(), reason);

    if (peer->strike_count >= STRIKE_THRESHOLD) {
        spdlog::warn("disconnecting peer {}: {} validation failures",
                     conn->remote_address(), peer->strike_count);
        asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
    }
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

bool PeerManager::is_trusted_address(const asio::ip::address& addr) const {
    if (addr.is_loopback()) return true;
    return trusted_peers_.count(addr.to_string()) > 0;
}

void PeerManager::reload_config() {
    spdlog::info("reloading access control from {}...", config_path_.string());

    // Re-read config file
    config::Config new_cfg;
    try {
        new_cfg = config::load_config(config_path_);
    } catch (const std::exception& e) {
        spdlog::error("config reload failed (invalid JSON): {} (keeping current config)", e.what());
        return;
    }

    // Validate both key lists
    try {
        config::validate_allowed_keys(new_cfg.allowed_client_keys);
        config::validate_allowed_keys(new_cfg.allowed_peer_keys);
    } catch (const std::exception& e) {
        spdlog::error("config reload rejected (malformed key): {} (keeping current config)", e.what());
        return;
    }

    // Reload ACL
    auto result = acl_.reload(new_cfg.allowed_client_keys, new_cfg.allowed_peer_keys);

    // Log diff summary
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

    // Disconnect revoked peers (check both lists)
    if (result.client_removed > 0 || result.peer_removed > 0 ||
        acl_.is_client_closed_mode() || acl_.is_peer_closed_mode()) {
        disconnect_unauthorized_peers();
    }

    // Reload rate limit parameters
    rate_limit_bytes_per_sec_ = new_cfg.rate_limit_bytes_per_sec;
    rate_limit_burst_ = new_cfg.rate_limit_burst;
    if (rate_limit_bytes_per_sec_ > 0) {
        spdlog::info("config reload: rate_limit={}B/s burst={}B",
                     rate_limit_bytes_per_sec_, rate_limit_burst_);
    } else {
        spdlog::info("config reload: rate_limit=disabled");
    }

    // Reload sync rate limit parameters
    sync_cooldown_seconds_ = new_cfg.sync_cooldown_seconds;
    max_sync_sessions_ = new_cfg.max_sync_sessions;
    if (sync_cooldown_seconds_ > 0) {
        spdlog::info("config reload: sync_cooldown={}s max_sync_sessions={}",
                     sync_cooldown_seconds_, max_sync_sessions_);
    } else {
        spdlog::info("config reload: sync_cooldown=disabled max_sync_sessions={}",
                     max_sync_sessions_);
    }

    // Reload sync_namespaces
    try {
        config::validate_allowed_keys(new_cfg.sync_namespaces);
    } catch (const std::exception& e) {
        spdlog::error("config reload rejected (malformed sync_namespace): {} (keeping current)", e.what());
        return;
    }
    sync_namespaces_.clear();
    for (const auto& hex : new_cfg.sync_namespaces) {
        sync_namespaces_.insert(hex_to_namespace(hex));
    }
    if (sync_namespaces_.empty()) {
        spdlog::info("config reload: sync_namespaces=all (unrestricted)");
    } else {
        spdlog::info("config reload: sync_namespaces={} namespaces", sync_namespaces_.size());
    }

    // Reload cursor config and reset round counters (force full resync on next round)
    full_resync_interval_ = new_cfg.full_resync_interval;
    cursor_stale_seconds_ = new_cfg.cursor_stale_seconds;
    {
        auto reset_count = storage_.reset_all_round_counters();
        spdlog::warn("SIGHUP: reset {} cursor round counters (next sync will be full resync)",
                     reset_count);
    }

    // Reload max_storage_bytes
    engine_.set_max_storage_bytes(new_cfg.max_storage_bytes);
    if (new_cfg.max_storage_bytes > 0) {
        spdlog::info("config reload: max_storage_bytes={}",
                     new_cfg.max_storage_bytes);
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

    // Reload trusted_peers
    try {
        config::validate_trusted_peers(new_cfg.trusted_peers);
    } catch (const std::exception& e) {
        spdlog::error("config reload rejected (invalid trusted_peer): {} (keeping current)", e.what());
        return;
    }
    trusted_peers_.clear();
    for (const auto& ip_str : new_cfg.trusted_peers) {
        asio::error_code ec;
        auto addr = asio::ip::make_address(ip_str, ec);
        if (!ec) {
            trusted_peers_.insert(addr.to_string());
        }
    }
    spdlog::info("config reload: trusted_peers={} addresses", trusted_peers_.size());

    // Reload expiry scan interval
    expiry_scan_interval_seconds_ = new_cfg.expiry_scan_interval_seconds;
    spdlog::info("config reload: expiry_scan_interval={}s", expiry_scan_interval_seconds_);
    // Cancel current expiry timer to restart with new interval
    if (expiry_timer_) { expiry_timer_->cancel(); }

    // Reload compaction interval
    auto old_compaction = compaction_interval_hours_;
    compaction_interval_hours_ = new_cfg.compaction_interval_hours;
    if (compaction_interval_hours_ > 0) {
        spdlog::info("config reload: compaction_interval={}h", compaction_interval_hours_);
    } else {
        spdlog::info("config reload: compaction=disabled");
    }
    // Cancel current timer to restart with new interval (or stop if now disabled)
    if (compaction_timer_) { compaction_timer_->cancel(); }
    // If was disabled and now enabled, spawn the loop
    if (old_compaction == 0 && compaction_interval_hours_ > 0) {
        asio::co_spawn(ioc_, compaction_loop(), asio::detached);
    }
}

void PeerManager::disconnect_unauthorized_peers() {
    // Take snapshot -- closing connections triggers on_peer_disconnected which modifies peers_
    std::vector<net::Connection::Ptr> to_disconnect;

    for (const auto& peer : peers_) {
        auto peer_ns = crypto::sha3_256(peer.connection->peer_pubkey());
        bool peer_allowed = peer.connection->is_uds()
            ? acl_.is_client_allowed(std::span<const uint8_t, 32>(peer_ns))
            : acl_.is_peer_allowed(std::span<const uint8_t, 32>(peer_ns));
        if (!peer_allowed) {
            to_disconnect.push_back(peer.connection);
        }
    }

    for (const auto& conn : to_disconnect) {
        auto full_hex = to_hex(std::span<const uint8_t>(conn->peer_pubkey()), 32);
        spdlog::warn("revoking peer: namespace={} ip={}", full_hex, conn->remote_address());
        conn->close();  // Immediate close, no goodbye
    }

    if (!to_disconnect.empty()) {
        spdlog::info("config reload: disconnected {} peer(s)", to_disconnect.size());
    }
}

// =============================================================================
// Expiry scanning (cancellable member coroutine)
// =============================================================================

asio::awaitable<void> PeerManager::expiry_scan_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        expiry_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(expiry_scan_interval_seconds_));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        expiry_timer_ = nullptr;
        if (ec || stopping_) co_return;

        auto purged = storage_.run_expiry_scan();
        if (purged > 0) {
            spdlog::info("expiry scan: purged {} blobs", purged);
        }
    }
}

// =============================================================================
// Periodic peer list flush
// =============================================================================

asio::awaitable<void> PeerManager::peer_flush_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        flush_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        flush_timer_ = nullptr;
        if (ec || stopping_) co_return;
        save_persisted_peers();
    }
}

// =============================================================================
// PEX wire encoding
// =============================================================================

std::vector<uint8_t> PeerManager::encode_peer_list(const std::vector<std::string>& addresses) {
    std::vector<uint8_t> result;
    // [uint16_t count][for each: uint16_t addr_len, utf8 addr_bytes]
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
// Unified notification fan-out (Phase 79 PUSH-01/PUSH-07/PUSH-08)
// =============================================================================

void PeerManager::on_blob_ingested(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone,
    net::Connection::Ptr source) {
    // Test hook -- fire notification callback if set
    if (on_notification_) {
        on_notification_(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);
    }

    // Build notification payload once (77 bytes) -- reuse existing encode_notification
    auto payload = encode_notification(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);

    // BlobNotify (type 59) to all TCP peers except source (PUSH-01, PUSH-07, PUSH-08)
    for (auto& peer : peers_) {
        if (peer.connection == source) continue;  // Source exclusion (D-06, D-09)
        if (peer.connection->is_uds()) continue;  // UDS = client, not peer

        auto conn = peer.connection;
        auto payload_copy = payload;
        asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
            co_await conn->send_message(wire::TransportMsgType_BlobNotify,
                                         std::span<const uint8_t>(p));
        }, asio::detached);
    }

    // Notification (type 21) to subscribed clients -- existing pub/sub behavior
    for (auto& peer : peers_) {
        if (peer.subscribed_namespaces.count(namespace_id)) {
            auto conn = peer.connection;
            auto payload_copy = payload;
            asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
                co_await conn->send_message(wire::TransportMsgType_Notification,
                                             std::span<const uint8_t>(p));
            }, asio::detached);
        }
    }
}

// =============================================================================
// Phase 80: Targeted blob fetch
// =============================================================================

void PeerManager::on_blob_notify(net::Connection::Ptr conn, std::vector<uint8_t> payload) {
    // BlobNotify payload: [namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]
    if (payload.size() != 77) return;

    auto* peer = find_peer(conn);
    if (!peer) return;

    std::array<uint8_t, 32> ns{}, hash{};
    std::memcpy(ns.data(), payload.data(), 32);
    std::memcpy(hash.data(), payload.data() + 32, 32);

    // D-07: Suppress during active sync with this peer
    if (peer->syncing) {
        spdlog::debug("BlobNotify from {} suppressed: sync active", peer_display_name(conn));
        return;
    }

    // D-04: Already have this blob?
    if (storage_.has_blob(ns, hash)) return;

    // D-05: Already fetching this blob?
    if (pending_fetches_.count(hash)) return;
    pending_fetches_.emplace(hash, conn);

    // Send BlobFetch: [namespace_id:32][blob_hash:32] = 64 bytes
    // Using namespace_id+hash because storage_.get_blob() requires compound key
    // (corrected from D-01 hash-only design -- see RESEARCH.md)
    asio::co_spawn(ioc_, [this, conn, ns, hash]() -> asio::awaitable<void> {
        std::vector<uint8_t> fetch_payload(64);
        std::memcpy(fetch_payload.data(), ns.data(), 32);
        std::memcpy(fetch_payload.data() + 32, hash.data(), 32);
        co_await conn->send_message(wire::TransportMsgType_BlobFetch,
                                     std::span<const uint8_t>(fetch_payload));
    }, asio::detached);
}

void PeerManager::handle_blob_fetch(net::Connection::Ptr conn, std::vector<uint8_t> payload) {
    // BlobFetch payload: [namespace_id:32][blob_hash:32] = 64 bytes
    if (payload.size() != 64) return;

    // co_spawn on ioc_ for storage lookup + send (same pattern as ReadRequest)
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        std::array<uint8_t, 32> ns{}, hash{};
        std::memcpy(ns.data(), payload.data(), 32);
        std::memcpy(hash.data(), payload.data() + 32, 32);

        auto blob = storage_.get_blob(ns, hash);
        if (blob) {
            auto encoded = wire::encode_blob(*blob);
            std::vector<uint8_t> resp(1 + encoded.size());
            resp[0] = 0x00;  // D-03: found
            std::memcpy(resp.data() + 1, encoded.data(), encoded.size());
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse,
                                         std::span<const uint8_t>(resp));
        } else {
            std::vector<uint8_t> resp = {0x01};  // D-03: not-found
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse,
                                         std::span<const uint8_t>(resp));
        }
    }, asio::detached);
}

void PeerManager::handle_blob_fetch_response(net::Connection::Ptr conn, std::vector<uint8_t> payload) {
    if (payload.empty()) return;

    uint8_t status = payload[0];
    if (status == 0x01) {
        // D-08: Not-found -- debug log, pending set entry stays until disconnect (harmless)
        spdlog::debug("BlobFetchResponse not-found from {}", peer_display_name(conn));
        return;
    }
    if (status != 0x00 || payload.size() < 2) return;

    // co_spawn with engine offload (same pattern as Data handler, CONC-03)
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            auto blob = wire::decode_blob(
                std::span<const uint8_t>(payload.data() + 1, payload.size() - 1));

            auto result = co_await engine_.ingest(blob, conn);

            // CONC-03: Transfer back to IO thread before accessing PeerManager state
            co_await asio::post(ioc_, asio::use_awaitable);

            // Clean pending set using ack blob_hash (Pitfall 5 from RESEARCH.md)
            if (result.accepted && result.ack.has_value()) {
                pending_fetches_.erase(result.ack->blob_hash);

                if (result.ack->status == engine::IngestStatus::stored) {
                    // Fan-out notification to other peers (source=conn excludes sender)
                    on_blob_ingested(
                        blob.namespace_id,
                        result.ack->blob_hash,
                        result.ack->seq_num,
                        static_cast<uint32_t>(blob.data.size()),
                        wire::is_tombstone(blob.data),
                        conn);  // Source exclusion: don't notify the peer we fetched from
                    ++metrics_.ingests;
                } else {
                    // Duplicate -- already had it (race with concurrent sync)
                    ++metrics_.ingests;
                }
            } else if (!result.accepted && result.error.has_value()) {
                // D-10: Failed ingestion -- log warning, no disconnect, no strike
                spdlog::warn("BlobFetchResponse ingest rejected from {}: {}",
                             peer_display_name(conn), result.error_detail);
            }
        } catch (const std::exception& e) {
            // D-10: Malformed response -- log warning, no disconnect, no strike
            spdlog::warn("malformed BlobFetchResponse from {}: {}",
                         conn->remote_address(), e.what());
        }
    }, asio::detached);
}

// =============================================================================
// Pub/Sub wire encoding
// =============================================================================

std::vector<uint8_t> PeerManager::encode_namespace_list(
    const std::vector<std::array<uint8_t, 32>>& namespaces) {
    std::vector<uint8_t> result(2 + namespaces.size() * 32);
    auto count = static_cast<uint16_t>(namespaces.size());
    result[0] = static_cast<uint8_t>(count >> 8);
    result[1] = static_cast<uint8_t>(count & 0xFF);
    size_t offset = 2;
    for (const auto& ns : namespaces) {
        std::memcpy(result.data() + offset, ns.data(), 32);
        offset += 32;
    }
    return result;
}

std::vector<std::array<uint8_t, 32>> PeerManager::decode_namespace_list(
    std::span<const uint8_t> payload) {
    std::vector<std::array<uint8_t, 32>> result;
    if (payload.size() < 2) return result;
    uint16_t count = (static_cast<uint16_t>(payload[0]) << 8) |
                      static_cast<uint16_t>(payload[1]);
    if (payload.size() != 2 + static_cast<size_t>(count) * 32) return result;
    for (uint16_t i = 0; i < count; ++i) {
        std::array<uint8_t, 32> ns{};
        std::memcpy(ns.data(), payload.data() + 2 + i * 32, 32);
        result.push_back(ns);
    }
    return result;
}

std::vector<uint8_t> PeerManager::encode_notification(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t, 32> blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone) {
    std::vector<uint8_t> result(77);
    std::memcpy(result.data(), namespace_id.data(), 32);
    std::memcpy(result.data() + 32, blob_hash.data(), 32);
    // seq_num big-endian at offset 64
    for (int i = 7; i >= 0; --i) {
        result[64 + (7 - i)] = static_cast<uint8_t>(seq_num >> (i * 8));
    }
    // blob_size big-endian at offset 72
    result[72] = static_cast<uint8_t>(blob_size >> 24);
    result[73] = static_cast<uint8_t>(blob_size >> 16);
    result[74] = static_cast<uint8_t>(blob_size >> 8);
    result[75] = static_cast<uint8_t>(blob_size & 0xFF);
    // is_tombstone at offset 76
    result[76] = is_tombstone ? 1 : 0;
    return result;
}

// =============================================================================
// PEX protocol
// =============================================================================

asio::awaitable<void> PeerManager::run_pex_with_peer(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;
    peer->syncing = true;
    peer->sync_inbox.clear();

    // Send PeerListRequest
    std::span<const uint8_t> empty{};
    if (!co_await conn->send_message(wire::TransportMsgType_PeerListRequest, empty)) {
        spdlog::debug("PEX: failed to send PeerListRequest to {}", conn->remote_address());
        peer->syncing = false;
        co_return;
    }

    // Wait for PeerListResponse
    auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
    if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListResponse) {
        handle_peer_list_response(conn, std::move(pex_msg->payload));
    }

    peer->syncing = false;
}

std::vector<std::string> PeerManager::build_peer_list_response(const std::string& exclude_address) {
    std::vector<std::string> candidates;

    for (const auto& peer : peers_) {
        if (!peer.connection->is_authenticated()) continue;
        if (peer.address == exclude_address) continue;
        if (peer.address == config_.bind_address) continue;
        candidates.push_back(peer.address);
    }

    // Shuffle for random subset
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(candidates.begin(), candidates.end(), gen);

    // Truncate to MAX_PEERS_PER_EXCHANGE
    if (candidates.size() > MAX_PEERS_PER_EXCHANGE) {
        candidates.resize(MAX_PEERS_PER_EXCHANGE);
    }

    return candidates;
}

asio::awaitable<void> PeerManager::handle_pex_as_responder(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;

    // In closed mode, don't respond to PEX requests (no address sharing)
    if (acl_.is_peer_closed_mode()) co_return;

    peer->syncing = true;
    // NOTE: do NOT clear sync_inbox -- PeerListRequest is already queued in it

    // Read PeerListRequest from inbox (already pushed by on_peer_message)
    auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
    if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListRequest) {
        auto addresses = build_peer_list_response(conn->remote_address());
        auto payload = encode_peer_list(addresses);
        spdlog::debug("PEX: sending {} peers to {}", addresses.size(), conn->remote_address());
        co_await conn->send_message(wire::TransportMsgType_PeerListResponse,
                                     std::span<const uint8_t>(payload));
    }

    peer->syncing = false;
}

void PeerManager::handle_peer_list_response(net::Connection::Ptr conn,
                                             std::vector<uint8_t> payload) {
    auto addresses = decode_peer_list(payload);
    spdlog::debug("PEX: received {} peer addresses from {}",
                  addresses.size(), conn->remote_address());

    uint32_t connected = 0;
    for (const auto& addr : addresses) {
        // Skip if we already know about this address
        if (known_addresses_.count(addr)) continue;
        // Skip if it's our own address
        if (addr == config_.bind_address) continue;
        // Skip if we're at max peers
        if (peers_.size() >= config_.max_peers) break;
        // Skip if we've connected enough this round
        if (connected >= MAX_DISCOVERED_PER_ROUND) break;

        known_addresses_.insert(addr);
        server_.connect_once(addr);
        connected++;
    }

    if (connected > 0) {
        spdlog::info("PEX: connecting to {} new peers discovered from {}",
                     connected, conn->remote_address());
    }
}

asio::awaitable<void> PeerManager::pex_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        pex_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(PEX_INTERVAL_SEC));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        pex_timer_ = nullptr;
        if (ec || stopping_) co_return;

        // Skip PEX entirely in closed mode -- don't advertise or discover peers
        if (acl_.is_peer_closed_mode()) continue;

        co_await request_peers_from_all();
    }
}

asio::awaitable<void> PeerManager::request_peers_from_all() {
    // Take a snapshot of connection pointers to avoid iterator invalidation
    // (peers_ may be modified during co_await when new peers connect/disconnect)
    std::vector<net::Connection::Ptr> connections;
    for (const auto& peer : peers_) {
        connections.push_back(peer.connection);
    }
    for (const auto& conn : connections) {
        auto* peer = find_peer(conn);
        if (peer && peer->connection->is_authenticated() && !peer->syncing) {
            co_await run_pex_with_peer(peer->connection);
        }
    }
}

// =============================================================================
// Peer persistence
// =============================================================================

std::filesystem::path PeerManager::peers_file_path() const {
    return std::filesystem::path(config_.data_dir) / "peers.json";
}

void PeerManager::load_persisted_peers() {
    auto path = peers_file_path();
    if (!std::filesystem::exists(path)) return;

    try {
        std::ifstream f(path);
        auto j = nlohmann::json::parse(f);

        persisted_peers_.clear();
        for (const auto& entry : j.value("peers", nlohmann::json::array())) {
            PersistedPeer p;
            p.address = entry.value("address", "");
            p.last_seen = entry.value("last_seen", uint64_t(0));
            p.fail_count = entry.value("fail_count", uint32_t(0));
            p.pubkey_hash = entry.value("pubkey_hash", std::string{});
            if (!p.address.empty() && p.fail_count < MAX_PERSIST_FAILURES) {
                persisted_peers_.push_back(std::move(p));
            }
        }

        // Increment fail_count for all loaded peers (reset on successful connect)
        for (auto& p : persisted_peers_) {
            p.fail_count++;
        }

        spdlog::info("loaded {} persisted peers from {}", persisted_peers_.size(), path.string());
        save_persisted_peers();  // Persist incremented fail_counts
    } catch (const std::exception& e) {
        spdlog::warn("failed to load persisted peers: {}", e.what());
        persisted_peers_.clear();
    }
}

void PeerManager::save_persisted_peers() {
    // Prune peers with too many failures
    persisted_peers_.erase(
        std::remove_if(persisted_peers_.begin(), persisted_peers_.end(),
                       [](const PersistedPeer& p) { return p.fail_count >= MAX_PERSIST_FAILURES; }),
        persisted_peers_.end());

    // Cap at MAX_PERSISTED_PEERS (keep most recently seen)
    if (persisted_peers_.size() > MAX_PERSISTED_PEERS) {
        std::sort(persisted_peers_.begin(), persisted_peers_.end(),
                  [](const PersistedPeer& a, const PersistedPeer& b) {
                      return a.last_seen > b.last_seen;
                  });
        persisted_peers_.resize(MAX_PERSISTED_PEERS);
    }

    nlohmann::json j;
    j["peers"] = nlohmann::json::array();
    for (const auto& p : persisted_peers_) {
        nlohmann::json entry = {
            {"address", p.address},
            {"last_seen", p.last_seen},
            {"fail_count", p.fail_count}
        };
        if (!p.pubkey_hash.empty()) {
            entry["pubkey_hash"] = p.pubkey_hash;
        }
        j["peers"].push_back(std::move(entry));
    }

    auto path = peers_file_path();
    auto tmp_path = std::filesystem::path(path.string() + ".tmp");

    try {
        std::filesystem::create_directories(path.parent_path());
        auto json_str = j.dump(2);

        // Write to temp file via raw POSIX (std::ofstream cannot fsync)
        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            spdlog::warn("failed to create temp peer file: {}", strerror(errno));
            return;
        }

        auto written = ::write(fd, json_str.data(), json_str.size());
        if (written < 0 || static_cast<size_t>(written) != json_str.size()) {
            ::close(fd);
            std::filesystem::remove(tmp_path);
            spdlog::warn("failed to write temp peer file");
            return;
        }

        if (::fsync(fd) != 0) {
            ::close(fd);
            std::filesystem::remove(tmp_path);
            spdlog::warn("failed to fsync temp peer file");
            return;
        }
        ::close(fd);

        // Atomic rename
        std::filesystem::rename(tmp_path, path);

        // Directory fsync (required on Linux for rename durability)
        int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }

        spdlog::debug("saved {} persisted peers", persisted_peers_.size());
    } catch (const std::exception& e) {
        spdlog::warn("failed to save persisted peers: {}", e.what());
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }
}

void PeerManager::update_persisted_peer(const std::string& address, bool success) {
    // Don't persist bootstrap peers (they have their own reconnect logic)
    if (bootstrap_addresses_.count(address)) return;

    auto it = std::find_if(persisted_peers_.begin(), persisted_peers_.end(),
                           [&address](const PersistedPeer& p) { return p.address == address; });

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    if (it != persisted_peers_.end()) {
        if (success) {
            it->last_seen = now;
            it->fail_count = 0;
        } else {
            it->fail_count++;
        }
    } else if (success) {
        persisted_peers_.push_back({address, now, 0, {}});
    }

    save_persisted_peers();
}

// =============================================================================
// Helpers
// =============================================================================

PeerInfo* PeerManager::find_peer(const net::Connection::Ptr& conn) {
    for (auto& p : peers_) {
        if (p.connection == conn) return &p;
    }
    return nullptr;
}

std::string PeerManager::peer_display_name(const net::Connection::Ptr& conn) {
    auto ns_hex = to_hex(conn->peer_pubkey(), 8);
    return ns_hex + "@" + conn->remote_address();
}

// =============================================================================
// Sync rate limiting helpers (Phase 40)
// =============================================================================

void PeerManager::send_sync_rejected(net::Connection::Ptr conn, uint8_t reason) {
    std::vector<uint8_t> payload = { reason };
    asio::co_spawn(ioc_, [conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        co_await conn->send_message(wire::TransportMsgType_SyncRejected,
                                     std::span<const uint8_t>(payload));
    }, asio::detached);
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
        dump_metrics();
    }
}

void PeerManager::dump_metrics() {
    spdlog::info("=== METRICS DUMP (SIGUSR1) ===");

    // Global counters (same data as periodic line)
    log_metrics_line();

    // Per-peer breakdown
    spdlog::info("  peers: {}", peers_.size());
    for (const auto& peer : peers_) {
        auto ns_hex = to_hex(peer.connection->peer_pubkey(), 4);
        spdlog::info("    {} (ns:{}...)", peer.address, ns_hex);
    }

    // UDS connection count
    if (uds_acceptor_) {
        spdlog::info("  uds_connections: {}", uds_acceptor_->connection_count());
    }

    // Quota metrics
    spdlog::info("  quota_rejections: {}", metrics_.quota_rejections);
    spdlog::info("  sync_rejections: {}", metrics_.sync_rejections);

    // Per-namespace stats via list_namespaces()
    auto namespaces = storage_.list_namespaces();
    spdlog::info("  namespaces: {}", namespaces.size());
    for (const auto& ns : namespaces) {
        auto ns_hex = to_hex(
            std::span<const uint8_t>(ns.namespace_id.data(), ns.namespace_id.size()), 4);
        spdlog::info("    ns:{:>8}... latest_seq={}", ns_hex, ns.latest_seq_num);
    }

    // Compaction stats
    spdlog::info("  last_compaction_time: {}", last_compaction_time_);
    spdlog::info("  compaction_count: {}", compaction_count_);

    spdlog::info("=== END METRICS DUMP ===");
}

// =============================================================================
// Periodic metrics
// =============================================================================

asio::awaitable<void> PeerManager::metrics_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        metrics_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        metrics_timer_ = nullptr;
        if (ec || stopping_) co_return;
        log_metrics_line();
    }
}

void PeerManager::log_metrics_line() {
    auto storage_bytes = storage_.used_data_bytes();
    auto storage_mib = static_cast<double>(storage_bytes) / (1024.0 * 1024.0);
    auto uptime = compute_uptime_seconds();

    // Sum latest_seq_num across namespaces as blob count proxy
    // (O(N namespaces) not O(N blobs), acceptable for periodic logging)
    uint64_t blob_count = 0;
    auto namespaces = storage_.list_namespaces();
    for (const auto& ns : namespaces) {
        blob_count += ns.latest_seq_num;
    }

    spdlog::info("metrics: peers={} connected_total={} disconnected_total={} "
                 "blobs={} storage={:.1f}MiB "
                 "syncs={} ingests={} rejections={} rate_limited={} "
                 "cursor_hits={} cursor_misses={} full_resyncs={} "
                 "quota_rejections={} sync_rejections={} uptime={}",
                 peers_.size(),
                 metrics_.peers_connected_total,
                 metrics_.peers_disconnected_total,
                 blob_count,
                 storage_mib,
                 metrics_.syncs,
                 metrics_.ingests,
                 metrics_.rejections,
                 metrics_.rate_limited,
                 metrics_.cursor_hits,
                 metrics_.cursor_misses,
                 metrics_.full_resyncs,
                 metrics_.quota_rejections,
                 metrics_.sync_rejections,
                 uptime);
}

uint64_t PeerManager::compute_uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
}

// =============================================================================
// Cursor compaction (prune cursors for disconnected peers)
// =============================================================================

asio::awaitable<void> PeerManager::cursor_compaction_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        cursor_compaction_timer_ = &timer;
        timer.expires_after(std::chrono::hours(6));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        cursor_compaction_timer_ = nullptr;
        if (ec || stopping_) co_return;

        // Build set of currently-connected peer pubkey hashes
        std::vector<std::array<uint8_t, 32>> connected;
        for (const auto& peer : peers_) {
            if (peer.connection && !peer.connection->peer_pubkey().empty()) {
                auto hash = crypto::sha3_256(peer.connection->peer_pubkey());
                connected.push_back(hash);
            }
        }
        auto removed = storage_.cleanup_stale_cursors(connected);
        if (removed > 0) {
            spdlog::info("cursor compaction: removed {} entries for disconnected peers", removed);
        }
    }
}

// =============================================================================
// Storage compaction (periodic mdbx file compaction)
// =============================================================================

asio::awaitable<void> PeerManager::compaction_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        compaction_timer_ = &timer;
        timer.expires_after(std::chrono::hours(compaction_interval_hours_));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        compaction_timer_ = nullptr;
        if (ec || stopping_) co_return;

        // If interval was set to 0 (disabled) via SIGHUP, exit the loop
        if (compaction_interval_hours_ == 0) co_return;

        auto result = storage_.compact();
        if (result.success) {
            auto before_mib = static_cast<double>(result.before_bytes) / (1024.0 * 1024.0);
            auto after_mib = static_cast<double>(result.after_bytes) / (1024.0 * 1024.0);
            double reduction = (result.before_bytes > 0)
                ? (1.0 - static_cast<double>(result.after_bytes) /
                         static_cast<double>(result.before_bytes)) * 100.0
                : 0.0;
            spdlog::info("compaction complete: {:.1f}MiB -> {:.1f}MiB ({:.1f}%) in {}ms",
                         before_mib, after_mib, reduction, result.duration_ms);
            last_compaction_time_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            ++compaction_count_;
        }
        // On failure, Storage::compact() already logged the error
    }
}

asio::awaitable<void> PeerManager::inactivity_check_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        inactivity_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        inactivity_timer_ = nullptr;
        if (ec || stopping_) co_return;

        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        uint64_t timeout_ms = static_cast<uint64_t>(config_.inactivity_timeout_seconds) * 1000;

        std::vector<net::Connection::Ptr> to_close;
        for (const auto& peer : peers_) {
            if (peer.last_message_time > 0 &&
                (now_ms - peer.last_message_time) > timeout_ms) {
                spdlog::warn("inactivity timeout: disconnecting {} ({}s idle)",
                             peer.address,
                             (now_ms - peer.last_message_time) / 1000);
                to_close.push_back(peer.connection);
            }
        }
        for (auto& conn : to_close) {
            conn->close();
        }
    }
}

} // namespace chromatindb::peer
