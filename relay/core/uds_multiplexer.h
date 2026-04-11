#pragma once

#include "relay/core/request_router.h"
#include "relay/identity/relay_identity.h"
#include "relay/ws/session_manager.h"

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::relay::core {

class SubscriptionTracker;  // Forward declaration (Phase 104)
struct RelayMetrics;  // Forward declaration (metrics_collector.h)

/// Single multiplexed UDS connection to the local chromatindb node.
/// Performs TrustedHello + HKDF + AEAD handshake, encrypts all post-handshake
/// messages, and routes node responses to the correct WebSocket client.
class UdsMultiplexer {
public:
    UdsMultiplexer(asio::io_context& ioc,
                   std::string uds_path,
                   const identity::RelayIdentity& identity,
                   RequestRouter& router,
                   ws::SessionManager& sessions);

    /// Start async connect loop. Non-blocking, spawns coroutine.
    void start();

    /// Send an already-encoded TransportMessage to the node over encrypted UDS.
    /// Returns false if not connected.
    bool send(std::vector<uint8_t> transport_msg);

    /// Whether UDS is connected and handshake complete.
    bool is_connected() const;

    /// Set subscription tracker for notification fan-out (Phase 104).
    void set_tracker(SubscriptionTracker* t);

    /// Set pointer to relay_main's SIGHUP-reloadable request timeout atomic.
    void set_request_timeout(const std::atomic<uint32_t>* timeout);

    /// Set pointer to relay-level metrics for counter increments.
    void set_metrics(RelayMetrics* metrics);

private:
    /// Retry connect with jittered backoff (1s base, 30s cap). Per D-04.
    asio::awaitable<void> connect_loop();

    /// Perform TrustedHello + HKDF + auth exchange.
    asio::awaitable<bool> do_handshake();

    /// Post-handshake read loop: recv_encrypted -> decode -> route response.
    asio::awaitable<void> read_loop();

    /// Periodic stale request cleanup (60s sweep). Per D-11.
    asio::awaitable<void> cleanup_loop();

    /// Send raw frame: [4B BE length][data]. No encryption (for handshake).
    asio::awaitable<bool> send_raw(std::span<const uint8_t> data);

    /// Recv raw frame: read [4B BE length], then read [data]. No encryption.
    asio::awaitable<std::optional<std::vector<uint8_t>>> recv_raw();

    /// Send encrypted frame: AEAD encrypt, then send_raw.
    asio::awaitable<bool> send_encrypted(std::span<const uint8_t> plaintext);

    /// Recv encrypted frame: recv_raw, then AEAD decrypt.
    asio::awaitable<std::optional<std::vector<uint8_t>>> recv_encrypted();

    /// Drain the send queue -- serializes outbound writes over UDS.
    asio::awaitable<void> drain_send_queue();

    /// Route a decoded response from the node to the correct WsSession.
    void route_response(uint8_t type, std::vector<uint8_t> payload, uint32_t request_id);

    /// Handle Notification (type 21) fan-out to subscribed sessions (Phase 104 D-06).
    void handle_notification(uint8_t type, std::span<const uint8_t> payload);

    /// Replay all active subscriptions as a batched Subscribe to the node (D-10).
    void replay_subscriptions();

    /// Bulk-fail all pending requests, sending error JSON to each client (D-13).
    void bulk_fail_pending_requests();

    /// Send timeout ErrorResponse JSON to client for a stale pending request.
    void send_timeout_error(const PendingRequest& pending);

    asio::io_context& ioc_;
    std::string uds_path_;
    const identity::RelayIdentity& identity_;
    RequestRouter& router_;
    ws::SessionManager& sessions_;

    asio::local::stream_protocol::socket socket_;
    bool connected_ = false;

    // AEAD state
    std::vector<uint8_t> send_key_;   // 32 bytes
    std::vector<uint8_t> recv_key_;   // 32 bytes
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;

    // Send serialization (only one send at a time on UDS)
    std::deque<std::vector<uint8_t>> send_queue_;
    bool draining_ = false;

    // Subscription tracking (Phase 104)
    SubscriptionTracker* tracker_ = nullptr;

    // Request timeout (SIGHUP-reloadable via relay_main atomic)
    const std::atomic<uint32_t>* request_timeout_ = nullptr;

    // Relay-level metrics for counter increments
    RelayMetrics* metrics_ = nullptr;
};

} // namespace chromatindb::relay::core
