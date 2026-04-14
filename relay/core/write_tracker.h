#pragma once

/// WriteTracker: maps blob_hash -> writer_session_id for relay-side
/// notification source exclusion (FEAT-01).
///
/// When a client writes a blob via the relay, the relay records the
/// blob_hash from the WriteAck/DeleteAck response. When the corresponding
/// Notification(21) arrives, the relay looks up the writer session and
/// skips it during fan-out.
///
/// Thread-safe via mutex -- accessed from HTTP handler threads and UDS read_loop.

#include "relay/core/subscription_tracker.h"  // for Namespace32Hash

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace chromatindb::relay::core {

using BlobHash32 = std::array<uint8_t, 32>;

class WriteTracker {
public:
    /// Record that session_id wrote the blob identified by blob_hash.
    /// Performs lazy expiry sweep of stale entries on each call.
    void record(const BlobHash32& blob_hash, uint64_t session_id) {
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (now - it->second.created > TTL) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        entries_[blob_hash] = {session_id, now};
    }

    /// Look up and remove the writer session for a blob_hash.
    /// Returns session_id if found and not expired, nullopt otherwise.
    std::optional<uint64_t> lookup_and_remove(const BlobHash32& blob_hash) {
        std::lock_guard lock(mu_);
        auto it = entries_.find(blob_hash);
        if (it == entries_.end()) return std::nullopt;
        auto now = std::chrono::steady_clock::now();
        if (now - it->second.created > TTL) {
            entries_.erase(it);
            return std::nullopt;
        }
        auto session_id = it->second.session_id;
        entries_.erase(it);
        return session_id;
    }

    /// Remove all entries for a disconnected session.
    void remove_session(uint64_t session_id) {
        std::lock_guard lock(mu_);
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (it->second.session_id == session_id) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// Return number of tracked entries.
    size_t size() const {
        std::lock_guard lock(mu_);
        return entries_.size();
    }

private:
    struct Entry {
        uint64_t session_id;
        std::chrono::steady_clock::time_point created;
    };

    static constexpr auto TTL = std::chrono::seconds(5);
    mutable std::mutex mu_;
    std::unordered_map<BlobHash32, Entry, Namespace32Hash> entries_;
};

} // namespace chromatindb::relay::core
