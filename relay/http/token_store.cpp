#include "relay/http/token_store.h"
#include "relay/http/sse_writer.h"
#include "relay/util/hex.h"

#include <openssl/rand.h>

namespace chromatindb::relay::http {

std::string TokenStore::create_session(std::vector<uint8_t> pubkey,
                                       std::array<uint8_t, 32> ns_hash,
                                       uint32_t rate_limit) {
    // Generate 32 random bytes -> 64-char hex token
    std::array<uint8_t, 32> random_bytes{};
    RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size()));
    std::string token = util::to_hex(random_bytes);

    std::lock_guard lock(mu_);

    // Build session state
    auto id = next_id_++;
    HttpSessionState state;
    state.session_id = id;
    state.client_pubkey = std::move(pubkey);
    state.client_namespace = ns_hash;
    state.last_activity = std::chrono::steady_clock::now();
    if (rate_limit > 0) {
        state.rate_limiter.set_rate(rate_limit);
    }

    // Store in both maps
    id_to_token_[id] = token;
    tokens_[token] = std::move(state);

    return token;
}

HttpSessionState* TokenStore::lookup(const std::string& token) {
    std::lock_guard lock(mu_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return nullptr;
    }
    it->second.last_activity = std::chrono::steady_clock::now();
    return &it->second;
}

HttpSessionState* TokenStore::lookup_by_id(uint64_t session_id) {
    std::lock_guard lock(mu_);
    auto it = id_to_token_.find(session_id);
    if (it == id_to_token_.end()) {
        return nullptr;
    }
    auto tok_it = tokens_.find(it->second);
    if (tok_it == tokens_.end()) {
        return nullptr;
    }
    return &tok_it->second;
}

const std::string* TokenStore::get_token(uint64_t session_id) const {
    std::lock_guard lock(mu_);
    auto it = id_to_token_.find(session_id);
    if (it == id_to_token_.end()) {
        return nullptr;
    }
    return &it->second;
}

void TokenStore::remove_session(uint64_t session_id) {
    std::lock_guard lock(mu_);
    auto it = id_to_token_.find(session_id);
    if (it == id_to_token_.end()) {
        return;
    }
    tokens_.erase(it->second);
    id_to_token_.erase(it);
}

void TokenStore::remove_by_token(const std::string& token) {
    std::lock_guard lock(mu_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return;
    }
    id_to_token_.erase(it->second.session_id);
    tokens_.erase(it);
}

std::vector<uint64_t> TokenStore::reap_idle(std::chrono::seconds timeout) {
    std::lock_guard lock(mu_);
    auto now = std::chrono::steady_clock::now();
    std::vector<uint64_t> reaped_ids;

    for (auto it = tokens_.begin(); it != tokens_.end(); ) {
        auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.last_activity);
        if (idle_duration >= timeout) {
            // Close SSE writer if active (stops drain loop cleanly).
            if (it->second.sse_writer) {
                it->second.sse_writer->close();
            }
            reaped_ids.push_back(it->second.session_id);
            id_to_token_.erase(it->second.session_id);
            it = tokens_.erase(it);
        } else {
            ++it;
        }
    }

    return reaped_ids;
}

size_t TokenStore::count() const {
    std::lock_guard lock(mu_);
    return tokens_.size();
}

} // namespace chromatindb::relay::http
