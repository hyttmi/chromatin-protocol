#include "relay/http/http_router.h"
#include "relay/http/http_parser.h"
#include "relay/http/token_store.h"
#include "relay/core/authenticator.h"
#include "relay/core/metrics_collector.h"
#include "relay/util/hex.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
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
        nullptr,  // no async handler
        public_route,
        false     // sync
    });
}

void HttpRouter::add_async_route(std::string_view method, std::string_view prefix,
                                 AsyncHandler handler, bool public_route) {
    routes_.push_back(Route{
        std::string(method),
        std::string(prefix),
        nullptr,  // no sync handler
        std::move(handler),
        public_route,
        true      // async
    });
}

// ---------------------------------------------------------------------------
// Helper: prefix matching
// ---------------------------------------------------------------------------

bool HttpRouter::route_matches(const Route& route, const HttpRequest& req) {
    return req.path == route.prefix ||
           (route.prefix.back() == '/' && req.path.size() > route.prefix.size() &&
            req.path.compare(0, route.prefix.size(), route.prefix) == 0);
}

// ---------------------------------------------------------------------------
// Helper: auth middleware (sync version for sync dispatch path)
// ---------------------------------------------------------------------------

static HttpResponse check_auth_sync(const HttpRequest& req, TokenStore& token_store,
                                     HttpSessionState** out_session) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        return HttpResponse::error(401, "unauthorized", "missing Authorization header");
    }

    const auto& auth_val = it->second;
    if (auth_val.size() < 8 || auth_val.compare(0, 7, "Bearer ") != 0) {
        return HttpResponse::error(401, "unauthorized", "invalid Authorization format");
    }
    auto token = auth_val.substr(7);

    auto* session = token_store.lookup(token);
    if (!session) {
        return HttpResponse::error(401, "unauthorized", "invalid or expired token");
    }

    if (!session->rate_limiter.try_consume()) {
        return HttpResponse::error(429, "rate_limited", "rate limit exceeded");
    }

    *out_session = session;
    HttpResponse ok;
    ok.status = 0;
    return ok;
}

// ---------------------------------------------------------------------------
// Helper: async auth middleware (posts to strand before TokenStore access)
// ---------------------------------------------------------------------------

using Strand = asio::strand<asio::io_context::executor_type>;

static asio::awaitable<HttpResponse> check_auth(const HttpRequest& req,
                                                  TokenStore& token_store,
                                                  HttpSessionState** out_session,
                                                  Strand& strand) {
    // Parse Authorization header off-strand (no shared state).
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        co_return HttpResponse::error(401, "unauthorized", "missing Authorization header");
    }

    const auto& auth_val = it->second;
    if (auth_val.size() < 8 || auth_val.compare(0, 7, "Bearer ") != 0) {
        co_return HttpResponse::error(401, "unauthorized", "invalid Authorization format");
    }
    auto token = auth_val.substr(7);

    // Enter strand for TokenStore::lookup (shared state).
    co_await asio::post(strand, asio::use_awaitable);

    auto* session = token_store.lookup(token);
    if (!session) {
        co_return HttpResponse::error(401, "unauthorized", "invalid or expired token");
    }

    if (!session->rate_limiter.try_consume()) {
        co_return HttpResponse::error(429, "rate_limited", "rate limit exceeded");
    }

    *out_session = session;
    // Return status 0 to indicate success (caller checks status != 0 for error).
    HttpResponse ok;
    ok.status = 0;
    co_return ok;
}

// ---------------------------------------------------------------------------
// Synchronous dispatch
// ---------------------------------------------------------------------------

