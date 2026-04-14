#include <catch2/catch_test_macros.hpp>

#include "relay/http/http_parser.h"
#include "relay/http/http_response.h"
#include "relay/http/http_router.h"
#include "relay/http/token_store.h"
#include "relay/core/authenticator.h"

#include <asio.hpp>
#include <nlohmann/json.hpp>

using namespace chromatindb::relay::http;
namespace core = chromatindb::relay::core;
using json = nlohmann::json;
using Strand = asio::strand<asio::io_context::executor_type>;

// ---------------------------------------------------------------------------
// Helper: build a minimal HttpRequest
// ---------------------------------------------------------------------------
static HttpRequest make_request(const std::string& method, const std::string& path,
                                const std::string& auth_header = "") {
    HttpRequest req;
    req.method = method;
    req.path = path;
    if (!auth_header.empty()) {
        req.headers["authorization"] = auth_header;
    }
    return req;
}

// ===========================================================================
// Route matching
// ===========================================================================

TEST_CASE("HttpRouter exact route match", "[http][router]") {
    HttpRouter router;
    bool handler_called = false;

    router.add_route("GET", "/health",
        [&](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            handler_called = true;
            return HttpResponse::json(200, {{"status", "ok"}});
        }, true);

    TokenStore store;
    auto req = make_request("GET", "/health");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 200);
    REQUIRE(handler_called);
}

TEST_CASE("HttpRouter prefix route match", "[http][router]") {
    HttpRouter router;
    std::string captured_path;

    router.add_route("GET", "/blob/",
        [&](const HttpRequest& r, const std::vector<uint8_t>&, HttpSessionState*) {
            captured_path = r.path;
            return HttpResponse::json(200, {{"found", true}});
        }, true);

    TokenStore store;
    auto req = make_request("GET", "/blob/abc/def");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 200);
    REQUIRE(captured_path == "/blob/abc/def");
}

TEST_CASE("HttpRouter returns 404 for unknown route", "[http][router]") {
    HttpRouter router;

    router.add_route("GET", "/health",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"status", "ok"}});
        }, true);

    TokenStore store;
    auto req = make_request("GET", "/unknown");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 404);
}

TEST_CASE("HttpRouter returns 405 for wrong method on known path", "[http][router]") {
    HttpRouter router;

    router.add_route("POST", "/auth/challenge",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"nonce", "abc"}});
        }, true);

    TokenStore store;
    auto req = make_request("GET", "/auth/challenge");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 405);
}

// ===========================================================================
// Auth middleware
// ===========================================================================

TEST_CASE("HttpRouter auth middleware rejects missing Authorization header", "[http][router][auth]") {
    HttpRouter router;

    router.add_route("GET", "/protected",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"data", "secret"}});
        }, false);  // NOT public -- requires auth

    TokenStore store;
    auto req = make_request("GET", "/protected");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 401);
}

TEST_CASE("HttpRouter auth middleware rejects invalid Bearer token", "[http][router][auth]") {
    HttpRouter router;

    router.add_route("GET", "/protected",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"data", "secret"}});
        }, false);

    TokenStore store;
    auto req = make_request("GET", "/protected", "Bearer invalid_token_here");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 401);
}

TEST_CASE("HttpRouter auth middleware accepts valid Bearer token", "[http][router][auth]") {
    HttpRouter router;
    HttpSessionState* received_session = nullptr;

    router.add_route("GET", "/protected",
        [&](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState* s) {
            received_session = s;
            return HttpResponse::json(200, {{"data", "secret"}});
        }, false);

    TokenStore store;
    std::array<uint8_t, 32> ns{};
    ns[0] = 0xAB;
    auto token = store.create_session({1, 2, 3}, ns, 0);

    auto req = make_request("GET", "/protected", "Bearer " + token);
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 200);
    REQUIRE(received_session != nullptr);
    REQUIRE(received_session->client_namespace[0] == 0xAB);
}

TEST_CASE("HttpRouter auth middleware rejects malformed Authorization format", "[http][router][auth]") {
    HttpRouter router;

    router.add_route("GET", "/protected",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"data", "secret"}});
        }, false);

    TokenStore store;
    auto req = make_request("GET", "/protected", "Basic dXNlcjpwYXNz");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 401);
}

// ===========================================================================
// Rate limiting via auth middleware
// ===========================================================================

TEST_CASE("HttpRouter auth middleware returns 429 when rate limited", "[http][router][rate]") {
    HttpRouter router;

    router.add_route("GET", "/protected",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"data", "ok"}});
        }, false);

    TokenStore store;
    std::array<uint8_t, 32> ns{};
    auto token = store.create_session({}, ns, 1);  // 1 req/sec rate limit

    // First request should succeed (token bucket starts full).
    auto req = make_request("GET", "/protected", "Bearer " + token);
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 200);

    // Rapid second request should be rate limited.
    auto resp2 = router.dispatch(req, {}, store);
    REQUIRE(resp2.status == 429);
}

// ===========================================================================
// Public vs authenticated route coexistence
// ===========================================================================

TEST_CASE("HttpRouter public routes skip auth check", "[http][router]") {
    HttpRouter router;

    router.add_route("GET", "/health",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"status", "ok"}});
        }, true);

    router.add_route("GET", "/protected",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            return HttpResponse::json(200, {{"data", "secret"}});
        }, false);

    TokenStore store;

    // Public route works without auth.
    auto req1 = make_request("GET", "/health");
    auto resp1 = router.dispatch(req1, {}, store);
    REQUIRE(resp1.status == 200);

    // Protected route fails without auth.
    auto req2 = make_request("GET", "/protected");
    auto resp2 = router.dispatch(req2, {}, store);
    REQUIRE(resp2.status == 401);
}

