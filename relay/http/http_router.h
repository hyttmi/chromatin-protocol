#pragma once

#include "relay/http/http_response.h"

#include <asio.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace chromatindb::relay::core {
class Authenticator;
class MetricsCollector;
} // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

struct HttpRequest;
struct HttpSessionState;
class TokenStore;

/// HTTP router with prefix-match routing table and auth middleware.
/// Per D-04, D-05, D-06: Bearer token auth for non-public routes.
class HttpRouter {
public:
    /// Synchronous handler (for auth, health, simple endpoints).
    using Handler = std::function<HttpResponse(
        const HttpRequest&, const std::vector<uint8_t>& body, HttpSessionState*)>;

    /// Async handler (for data endpoints that co_await UDS responses).
    using AsyncHandler = std::function<asio::awaitable<HttpResponse>(
        const HttpRequest&, const std::vector<uint8_t>& body, HttpSessionState*)>;

    /// Register a synchronous route. public_route=true means no auth check.
    void add_route(std::string_view method, std::string_view prefix,
                   Handler handler, bool public_route = false);

    /// Register an asynchronous route. public_route=true means no auth check.
    void add_async_route(std::string_view method, std::string_view prefix,
                         AsyncHandler handler, bool public_route = false);

    /// Synchronous dispatch (for sync routes only, used by tests and simple paths).
    HttpResponse dispatch(const HttpRequest& req, const std::vector<uint8_t>& body,
                          TokenStore& token_store);

    /// Async dispatch -- handles both sync and async routes.
    /// HttpConnection::handle() should use this.
    asio::awaitable<HttpResponse> dispatch_async(const HttpRequest& req,
                                                  const std::vector<uint8_t>& body,
                                                  TokenStore& token_store);

    struct Route {
        std::string method;
        std::string prefix;
        Handler sync_handler;      // Non-null for sync routes
        AsyncHandler async_handler; // Non-null for async routes
        bool public_route;
        bool is_async;
    };

    static bool route_matches(const Route& route, const HttpRequest& req);

    std::vector<Route> routes_;  // Linear scan; ~25 routes max, fast enough.
};

/// Register POST /auth/challenge and POST /auth/verify routes (public, no auth required).
void register_auth_routes(HttpRouter& router, core::Authenticator& authenticator,
                          TokenStore& token_store);

/// Register GET /health route (public, no auth required).
using HealthProvider = std::function<bool()>;
void register_health_route(HttpRouter& router, HealthProvider health_provider);

/// Register GET /metrics route (public, no auth required).
/// Calls MetricsCollector::format_prometheus() on each request.
void register_metrics_route(HttpRouter& router, core::MetricsCollector& metrics);

} // namespace chromatindb::relay::http