HttpResponse HttpRouter::dispatch(const HttpRequest& req, const std::vector<uint8_t>& body,
                                  TokenStore& token_store) {
    bool prefix_matched = false;

    for (const auto& route : routes_) {
        if (route_matches(route, req)) {
            prefix_matched = true;
            if (req.method != route.method) continue;

            if (!route.public_route) {
                HttpSessionState* session = nullptr;
                auto auth_result = check_auth_sync(req, token_store, &session);
                if (auth_result.status != 0) return auth_result;
                if (route.is_async) {
                    // Can't call async handler from sync dispatch.
                    return HttpResponse::error(500, "internal_error",
                        "async route called from sync context");
                }
                return route.sync_handler(req, body, session);
            }

            if (route.is_async) {
                return HttpResponse::error(500, "internal_error",
                    "async route called from sync context");
            }
            return route.sync_handler(req, body, nullptr);
        }
    }

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
// Async dispatch (for HttpConnection coroutine context)
// ---------------------------------------------------------------------------

asio::awaitable<HttpResponse> HttpRouter::dispatch_async(
    const HttpRequest& req, const std::vector<uint8_t>& body,
    TokenStore& token_store) {

    bool prefix_matched = false;

    for (const auto& route : routes_) {
        if (route_matches(route, req)) {
            prefix_matched = true;
            if (req.method != route.method) continue;

            HttpSessionState* session = nullptr;
            if (!route.public_route) {
                if (strand_) {
                    auto auth_result = co_await check_auth(req, token_store, &session, *strand_);
                    if (auth_result.status != 0) co_return auth_result;
                } else {
                    // Fallback for tests without strand
                    auto auth_result = check_auth_sync(req, token_store, &session);
                    if (auth_result.status != 0) co_return auth_result;
                }
            }

            if (route.is_async) {
                co_return co_await route.async_handler(req, body, session);
            } else {
                co_return route.sync_handler(req, body, session);
            }
        }
    }

    if (prefix_matched) {
        HttpResponse resp;
        resp.status = 405;
        resp.status_text = "Method Not Allowed";
        auto j = nlohmann::json{{"error", "method_not_allowed"}};
        auto body_str = j.dump();
        resp.body.assign(body_str.begin(), body_str.end());
        resp.headers.emplace_back("Content-Type", "application/json");
        resp.headers.emplace_back("Content-Length", std::to_string(resp.body.size()));
        co_return resp;
    }

    co_return HttpResponse::not_found();
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
                          TokenStore& token_store, HttpRouter::Strand& strand) {
    auto challenges = std::make_shared<ChallengeStore>();

    // POST /auth/challenge: generate nonce, store in ChallengeStore.
    // Async: posts to strand before ChallengeStore::add.
    router.add_async_route("POST", "/auth/challenge",
        [&authenticator, challenges, &strand](
            const HttpRequest&, const std::vector<uint8_t>&,
            HttpSessionState*) -> asio::awaitable<HttpResponse> {

            // Off-strand: generate_challenge uses RAND_bytes (thread-safe).
            auto challenge_bytes = authenticator.generate_challenge();
            auto nonce_hex = util::to_hex(challenge_bytes);

            // Enter strand for ChallengeStore access.
            co_await asio::post(strand, asio::use_awaitable);
            challenges->add(nonce_hex, challenge_bytes);

            spdlog::debug("auth: challenge issued, nonce={}", nonce_hex);
            co_return HttpResponse::json(200, {{"nonce", nonce_hex}});
        }, true);

    // POST /auth/verify: validate signature, create session token.
    // Async: crypto verification off-strand per D-16, then posts to strand
    // before ChallengeStore::consume and TokenStore::create_session.
    router.add_async_route("POST", "/auth/verify",
        [&authenticator, &token_store, challenges, &strand](
            const HttpRequest&, const std::vector<uint8_t>& body,
            HttpSessionState*) -> asio::awaitable<HttpResponse> {

            // Off-strand: JSON parsing (CPU work per D-16).
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(body.begin(), body.end());
            } catch (const nlohmann::json::parse_error&) {
                co_return HttpResponse::error(400, "bad_json", "invalid JSON body");
            }

            if (!j.contains("nonce") || !j["nonce"].is_string() ||
                !j.contains("public_key") || !j["public_key"].is_string() ||
                !j.contains("signature") || !j["signature"].is_string()) {
                co_return HttpResponse::error(400, "missing_field",
                    "required: nonce, public_key, signature");
            }

            auto nonce_hex = j["nonce"].get<std::string>();
            auto pubkey_hex = j["public_key"].get<std::string>();
            auto sig_hex = j["signature"].get<std::string>();

            // Off-strand: hex decoding (pure computation).
            auto pubkey_bytes = util::from_hex(pubkey_hex);
            if (!pubkey_bytes || pubkey_bytes->size() != 2592) {
                co_return HttpResponse::error(401, "bad_pubkey_format",
                    "public key must be 2592 bytes (5184 hex chars)");
            }

            auto sig_bytes = util::from_hex(sig_hex);
            if (!sig_bytes) {
                co_return HttpResponse::error(401, "bad_signature_format",
                    "invalid hex signature");
            }

            // Enter strand for ChallengeStore::consume (shared state).
            co_await asio::post(strand, asio::use_awaitable);

            auto challenge_bytes = challenges->consume(nonce_hex);
            if (!challenge_bytes) {
                co_return HttpResponse::error(401, "invalid_nonce",
                    "nonce not found or expired");
            }

            // ML-DSA-87 verification: CPU-heavy crypto. Per D-16 this should
            // ideally run off-strand, but the verify call is short enough
            // relative to I/O that keeping it on-strand avoids complexity.
            // The strand serializes access but doesn't block I/O threads --
            // other coroutines simply queue behind this one.
            auto result = authenticator.verify(*challenge_bytes, *pubkey_bytes, *sig_bytes);

            if (!result.success) {
                spdlog::info("auth: verification failed: {}", result.error_code);
                co_return HttpResponse::error(401, "auth_failed", result.error_code);
            }

            // On-strand: TokenStore::create_session (shared state).
            auto token = token_store.create_session(
                std::move(result.public_key),
                result.namespace_hash,
                0
            );

            auto ns_hex = util::to_hex(result.namespace_hash);
            spdlog::info("auth: session created, namespace={}", ns_hex);

            co_return HttpResponse::json(200, {
                {"token", token},
                {"namespace", ns_hex}
            });
        }, true);
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
        }, true);
}

// ---------------------------------------------------------------------------
// Metrics route registration
// ---------------------------------------------------------------------------

void register_metrics_route(HttpRouter& router, core::MetricsCollector& metrics) {
    router.add_route("GET", "/metrics",
        [&metrics](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) -> HttpResponse {
            auto body = metrics.format_prometheus();
            HttpResponse resp;
            resp.status = 200;
            resp.status_text = "OK";
            resp.body.assign(body.begin(), body.end());
            resp.headers.emplace_back("Content-Type",
                "text/plain; version=0.0.4; charset=utf-8");
            resp.headers.emplace_back("Content-Length",
                std::to_string(resp.body.size()));
            return resp;
        }, true);
}

} // namespace chromatindb::relay::http
