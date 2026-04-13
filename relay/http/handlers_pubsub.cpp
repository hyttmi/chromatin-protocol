#include "relay/http/handlers_pubsub.h"
#include "relay/http/http_parser.h"
#include "relay/http/handlers_query.h"  // parse_query_param, is_valid_hex32
#include "relay/http/token_store.h"
#include "relay/core/subscription_tracker.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/util/endian.h"
#include "relay/util/hex.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstring>

namespace chromatindb::relay::http {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PubSubHandlers::PubSubHandlers(core::SubscriptionTracker& tracker,
                               core::UdsMultiplexer& uds,
                               TokenStore& token_store)
    : tracker_(tracker)
    , uds_(uds)
    , token_store_(token_store) {}

// ---------------------------------------------------------------------------
// Namespace list parsing (JSON -> Namespace32 array)
// ---------------------------------------------------------------------------

std::vector<std::array<uint8_t, 32>> PubSubHandlers::parse_namespace_list(
    const std::vector<uint8_t>& body) {
    std::vector<std::array<uint8_t, 32>> result;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body.begin(), body.end());
    } catch (...) {
        return result;
    }

    if (!j.contains("namespaces") || !j["namespaces"].is_array()) {
        return result;
    }

    for (const auto& ns_val : j["namespaces"]) {
        if (!ns_val.is_string()) continue;
        auto ns_hex = ns_val.get<std::string>();
        if (!is_valid_hex32(ns_hex)) continue;

        auto bytes = util::from_hex(ns_hex);
        if (!bytes || bytes->size() != 32) continue;

        std::array<uint8_t, 32> ns{};
        std::memcpy(ns.data(), bytes->data(), 32);
        result.push_back(ns);
    }

    return result;
}

// ---------------------------------------------------------------------------
// u16BE namespace list encoding (for node Subscribe/Unsubscribe wire format)
// ---------------------------------------------------------------------------

std::vector<uint8_t> PubSubHandlers::encode_namespace_list_u16be(
    const std::vector<std::array<uint8_t, 32>>& namespaces) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + namespaces.size() * 32);
    uint8_t buf[2];
    util::store_u16_be(buf, static_cast<uint16_t>(namespaces.size()));
    payload.insert(payload.end(), buf, buf + 2);
    for (const auto& ns : namespaces) {
        payload.insert(payload.end(), ns.begin(), ns.end());
    }
    return payload;
}

// ---------------------------------------------------------------------------
// POST /subscribe (D-22)
// ---------------------------------------------------------------------------

HttpResponse PubSubHandlers::handle_subscribe(const HttpRequest& /*req*/,
                                               const std::vector<uint8_t>& body,
                                               HttpSessionState* session) {
    if (!session) {
        return HttpResponse::error(401, "unauthorized", "Authentication required");
    }

    auto namespaces = parse_namespace_list(body);
    if (namespaces.empty()) {
        return HttpResponse::error(400, "bad_request",
                                   "Body must contain {\"namespaces\": [\"hex64\", ...]}");
    }

    // Cap check: 256 per session (same as WsSession)
    size_t current = tracker_.client_subscription_count(session->session_id);
    if (current + namespaces.size() > 256) {
        return HttpResponse::error(400, "subscription_limit",
                                   "Maximum 256 subscriptions per client");
    }

    auto sub_result = tracker_.subscribe(session->session_id, namespaces);

    // Forward new namespaces to node via UDS (reference count 0->1)
    if (sub_result.forward_to_node && uds_.is_connected()) {
        auto payload = encode_namespace_list_u16be(sub_result.new_namespaces);
        auto msg = wire::TransportCodec::encode(
            chromatindb::wire::TransportMsgType_Subscribe, payload, 0);
        uds_.send(std::move(msg));
        spdlog::debug("subscribe: forwarded {} new namespace(s) to node for session {}",
                      sub_result.new_namespaces.size(), session->session_id);
    }

    nlohmann::json resp = {
        {"status", "ok"},
        {"subscribed", namespaces.size()}
    };
    return HttpResponse::json(200, resp);
}

