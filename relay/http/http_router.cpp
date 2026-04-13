#include "relay/http/http_router.h"
#include "relay/http/http_parser.h"
#include "relay/http/token_store.h"

#include <spdlog/spdlog.h>

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

} // namespace chromatindb::relay::http
