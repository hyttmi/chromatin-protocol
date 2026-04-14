#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chromatindb::relay::core {

/// 32-byte namespace identifier (SHA3-256 output).
using Namespace32 = std::array<uint8_t, 32>;

/// Hash for Namespace32. Uses first 8 bytes as uint64_t -- SHA3-256 output
/// has excellent distribution, so this is a high-quality hash (per RESEARCH Pitfall 6).
struct Namespace32Hash {
    size_t operator()(const Namespace32& ns) const noexcept {
        uint64_t h;
        std::memcpy(&h, ns.data(), sizeof(h));
        return static_cast<size_t>(h);
    }
};

/// Result of a subscribe operation.
struct SubscribeResult {
    bool forward_to_node;                  ///< True if any namespace transitioned 0->1 subscribers
    std::vector<Namespace32> new_namespaces;  ///< Namespaces that need to be sent to the node
};

/// Result of an unsubscribe operation.
struct UnsubscribeResult {
    bool forward_to_node;                       ///< True if any namespace's last subscriber left
    std::vector<Namespace32> removed_namespaces;  ///< Namespaces to unsubscribe from node
};

/// Reference-counted subscription aggregation.
///
/// Multiple WebSocket clients subscribe to namespaces. The tracker ensures
/// the node sees exactly one Subscribe per namespace (first subscriber)
/// and one Unsubscribe (last subscriber leaves).
///
/// Thread-safe via mutex -- accessed from HTTP handler threads and UDS read_loop.
class SubscriptionTracker {
public:
    /// Subscribe a client session to one or more namespaces.
    /// Returns which namespaces are new (need forwarding to node).
    SubscribeResult subscribe(uint64_t session_id, const std::vector<Namespace32>& namespaces);

    /// Unsubscribe a client session from one or more namespaces.
    /// Returns which namespaces became empty (need forwarding to node).
    UnsubscribeResult unsubscribe(uint64_t session_id, const std::vector<Namespace32>& namespaces);

    /// Remove all subscriptions for a disconnected client.
    /// Returns namespaces that became empty (caller decides whether to send Unsubscribe).
    std::vector<Namespace32> remove_client(uint64_t session_id);

    /// Return all currently tracked namespaces (for subscription replay after reconnect).
    std::vector<Namespace32> get_all_namespaces() const;

    /// Return set of session IDs subscribed to a namespace (for notification fan-out).
    std::unordered_set<uint64_t> get_subscribers(const Namespace32& ns) const;

    /// Return number of namespaces a client is subscribed to (for cap enforcement).
    size_t client_subscription_count(uint64_t session_id) const;

    /// Return total number of tracked namespaces (for gauge computation).
    size_t namespace_count() const {
        std::lock_guard lock(mu_);
        return subs_.size();
    }

    /// Lock for thread-safe access.
    std::mutex& mutex() { return mu_; }

private:
    mutable std::mutex mu_;

    /// Namespace -> set of subscribed session IDs (per D-02)
    std::unordered_map<Namespace32, std::unordered_set<uint64_t>, Namespace32Hash> subs_;

    /// Per-client tracking for cap enforcement and disconnect cleanup
    std::unordered_map<uint64_t, std::unordered_set<Namespace32, Namespace32Hash>> client_subs_;
};

} // namespace chromatindb::relay::core