// ---------------------------------------------------------------------------
// POST /unsubscribe (D-23)
// ---------------------------------------------------------------------------

HttpResponse PubSubHandlers::handle_unsubscribe(const HttpRequest& /*req*/,
                                                 const std::vector<uint8_t>& body,
                                                 HttpSessionState* session) {
    if (!session) {
        return HttpResponse::error(401, "unauthorized", "Authentication required");
    }

    auto namespaces = parse_namespace_list(body);
    if (namespaces.empty()) {
        return HttpResponse::error(400, "bad_request",
                                   "Body must contain {\"namespaces\": [\"hex64\", ...]}");
    }

    auto unsub_result = tracker_.unsubscribe(session->session_id, namespaces);

    // Forward empty namespaces to node via UDS (reference count 1->0)
    if (unsub_result.forward_to_node && uds_.is_connected()) {
        auto payload = encode_namespace_list_u16be(unsub_result.removed_namespaces);
        auto msg = wire::TransportCodec::encode(
            chromatindb::wire::TransportMsgType_Unsubscribe, payload, 0);
        uds_.send(std::move(msg));
        spdlog::debug("unsubscribe: forwarded {} empty namespace(s) to node for session {}",
                      unsub_result.removed_namespaces.size(), session->session_id);
    }

    nlohmann::json resp = {
        {"status", "ok"},
        {"unsubscribed", namespaces.size()}
    };
    return HttpResponse::json(200, resp);
}

// ---------------------------------------------------------------------------
// GET /events?token=<token> auth check (D-24)
// ---------------------------------------------------------------------------

HttpResponse PubSubHandlers::handle_events_auth(const HttpRequest& req,
                                                 const std::vector<uint8_t>& /*body*/,
                                                 HttpSessionState* /*session*/) {
    // SSE endpoint uses token in query param, not Bearer header.
    // The router marks this as a public route, so session will be nullptr.
    auto token_opt = parse_query_param(req.query, "token");
    if (!token_opt || token_opt->empty()) {
        return HttpResponse::error(401, "unauthorized", "Missing token query parameter");
    }

    auto* sse_session = token_store_.lookup(*token_opt);
    if (!sse_session) {
        return HttpResponse::error(401, "unauthorized", "Invalid or expired token");
    }

    // Return a special 200 response that signals "this is an SSE session".
    // The actual SSE stream setup happens in HttpConnection or relay_main (Plan 08).
    // For now, return the session_id so the caller knows which session to wire up.
    //
    // The SSE response headers will be sent by HttpConnection when it detects
    // the /events path and creates the SseWriter.
    //
    // For now, mark as SSE-ready with a custom header to signal HttpConnection.
    HttpResponse resp;
    resp.status = 200;
    resp.status_text = "OK";
    resp.headers.emplace_back("Content-Type", "text/event-stream");
    resp.headers.emplace_back("Cache-Control", "no-cache");
    resp.headers.emplace_back("Connection", "keep-alive");
    resp.headers.emplace_back("X-SSE-Session-Id", std::to_string(sse_session->session_id));
    return resp;
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void register_pubsub_routes(HttpRouter& router, PubSubHandlers& handlers) {
    // POST /subscribe -- authenticated
    router.add_route("POST", "/subscribe",
        [&handlers](const HttpRequest& req, const std::vector<uint8_t>& body,
                    HttpSessionState* session) -> HttpResponse {
            return handlers.handle_subscribe(req, body, session);
        });

    // POST /unsubscribe -- authenticated
    router.add_route("POST", "/unsubscribe",
        [&handlers](const HttpRequest& req, const std::vector<uint8_t>& body,
                    HttpSessionState* session) -> HttpResponse {
            return handlers.handle_unsubscribe(req, body, session);
        });

    // GET /events -- public (auth via query param, not Bearer header)
    router.add_route("GET", "/events",
        [&handlers](const HttpRequest& req, const std::vector<uint8_t>& body,
                    HttpSessionState* session) -> HttpResponse {
            return handlers.handle_events_auth(req, body, session);
        }, /*public_route=*/true);
}

} // namespace chromatindb::relay::http
