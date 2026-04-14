#pragma once

#include "relay/http/http_response.h"
#include "relay/http/http_router.h"

#include <asio.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace chromatindb::relay::core {
class SubscriptionTracker;
class UdsMultiplexer;
} // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

struct HttpRequest;
struct HttpSessionState;
class SseWriter;
class TokenStore;

/// Pub/Sub HTTP handlers: subscribe, unsubscribe, and SSE event stream.
///
/// POST /subscribe    -- Add namespaces to session's subscription set (D-22)
/// POST /unsubscribe  -- Remove namespaces from subscription set (D-23)
/// GET  /events       -- SSE notification stream (D-24, auth via query param)
///
/// Subscribe/unsubscribe forward to the node via UDS when a namespace's
/// reference count transitions (0->1 for subscribe, 1->0 for unsubscribe).
///
/// All handlers post to strand_ before accessing shared state
/// (SubscriptionTracker, UdsMultiplexer::send, TokenStore).
class PubSubHandlers {
public:
    using Strand = asio::strand<asio::io_context::executor_type>;

    PubSubHandlers(core::SubscriptionTracker& tracker,
                   core::UdsMultiplexer& uds,
                   TokenStore& token_store,
                   Strand& strand);

    /// POST /subscribe: parse JSON body, add to tracker, forward new to node.
    /// Async: posts to strand before SubscriptionTracker/UdsMultiplexer access.
    asio::awaitable<HttpResponse> handle_subscribe(const HttpRequest& req,
                                                    const std::vector<uint8_t>& body,
                                                    HttpSessionState* session);

    /// POST /unsubscribe: parse JSON body, remove from tracker, forward empty to node.
    /// Async: posts to strand before SubscriptionTracker/UdsMultiplexer access.
    asio::awaitable<HttpResponse> handle_unsubscribe(const HttpRequest& req,
                                                      const std::vector<uint8_t>& body,
                                                      HttpSessionState* session);

    /// GET /events?token=<token>: SSE event stream (long-lived connection).
    /// Async: posts to strand before TokenStore::lookup.
    asio::awaitable<HttpResponse> handle_events_auth(const HttpRequest& req,
                                                      const std::vector<uint8_t>& body,
                                                      HttpSessionState* session);

private:
    /// Encode namespace list as u16BE for node Subscribe/Unsubscribe wire format.
    /// Format: [u16BE count][ns:32][ns:32]...
    static std::vector<uint8_t> encode_namespace_list_u16be(
        const std::vector<std::array<uint8_t, 32>>& namespaces);

    /// Parse namespace hex strings from JSON body.
    /// Returns empty vector on parse error.
    static std::vector<std::array<uint8_t, 32>> parse_namespace_list(
        const std::vector<uint8_t>& body);

    core::SubscriptionTracker& tracker_;
    core::UdsMultiplexer& uds_;
    TokenStore& token_store_;
    Strand& strand_;
};

/// Register pub/sub routes on the HTTP router.
/// Routes:
///   POST /subscribe    -> handle_subscribe (auth required)
///   POST /unsubscribe  -> handle_unsubscribe (auth required)
///   GET  /events       -> handle_events_auth (public -- auth via query param)
void register_pubsub_routes(HttpRouter& router, PubSubHandlers& handlers);

} // namespace chromatindb::relay::http
