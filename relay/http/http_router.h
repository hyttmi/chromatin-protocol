#pragma once

#include "relay/http/http_response.h"

#include <asio.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chromatindb::relay::core {
class Authenticator;
} // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

struct HttpRequest;
struct HttpSessionState;
class TokenStore;

/// HTTP router with prefix-match routing table and auth middleware.
/// Per D-04, D-05, D-06: Bearer token auth for non-public routes.
/// Supports both synchronous and async (coroutine) handlers.
class HttpRouter {
public:
    /// Synchronous handler receives (request, body bytes, session state pointer).
    /// session is non-null for authenticated endpoints, null for public.
    using Handler = std::function<HttpResponse(
        const HttpRequest&, const std::vector<uint8_t>& body, HttpSessionState*)>;

    /// Async handler for endpoints that need co_await (e.g., UDS query forwarding).
    using AsyncHandler = std::function<asio::awaitable<HttpResponse>(
        const HttpRequest&, const std::vector<uint8_t>& body, HttpSessionState*)>;

    /// Register a synchronous route. public_route=true means no auth check.
    void add_route(std::string_view method, std::string_view prefix,
                   Handler handler, bool public_route = false);

    /// Register an async (coroutine) route. public_route=true means no auth check.
    void add_async_route(std::string_view method, std::string_view prefix,
                         AsyncHandler handler, bool public_route = false);

    /// Dispatch a request. Handles auth middleware for non-public routes.
    /// Returns synchronous response. Async routes must use dispatch_async().
    HttpResponse dispatch(const HttpRequest& req, const std::vector<uint8_t>& body,
                          TokenStore& token_store);

    /// Dispatch a request, supporting async handlers.
    /// If the matched route is async, co_awaits the handler.
    /// If synchronous, returns immediately.
    asio::awaitable<HttpResponse> dispatch_async(const HttpRequest& req,
                                                  const std::vector<uint8_t>& body,
                                                  TokenStore& token_store);

private:
    struct Route {
        std::string method;
        std::string prefix;
        std::variant<Handler, AsyncHandler> handler;
        bool public_route;
    };
    std::vector<Route> routes_;  // Linear scan; ~25 routes max, fast enough.
};

/// Register POST /auth/challenge and POST /auth/verify routes (public, no auth required).
/// Manages pending challenges internally (challenge nonce -> bytes map with 30s expiry).
/// Per D-04, D-05, D-06.
void register_auth_routes(HttpRouter& router, core::Authenticator& authenticator,
                          TokenStore& token_store);

/// Register GET /health route (public, no auth required).
/// health_provider returns true if node UDS is connected.
using HealthProvider = std::function<bool()>;
void register_health_route(HttpRouter& router, HealthProvider health_provider);

} // namespace chromatindb::relay::http
