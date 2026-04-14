#include "relay/http/handlers_query.h"
#include "relay/http/http_parser.h"
#include "relay/http/response_promise.h"
#include "relay/http/token_store.h"
#include "relay/core/request_router.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/translate/translator.h"
#include "relay/translate/type_registry.h"
#include "relay/util/offload_if_large.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chromatindb::relay::http {

// =============================================================================
// Query string parsing
// =============================================================================

std::optional<std::string> parse_query_param(std::string_view query, std::string_view key) {
    if (query.empty()) return std::nullopt;

    // Iterate over "&"-delimited pairs.
    size_t pos = 0;
    while (pos < query.size()) {
        auto amp = query.find('&', pos);
        auto segment = query.substr(pos, amp == std::string_view::npos ? amp : amp - pos);

        auto eq = segment.find('=');
        if (eq != std::string_view::npos) {
            auto k = segment.substr(0, eq);
            auto v = segment.substr(eq + 1);
            if (k == key) {
                return std::string(v);
            }
        }

        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return std::nullopt;
}

// =============================================================================
// Path parameter extraction
// =============================================================================

std::string_view extract_path_segment(std::string_view path, std::string_view prefix) {
    if (path.size() <= prefix.size()) return {};
    if (path.compare(0, prefix.size(), prefix) != 0) return {};
    auto remaining = path.substr(prefix.size());
    // Trim trailing slash if present.
    if (!remaining.empty() && remaining.back() == '/') {
        remaining = remaining.substr(0, remaining.size() - 1);
    }
    return remaining;
}

std::pair<std::string_view, std::string_view> extract_two_segments(
    std::string_view path, std::string_view prefix) {

    auto remaining = extract_path_segment(path, prefix);
    if (remaining.empty()) return {{}, {}};

    auto slash = remaining.find('/');
    if (slash == std::string_view::npos) return {remaining, {}};

    auto seg1 = remaining.substr(0, slash);
    auto seg2 = remaining.substr(slash + 1);
    // Trim trailing slash from seg2.
    if (!seg2.empty() && seg2.back() == '/') {
        seg2 = seg2.substr(0, seg2.size() - 1);
    }
    return {seg1, seg2};
}

// =============================================================================
// Hex validation
// =============================================================================

bool is_valid_hex32(std::string_view s) {
    if (s.size() != 64) return false;
    for (auto c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

// =============================================================================
// Forward query helper
// =============================================================================

namespace {

/// Default request timeout: 30 seconds.
static constexpr uint32_t DEFAULT_REQUEST_TIMEOUT_SECS = 30;

/// Encapsulate the translate -> UDS send -> await response -> translate pipeline.
///
/// All steps execute on the single event loop thread -- no strand needed.
///
/// 1. json_to_binary(query_json) -> {wire_type, payload}
/// 2. register_request with RequestRouter
/// 3. create ResponsePromise + TransportCodec::encode + send
/// 4. co_await ResponsePromise (timer on ioc executor)
/// 5. binary_to_json(response) -> JSON
/// 6. Return HttpResponse::json(200, result_json)
asio::awaitable<HttpResponse> forward_query(
    const nlohmann::json& query_json,
    uint64_t session_id,
    core::UdsMultiplexer& uds_mux,
    core::RequestRouter& router,
    ResponsePromiseMap& promises,
    asio::io_context& ioc,
    const uint32_t* request_timeout,
    asio::thread_pool* pool) {

    // 1. Translate JSON -> binary.
    // Query JSON is always small (sub-KB) -- size 0 ensures inline per D-08.
    auto result = co_await util::offload_if_large(*pool, ioc, 0,
        [&] { return translate::json_to_binary(query_json); });
    if (!result) {
        co_return HttpResponse::error(400, "translation_error", "failed to translate request");
    }

    // 2. Check UDS connection.
    if (!uds_mux.is_connected()) {
        co_return HttpResponse::error(502, "node_unavailable", "node connection not ready");
    }

    // 3. Register request for response routing.
    uint32_t relay_rid = router.register_request(session_id, 0, result->wire_type);

    // 4. Create ResponsePromise (timer on ioc executor) and register it.
    auto promise = promises.create_promise(relay_rid, ioc.get_executor());

    // 5. Encode transport envelope and send to node.
    auto transport_msg = wire::TransportCodec::encode(
        static_cast<chromatindb::wire::TransportMsgType>(result->wire_type),
        result->payload, relay_rid);
    if (!uds_mux.send(std::move(transport_msg))) {
        promises.remove(relay_rid);
        router.resolve_response(relay_rid);
        co_return HttpResponse::error(502, "send_failed", "failed to send to node");
    }

    // 6. Await response with timeout.
    uint32_t timeout_secs = request_timeout
        ? *request_timeout
        : DEFAULT_REQUEST_TIMEOUT_SECS;
    if (timeout_secs == 0) timeout_secs = DEFAULT_REQUEST_TIMEOUT_SECS;

    auto response = co_await promise->wait(std::chrono::seconds(timeout_secs));
    if (!response) {
        promises.remove(relay_rid);  // Clean up if still registered
        co_return HttpResponse::error(504, "timeout", "node did not respond in time");
    }

    // 7. Translate response binary -> JSON.
    auto response_json = co_await util::offload_if_large(*pool, ioc,
        response->payload.size(),
        [&] { return translate::binary_to_json(
            response->type, std::span<const uint8_t>(response->payload)); });
    if (!response_json) {
        co_return HttpResponse::error(502, "translation_error",
                                      "failed to translate node response");
    }

    co_return HttpResponse::json(200, *response_json);
}

/// Parse an optional uint64 query parameter, returning default on missing/invalid.
uint64_t parse_uint64_param(std::string_view query, std::string_view key, uint64_t default_val) {
    auto val = parse_query_param(query, key);
    if (!val) return default_val;
    try {
        return std::stoull(*val);
    } catch (...) {
        return default_val;
    }
}

/// Parse an optional uint32 query parameter, returning default on missing/invalid.
uint32_t parse_uint32_param(std::string_view query, std::string_view key, uint32_t default_val) {
    auto val = parse_query_param(query, key);
    if (!val) return default_val;
    try {
        auto v = std::stoul(*val);
        if (v > UINT32_MAX) return default_val;
        return static_cast<uint32_t>(v);
    } catch (...) {
        return default_val;
    }
}

} // namespace

// =============================================================================
// Route registration
// =============================================================================

void register_query_routes(HttpRouter& router, QueryHandlerDeps deps) {

    // Capture deps by copy (all are references/pointers, lightweight).
    auto& uds_mux = deps.uds_mux;
    auto& req_router = deps.router;
    auto& promises = deps.promises;
    auto& ioc = deps.ioc;
    auto* timeout = deps.request_timeout;
    auto* pool = deps.pool;

    // -------------------------------------------------------------------------
    // GET /list/{namespace}?since_seq=N&limit=N
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/list/",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            auto ns_hex = extract_path_segment(req.path, "/list/");
            if (!is_valid_hex32(ns_hex)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }

            uint64_t since_seq = parse_uint64_param(req.query, "since_seq", 0);
            uint32_t limit = parse_uint32_param(req.query, "limit", 100);

            nlohmann::json j = {
                {"type", "list_request"},
                {"namespace", std::string(ns_hex)},
                {"since_seq", std::to_string(since_seq)},
                {"limit", limit}
            };

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /stats/{namespace} (D-11, alias for namespace stats)
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/stats/",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            auto ns_hex = extract_path_segment(req.path, "/stats/");
            if (!is_valid_hex32(ns_hex)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }

            nlohmann::json j = {
                {"type", "namespace_stats_request"},
                {"namespace", std::string(ns_hex)}
            };

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /exists/{namespace}/{hash}
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/exists/",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            auto [ns_hex, hash_hex] = extract_two_segments(req.path, "/exists/");
            if (!is_valid_hex32(ns_hex)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }
            if (!is_valid_hex32(hash_hex)) {
                co_return HttpResponse::error(400, "invalid_hash",
                    "hash must be 64 hex characters");
            }

            nlohmann::json j = {
                {"type", "exists_request"},
                {"namespace", std::string(ns_hex)},
                {"hash", std::string(hash_hex)}
            };

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // POST /batch/exists
    // -------------------------------------------------------------------------
    router.add_async_route("POST", "/batch/exists",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest&, const std::vector<uint8_t>& body,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            nlohmann::json j;
            try {
                j = nlohmann::json::parse(body.begin(), body.end());
            } catch (const nlohmann::json::parse_error&) {
                co_return HttpResponse::error(400, "bad_json", "invalid JSON body");
            }

            // Validate required fields.
            if (!j.contains("namespace") || !j["namespace"].is_string()) {
                co_return HttpResponse::error(400, "missing_field", "namespace required");
            }
            if (!j.contains("hashes") || !j["hashes"].is_array()) {
                co_return HttpResponse::error(400, "missing_field", "hashes array required");
            }

            auto ns = j["namespace"].get<std::string>();
            if (!is_valid_hex32(ns)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }

            // Build translator-compatible JSON.
            nlohmann::json query = {
                {"type", "batch_exists_request"},
                {"namespace", ns},
                {"hashes", j["hashes"]}
            };

            co_return co_await forward_query(query, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /node-info
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/node-info",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest&, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            nlohmann::json j = {{"type", "node_info_request"}};

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /peer-info
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/peer-info",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest&, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            nlohmann::json j = {{"type", "peer_info_request"}};

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /storage-status
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/storage-status",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest&, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            nlohmann::json j = {{"type", "storage_status_request"}};

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /namespace-stats/{namespace} (D-18)
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/namespace-stats/",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            auto ns_hex = extract_path_segment(req.path, "/namespace-stats/");
            if (!is_valid_hex32(ns_hex)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }

            nlohmann::json j = {
                {"type", "namespace_stats_request"},
                {"namespace", std::string(ns_hex)}
            };

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /metadata/{namespace}/{hash}
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/metadata/",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            auto [ns_hex, hash_hex] = extract_two_segments(req.path, "/metadata/");
            if (!is_valid_hex32(ns_hex)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }
            if (!is_valid_hex32(hash_hex)) {
                co_return HttpResponse::error(400, "invalid_hash",
                    "hash must be 64 hex characters");
            }

            nlohmann::json j = {
                {"type", "metadata_request"},
                {"namespace", std::string(ns_hex)},
                {"hash", std::string(hash_hex)}
            };

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /delegations/{namespace}
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/delegations/",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            auto ns_hex = extract_path_segment(req.path, "/delegations/");
            if (!is_valid_hex32(ns_hex)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }

            nlohmann::json j = {
                {"type", "delegation_list_request"},
                {"namespace", std::string(ns_hex)}
            };

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /time-range/{namespace}?start=N&end=N&limit=N
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/time-range/",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            auto ns_hex = extract_path_segment(req.path, "/time-range/");
            if (!is_valid_hex32(ns_hex)) {
                co_return HttpResponse::error(400, "invalid_namespace",
                    "namespace must be 64 hex characters");
            }

            uint64_t start = parse_uint64_param(req.query, "start", 0);
            uint64_t end = parse_uint64_param(req.query, "end", UINT64_MAX);
            uint32_t limit = parse_uint32_param(req.query, "limit", 100);

            nlohmann::json j = {
                {"type", "time_range_request"},
                {"namespace", std::string(ns_hex)},
                {"since", std::to_string(start)},
                {"until", std::to_string(end)},
                {"limit", limit}
            };

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });

    // -------------------------------------------------------------------------
    // GET /namespace-list?after=HEX&limit=N
    // -------------------------------------------------------------------------
    router.add_async_route("GET", "/namespace-list",
        [&uds_mux, &req_router, &promises, &ioc, timeout, pool](
            const HttpRequest& req, const std::vector<uint8_t>&,
            HttpSessionState* session) -> asio::awaitable<HttpResponse> {

            nlohmann::json j = {{"type", "namespace_list_request"}};

            auto after = parse_query_param(req.query, "after");
            if (after && is_valid_hex32(*after)) {
                j["after_namespace"] = *after;
            }

            uint32_t limit = parse_uint32_param(req.query, "limit", 100);
            j["limit"] = limit;

            co_return co_await forward_query(j, session->session_id,
                uds_mux, req_router, promises, ioc, timeout, pool);
        });
}

} // namespace chromatindb::relay::http
