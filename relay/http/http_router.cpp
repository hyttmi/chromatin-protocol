#include "relay/http/http_router.h"
#include "relay/http/http_parser.h"
#include "relay/http/token_store.h"
#include "relay/core/authenticator.h"
#include "relay/util/hex.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace chromatindb::relay::http {

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void HttpRouter::add_route(std::string_view method, std::string_view prefix,
                           Handler handler, bool public_route) {
    routes_.push_back(Route{
        std::string(method),
        std::string(prefix),
        std::move(handler),
        public_route
    });
}

// ---------------------------------------------------------------------------
// Dispatch with auth middleware
// ---------------------------------------------------------------------------

HttpResponse HttpRouter::dispatch(const HttpRequest& req, const std::vector<uint8_t>& body,
                                  TokenStore& token_store) {
    // Track whether we found any prefix match (for 405 vs 404 distinction).
    bool prefix_matched = false;

    for (const auto& route : routes_) {
        // Check prefix match first.
        if (req.path == route.prefix ||
            (route.prefix.back() == '/' && req.path.size() > route.prefix.size() &&
             req.path.compare(0, route.prefix.size(), route.prefix) == 0)) {

            prefix_matched = true;

            // Check method.
            if (req.method != route.method) continue;

            // Auth middleware for non-public routes.
            if (!route.public_route) {
                auto it = req.headers.find("authorization");
                if (it == req.headers.end()) {
                    return HttpResponse::error(401, "unauthorized", "missing Authorization header");
                }

                // Parse "Bearer <token>".
                const auto& auth_val = it->second;
                if (auth_val.size() < 8 || auth_val.compare(0, 7, "Bearer ") != 0) {
                    return HttpResponse::error(401, "unauthorized", "invalid Authorization format");
                }
                auto token = auth_val.substr(7);

                auto* session = token_store.lookup(token);
                if (!session) {
                    return HttpResponse::error(401, "unauthorized", "invalid or expired token");
                }

                // Rate limit check per D-31.
                if (!session->rate_limiter.try_consume()) {
                    return HttpResponse::error(429, "rate_limited", "rate limit exceeded");
                }

                return route.handler(req, body, session);
            }

            // Public route -- no auth needed.
            return route.handler(req, body, nullptr);
        }
    }

    // No prefix match at all -> 404.
    // If prefix matched but method didn't -> 405.
    if (prefix_matched) {
        HttpResponse resp;
        resp.status = 405;
        resp.status_text = "Method Not Allowed";
        auto j = nlohmann::json{{"error", "method_not_allowed"}};
        auto body_str = j.dump();
        resp.body.assign(body_str.begin(), body_str.end());
        resp.headers.emplace_back("Content-Type", "application/json");
        resp.headers.emplace_back("Content-Length", std::to_string(resp.body.size()));
        return resp;
    }

    return HttpResponse::not_found();
}

// ---------------------------------------------------------------------------
// Pending challenges store (shared between challenge and verify handlers)
// ---------------------------------------------------------------------------

struct PendingChallenge {
    std::array<uint8_t, 32> bytes;
    std::chrono::steady_clock::time_point created;
};

/// Thread-safe pending challenges map.
/// Challenges expire after 30 seconds.
class ChallengeStore {
public:
    void add(const std::string& nonce_hex, std::array<uint8_t, 32> bytes) {
        reap_expired();
        challenges_[nonce_hex] = PendingChallenge{bytes, std::chrono::steady_clock::now()};
    }

