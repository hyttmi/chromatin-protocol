#pragma once

#include "relay/core/rate_limiter.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chromatindb::relay::http {

class SseWriter;  // Forward declaration (sse_writer.h)

struct HttpSessionState {
    uint64_t session_id = 0;
    std::vector<uint8_t> client_pubkey;
    std::array<uint8_t, 32> client_namespace{};
    core::RateLimiter rate_limiter;
    std::chrono::steady_clock::time_point last_activity;

    /// Non-owning pointer to the SSE writer for this session's event stream.
    /// Set when GET /events establishes an SSE connection.
    /// Null when no SSE stream is active. The SseWriter lives on the
    /// HttpConnection's coroutine stack.
    SseWriter* sse_writer = nullptr;
};

/// Maps opaque hex tokens to HTTP session state.
/// Thread-safe via mutex -- accessed from multiple io_context threads.
class TokenStore {
public:
    /// Create a new session. Returns hex token (64 chars).
    std::string create_session(std::vector<uint8_t> pubkey,
                               std::array<uint8_t, 32> ns_hash,
                               uint32_t rate_limit = 0);

    /// Look up session by token. Updates last_activity. Returns nullptr if not found.
    HttpSessionState* lookup(const std::string& token);

    /// Look up session by session_id. Returns nullptr if not found.
    HttpSessionState* lookup_by_id(uint64_t session_id);

    /// Get token for a session_id (for reverse lookup).
    const std::string* get_token(uint64_t session_id) const;

    /// Remove session by ID.
    void remove_session(uint64_t session_id);

    /// Remove session by token.
    void remove_by_token(const std::string& token);

    /// Reap sessions idle longer than timeout. Returns session IDs of reaped sessions.
    /// If a reaped session has an active SseWriter, it is closed before removal.
    std::vector<uint64_t> reap_idle(std::chrono::seconds timeout);

    /// Number of active sessions.
    size_t count() const;

    /// Iterate all sessions (const).
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [token, state] : tokens_) {
            fn(state);
        }
    }

    /// Iterate all sessions (mutable).
    template<typename Fn>
    void for_each_mut(Fn&& fn) {
        for (auto& [token, state] : tokens_) {
            fn(state);
        }
    }

    /// Expose mutex for callers that need to hold the lock across multiple operations.
    std::mutex& mutex() { return mu_; }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, HttpSessionState> tokens_;       // token -> state
    std::unordered_map<uint64_t, std::string> id_to_token_;          // session_id -> token
    uint64_t next_id_ = 1;
};

} // namespace chromatindb::relay::http
