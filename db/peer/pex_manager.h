#pragma once

#include "db/peer/peer_types.h"

#include <asio.hpp>

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace chromatindb::net { class Server; }
namespace chromatindb::acl { class AccessControl; }

namespace chromatindb::peer {

/// Owns peer exchange protocol (PEX), peer persistence, and known address
/// tracking.  Extracted from PeerManager (Phase 96 ARCH-01, component D-04).
class PexManager {
public:
    /// PEX constants.
    static constexpr uint32_t PEX_INTERVAL_SEC = 300;        // 5 minutes
    static constexpr uint32_t MAX_PEERS_PER_EXCHANGE = 8;    // Max peers to share per response
    static constexpr uint32_t MAX_DISCOVERED_PER_ROUND = 3;  // Max new peers to connect per round
    static constexpr uint32_t MAX_PERSISTED_PEERS = 100;     // Max entries in peers.json
    static constexpr uint32_t MAX_PERSIST_FAILURES = 3;      // Prune after N consecutive startup failures

    /// Callback types for cross-component operations.
    using EncodeCallback = std::function<std::vector<uint8_t>(const std::vector<std::string>&)>;
    using DecodeCallback = std::function<std::vector<std::string>(std::span<const uint8_t>)>;

    PexManager(asio::io_context& ioc,
               const bool& stopping,
               std::deque<std::unique_ptr<PeerInfo>>& peers,
               net::Server& server,
               acl::AccessControl& acl,
               const std::string& bind_address,
               const std::string& data_dir,
               uint32_t max_peers,
               const std::set<std::string>& bootstrap_addresses,
               EncodeCallback encode_peer_list,
               DecodeCallback decode_peer_list);

    // Timer loops (co_spawn from PeerManager::start)
    asio::awaitable<void> pex_timer_loop();
    asio::awaitable<void> peer_flush_timer_loop();

    // PEX protocol handlers (called from on_peer_message)
    asio::awaitable<void> run_pex_with_peer(net::Connection::Ptr conn);
    asio::awaitable<void> handle_pex_as_responder(net::Connection::Ptr conn);
    void handle_peer_list_response(net::Connection::Ptr conn, std::vector<uint8_t> payload);

    // Peer persistence
    void load_persisted_peers();
    void save_persisted_peers();
    void update_persisted_peer(const std::string& address, bool success);

    // Peer access
    std::set<std::string>& known_addresses() { return known_addresses_; }
    const std::set<std::string>& known_addresses() const { return known_addresses_; }
    const std::vector<PersistedPeer>& persisted_peers() const { return persisted_peers_; }
    std::vector<PersistedPeer>& persisted_peers() { return persisted_peers_; }

    void cancel_timers();
    void set_max_peers(uint32_t max_peers) { max_peers_ = max_peers; }

    // Public for inline PEX after sync (used by SyncOrchestrator / PeerManager)
    std::vector<std::string> build_peer_list_response(const std::string& exclude_address);

private:
    asio::awaitable<void> request_peers_from_all();
    std::filesystem::path peers_file_path() const;
    PeerInfo* find_peer(const net::Connection::Ptr& conn);
    asio::awaitable<std::optional<SyncMessage>> recv_sync_msg(
        const net::Connection::Ptr& conn, std::chrono::seconds timeout);

    asio::io_context& ioc_;
    const bool& stopping_;
    std::deque<std::unique_ptr<PeerInfo>>& peers_;
    net::Server& server_;
    acl::AccessControl& acl_;
    std::string bind_address_;
    std::string data_dir_;

    std::set<std::string> known_addresses_;
    std::vector<PersistedPeer> persisted_peers_;
    const std::set<std::string>& bootstrap_addresses_;
    asio::steady_timer* pex_timer_ = nullptr;
    asio::steady_timer* flush_timer_ = nullptr;
    uint32_t max_peers_;

    EncodeCallback encode_peer_list_;
    DecodeCallback decode_peer_list_;
};

} // namespace chromatindb::peer
