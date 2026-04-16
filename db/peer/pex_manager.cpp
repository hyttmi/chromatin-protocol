#include "db/peer/pex_manager.h"
#include "db/acl/access_control.h"
#include "db/logging/logging.h"
#include "db/net/server.h"
#include "db/util/hex.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <random>

namespace chromatindb::peer {

using chromatindb::util::to_hex;

PexManager::PexManager(asio::io_context& ioc,
                       const bool& stopping,
                       std::deque<std::unique_ptr<PeerInfo>>& peers,
                       net::Server& server,
                       acl::AccessControl& acl,
                       const std::string& bind_address,
                       const std::string& data_dir,
                       uint32_t max_peers,
                       uint32_t pex_interval,
                       const std::set<std::string>& bootstrap_addresses,
                       EncodeCallback encode_peer_list,
                       DecodeCallback decode_peer_list)
    : ioc_(ioc)
    , stopping_(stopping)
    , peers_(peers)
    , server_(server)
    , acl_(acl)
    , bind_address_(bind_address)
    , data_dir_(data_dir)
    , bootstrap_addresses_(bootstrap_addresses)
    , max_peers_(max_peers)
    , pex_interval_sec_(pex_interval)
    , encode_peer_list_(std::move(encode_peer_list))
    , decode_peer_list_(std::move(decode_peer_list)) {
}

// =============================================================================
// PEX protocol
// =============================================================================

asio::awaitable<void> PexManager::run_pex_with_peer(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;
    peer->syncing = true;
    peer->sync_inbox.clear();

    // Send PeerListRequest
    std::span<const uint8_t> empty{};
    if (!co_await conn->send_message(wire::TransportMsgType_PeerListRequest, empty)) {
        spdlog::debug("PEX: failed to send PeerListRequest to {}", conn->remote_address());
        peer = find_peer(conn);
        if (peer) peer->syncing = false;
        co_return;
    }

    // Wait for PeerListResponse
    auto pex_msg = co_await recv_sync_msg(conn, std::chrono::seconds(5));
    if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListResponse) {
        handle_peer_list_response(conn, std::move(pex_msg->payload));
    }

    peer = find_peer(conn);
    if (peer) peer->syncing = false;
}

std::vector<std::string> PexManager::build_peer_list_response(const std::string& exclude_address) {
    std::vector<std::string> candidates;

    for (const auto& peer : peers_) {
        if (!peer->connection->is_authenticated()) continue;
        if (peer->address == exclude_address) continue;
        if (peer->address == bind_address_) continue;
        candidates.push_back(peer->address);
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

asio::awaitable<void> PexManager::handle_pex_as_responder(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;

    // In closed mode, don't respond to PEX requests (no address sharing)
    if (acl_.is_peer_closed_mode()) co_return;

    peer->syncing = true;
    // NOTE: do NOT clear sync_inbox -- PeerListRequest is already queued in it

    // Read PeerListRequest from inbox (already pushed by on_peer_message)
    auto pex_msg = co_await recv_sync_msg(conn, std::chrono::seconds(5));
    peer = find_peer(conn);
    if (!peer) co_return;
    if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListRequest) {
        auto addresses = build_peer_list_response(conn->remote_address());
        auto payload = encode_peer_list_(addresses);
        spdlog::debug("PEX: sending {} peers to {}", addresses.size(), conn->remote_address());
        co_await conn->send_message(wire::TransportMsgType_PeerListResponse,
                                     std::span<const uint8_t>(payload));
    }

    peer = find_peer(conn);
    if (peer) peer->syncing = false;
}

void PexManager::handle_peer_list_response(net::Connection::Ptr conn,
                                            std::vector<uint8_t> payload) {
    auto addresses = decode_peer_list_(payload);
    spdlog::debug("PEX: received {} peer addresses from {}",
                  addresses.size(), conn->remote_address());

    uint32_t connected = 0;
    for (const auto& addr : addresses) {
        // Skip if we already know about this address
        if (known_addresses_.count(addr)) continue;
        // Skip if it's our own address
        if (addr == bind_address_) continue;
        // Skip if we're at max peers
        if (peers_.size() >= max_peers_) break;
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

asio::awaitable<void> PexManager::pex_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        pex_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(pex_interval_sec_));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        pex_timer_ = nullptr;
        if (ec || stopping_) co_return;

        // Skip PEX entirely in closed mode -- don't advertise or discover peers
        if (acl_.is_peer_closed_mode()) continue;

        co_await request_peers_from_all();
    }
}

asio::awaitable<void> PexManager::request_peers_from_all() {
    // Take a snapshot of connection pointers to avoid iterator invalidation
    // (peers_ may be modified during co_await when new peers connect/disconnect)
    std::vector<net::Connection::Ptr> connections;
    for (const auto& peer : peers_) {
        connections.push_back(peer->connection);
    }
    for (const auto& conn : connections) {
        auto* peer = find_peer(conn);
        if (peer && peer->connection->is_authenticated() && !peer->syncing) {
            co_await run_pex_with_peer(peer->connection);
        }
    }
}

// =============================================================================
// Periodic peer list flush
// =============================================================================

asio::awaitable<void> PexManager::peer_flush_timer_loop() {
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
// Peer persistence
// =============================================================================

std::filesystem::path PexManager::peers_file_path() const {
    return std::filesystem::path(data_dir_) / "peers.json";
}

void PexManager::load_persisted_peers() {
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

void PexManager::save_persisted_peers() {
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
        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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

void PexManager::update_persisted_peer(const std::string& address, bool success) {
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

PeerInfo* PexManager::find_peer(const net::Connection::Ptr& conn) {
    for (auto& p : peers_) {
        if (p->connection == conn) return p.get();
    }
    return nullptr;
}

asio::awaitable<std::optional<SyncMessage>>
PexManager::recv_sync_msg(const net::Connection::Ptr& conn, std::chrono::seconds timeout) {
    // Ensure we're on the io_context thread before accessing sync_inbox.
    co_await asio::post(ioc_, asio::use_awaitable);

    auto* peer = find_peer(conn);
    if (!peer) co_return std::nullopt;

    if (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        co_return msg;
    }
    asio::steady_timer timer(ioc_);
    peer->sync_notify = &timer;
    timer.expires_after(timeout);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));

    // Re-lookup after co_await -- peer may have disconnected during wait
    peer = find_peer(conn);
    if (!peer) co_return std::nullopt;

    peer->sync_notify = nullptr;
    if (peer->sync_inbox.empty()) {
        co_return std::nullopt;  // Timeout
    }
    auto msg = std::move(peer->sync_inbox.front());
    peer->sync_inbox.pop_front();
    co_return msg;
}

void PexManager::cancel_timers() {
    if (pex_timer_) pex_timer_->cancel();
    if (flush_timer_) flush_timer_->cancel();
}

} // namespace chromatindb::peer