    /// Look up and consume a challenge. Returns nullopt if not found or expired.
    std::optional<std::array<uint8_t, 32>> consume(const std::string& nonce_hex) {
        auto it = challenges_.find(nonce_hex);
        if (it == challenges_.end()) return std::nullopt;

        auto age = std::chrono::steady_clock::now() - it->second.created;
        if (age > EXPIRY) {
            challenges_.erase(it);
            return std::nullopt;
        }

        auto bytes = it->second.bytes;
        challenges_.erase(it);
        return bytes;
    }

private:
    void reap_expired() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = challenges_.begin(); it != challenges_.end(); ) {
            if (now - it->second.created > EXPIRY) {
                it = challenges_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::unordered_map<std::string, PendingChallenge> challenges_;
    static constexpr auto EXPIRY = std::chrono::seconds(30);
};

// ---------------------------------------------------------------------------
// Auth route registration (per D-04, D-05, D-06)
// ---------------------------------------------------------------------------

void register_auth_routes(HttpRouter& router, core::Authenticator& authenticator,
                          TokenStore& token_store) {
    // Shared challenge store -- captured by shared_ptr for lambda lifetime safety.
    auto challenges = std::make_shared<ChallengeStore>();

    // POST /auth/challenge: generate a nonce and return it.
    router.add_route("POST", "/auth/challenge",
        [&authenticator, challenges](
            const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) -> HttpResponse {

            auto challenge_bytes = authenticator.generate_challenge();
            auto nonce_hex = util::to_hex(challenge_bytes);
            challenges->add(nonce_hex, challenge_bytes);

            spdlog::debug("auth: challenge issued, nonce={}", nonce_hex);
            return HttpResponse::json(200, {{"nonce", nonce_hex}});
        }, true);  // Public route.

    // POST /auth/verify: verify signature against a pending challenge.
    router.add_route("POST", "/auth/verify",
        [&authenticator, &token_store, challenges](
            const HttpRequest&, const std::vector<uint8_t>& body, HttpSessionState*) -> HttpResponse {

            // 1. Parse JSON body.
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(body.begin(), body.end());
            } catch (const nlohmann::json::parse_error&) {
                return HttpResponse::error(400, "bad_json", "invalid JSON body");
            }

            // 2. Validate required fields.
            if (!j.contains("nonce") || !j["nonce"].is_string() ||
                !j.contains("public_key") || !j["public_key"].is_string() ||
                !j.contains("signature") || !j["signature"].is_string()) {
                return HttpResponse::error(400, "missing_field",
                    "required: nonce, public_key, signature");
            }

            auto nonce_hex = j["nonce"].get<std::string>();
            auto pubkey_hex = j["public_key"].get<std::string>();
            auto sig_hex = j["signature"].get<std::string>();

            // 3. Look up the pending challenge by nonce.
            auto challenge_bytes = challenges->consume(nonce_hex);
            if (!challenge_bytes) {
                return HttpResponse::error(401, "invalid_nonce",
                    "nonce not found or expired");
            }

            // 4. Decode hex pubkey.
            auto pubkey_bytes = util::from_hex(pubkey_hex);
            if (!pubkey_bytes || pubkey_bytes->size() != 2592) {
                return HttpResponse::error(401, "bad_pubkey_format",
                    "public key must be 2592 bytes (5184 hex chars)");
            }

            // 5. Decode hex signature.
            auto sig_bytes = util::from_hex(sig_hex);
            if (!sig_bytes) {
                return HttpResponse::error(401, "bad_signature_format",
                    "invalid hex signature");
            }

            // 6. Verify signature (blocking -- this is synchronous in HTTP context,
            //    but verify is a few ms for ML-DSA-87).
            auto result = authenticator.verify(*challenge_bytes, *pubkey_bytes, *sig_bytes);

            if (!result.success) {
                spdlog::info("auth: verification failed: {}", result.error_code);
                return HttpResponse::error(401, "auth_failed", result.error_code);
            }

            // 7. Create session token.
            auto token = token_store.create_session(
                std::move(result.public_key),
                result.namespace_hash,
                0  // Rate limit set later via config.
            );

            auto ns_hex = util::to_hex(result.namespace_hash);
            spdlog::info("auth: session created, namespace={}", ns_hex);

            return HttpResponse::json(200, {
                {"token", token},
                {"namespace", ns_hex}
            });
        }, true);  // Public route (auth is the point of these endpoints).
}

// ---------------------------------------------------------------------------
// Health route registration
// ---------------------------------------------------------------------------

void register_health_route(HttpRouter& router, HealthProvider health_provider) {
    auto provider = std::make_shared<HealthProvider>(std::move(health_provider));

    router.add_route("GET", "/health",
        [provider](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) -> HttpResponse {
            bool node_connected = (*provider) ? (*provider)() : false;
            std::string status_str = node_connected ? "healthy" : "degraded";
            std::string node_str = node_connected ? "connected" : "disconnected";
            uint16_t http_status = node_connected ? 200 : 503;

            return HttpResponse::json(http_status, {
                {"status", status_str},
                {"relay", "ok"},
                {"node", node_str}
            });
        }, true);  // Public route.
}

} // namespace chromatindb::relay::http