// ===========================================================================
// Body is passed through to handler
// ===========================================================================

TEST_CASE("HttpRouter passes body to handler", "[http][router]") {
    HttpRouter router;
    std::vector<uint8_t> received_body;

    router.add_route("POST", "/data",
        [&](const HttpRequest&, const std::vector<uint8_t>& body, HttpSessionState*) {
            received_body = body;
            return HttpResponse::json(200, {{"ok", true}});
        }, true);

    TokenStore store;
    auto req = make_request("POST", "/data");
    std::vector<uint8_t> body = {0x01, 0x02, 0x03};
    auto resp = router.dispatch(req, body, store);
    REQUIRE(resp.status == 200);
    REQUIRE(received_body == body);
}

// ===========================================================================
// Auth challenge endpoint
// ===========================================================================

/// Helper: run an async dispatch in a test context with ioc + strand.
static HttpResponse run_async_dispatch(HttpRouter& router, const HttpRequest& req,
                                        const std::vector<uint8_t>& body, TokenStore& store) {
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);
    router.set_strand(&strand);

    HttpResponse result;
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        result = co_await router.dispatch_async(req, body, store);
    }, asio::detached);
    ioc.run();
    return result;
}

TEST_CASE("POST /auth/challenge returns 200 with nonce", "[http][router][auth-endpoint]") {
    HttpRouter router;
    core::Authenticator authenticator;
    TokenStore store;
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);

    register_auth_routes(router, authenticator, store, strand);

    auto req = make_request("POST", "/auth/challenge");
    auto resp = run_async_dispatch(router, req, {}, store);
    REQUIRE(resp.status == 200);

    // Parse response body as JSON.
    auto body_str = std::string(resp.body.begin(), resp.body.end());
    auto j = json::parse(body_str);
    REQUIRE(j.contains("nonce"));
    REQUIRE(j["nonce"].is_string());
    // Nonce should be 64 hex chars (32 bytes).
    REQUIRE(j["nonce"].get<std::string>().size() == 64);
}

TEST_CASE("POST /auth/verify with invalid nonce returns 401", "[http][router][auth-endpoint]") {
    HttpRouter router;
    core::Authenticator authenticator;
    TokenStore store;
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);

    register_auth_routes(router, authenticator, store, strand);

    // Try to verify with a nonce that was never issued.
    json body_json = {
        {"nonce", "0000000000000000000000000000000000000000000000000000000000000000"},
        {"public_key", "00"},
        {"signature", "00"}
    };
    auto body_str = body_json.dump();
    std::vector<uint8_t> body(body_str.begin(), body_str.end());

    auto req = make_request("POST", "/auth/verify");
    req.content_length = body.size();
    auto resp = run_async_dispatch(router, req, body, store);
    REQUIRE(resp.status == 401);
}

TEST_CASE("POST /auth/verify with bad JSON returns 400", "[http][router][auth-endpoint]") {
    HttpRouter router;
    core::Authenticator authenticator;
    TokenStore store;
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);

    register_auth_routes(router, authenticator, store, strand);

    std::vector<uint8_t> body = {'n', 'o', 't', ' ', 'j', 's', 'o', 'n'};

    auto req = make_request("POST", "/auth/verify");
    req.content_length = body.size();
    auto resp = run_async_dispatch(router, req, body, store);
    REQUIRE(resp.status == 400);
}

TEST_CASE("POST /auth/verify with missing fields returns 400", "[http][router][auth-endpoint]") {
    HttpRouter router;
    core::Authenticator authenticator;
    TokenStore store;
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);

    register_auth_routes(router, authenticator, store, strand);

    json body_json = {{"nonce", "abc"}};  // Missing public_key and signature.
    auto body_str = body_json.dump();
    std::vector<uint8_t> body(body_str.begin(), body_str.end());

    auto req = make_request("POST", "/auth/verify");
    req.content_length = body.size();
    auto resp = run_async_dispatch(router, req, body, store);
    REQUIRE(resp.status == 400);
}

// ===========================================================================
// Health endpoint
// ===========================================================================

TEST_CASE("GET /health returns 200 with status JSON", "[http][router][health]") {
    HttpRouter router;
    TokenStore store;

    register_health_route(router, []() { return true; });

    auto req = make_request("GET", "/health");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 200);

    auto body_str = std::string(resp.body.begin(), resp.body.end());
    auto j = json::parse(body_str);
    REQUIRE(j.contains("status"));
    REQUIRE(j["status"] == "healthy");
}

TEST_CASE("GET /health returns 503 when node disconnected", "[http][router][health]") {
    HttpRouter router;
    TokenStore store;

    register_health_route(router, []() { return false; });

    auto req = make_request("GET", "/health");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 503);

    auto body_str = std::string(resp.body.begin(), resp.body.end());
    auto j = json::parse(body_str);
    REQUIRE(j["status"] == "degraded");
}

// ===========================================================================
// Multiple routes with same prefix but different methods
// ===========================================================================

TEST_CASE("HttpRouter routes different methods on same path", "[http][router]") {
    HttpRouter router;
    bool get_called = false;
    bool post_called = false;

    router.add_route("GET", "/resource",
        [&](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            get_called = true;
            return HttpResponse::json(200, {{"method", "GET"}});
        }, true);

    router.add_route("POST", "/resource",
        [&](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*) {
            post_called = true;
            return HttpResponse::json(200, {{"method", "POST"}});
        }, true);

    TokenStore store;

    auto req1 = make_request("GET", "/resource");
    router.dispatch(req1, {}, store);
    REQUIRE(get_called);
    REQUIRE_FALSE(post_called);

    auto req2 = make_request("POST", "/resource");
    router.dispatch(req2, {}, store);
    REQUIRE(post_called);
}
