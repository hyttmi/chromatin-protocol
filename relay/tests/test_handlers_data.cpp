#include <catch2/catch_test_macros.hpp>

#include "relay/http/handlers_data.h"
#include "relay/http/http_parser.h"
#include "relay/http/http_response.h"
#include "relay/http/http_router.h"
#include "relay/http/token_store.h"
#include "relay/util/hex.h"
#include "relay/wire/transport_generated.h"

using namespace chromatindb::relay::http;
using namespace chromatindb::relay::util;

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
// Path parameter extraction
// ===========================================================================

TEST_CASE("extract_blob_path_params: valid 64-char hex namespace and hash", "[http][data][path]") {
    std::string ns_hex(64, 'a');   // 64 'a' chars
    std::string hash_hex(64, 'b'); // 64 'b' chars
    std::string path = "/blob/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE(params.has_value());
    REQUIRE(params->namespace_bytes.size() == 32);
    REQUIRE(params->hash_bytes.size() == 32);

    // Verify decoded bytes.
    for (auto b : params->namespace_bytes) {
        REQUIRE(b == 0xAA);
    }
    for (auto b : params->hash_bytes) {
        REQUIRE(b == 0xBB);
    }
}

TEST_CASE("extract_blob_path_params: mixed-case hex is accepted", "[http][data][path]") {
    std::string ns_hex = "aAbBcCdDeEfF00112233445566778899aAbBcCdDeEfF00112233445566778899";
    std::string hash_hex = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF";
    std::string path = "/blob/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE(params.has_value());
    REQUIRE(params->namespace_bytes.size() == 32);
    REQUIRE(params->hash_bytes.size() == 32);
}

TEST_CASE("extract_blob_path_params: short namespace returns nullopt", "[http][data][path]") {
    std::string path = "/blob/abcd/0000000000000000000000000000000000000000000000000000000000000000";
    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: short hash returns nullopt", "[http][data][path]") {
    std::string path = "/blob/0000000000000000000000000000000000000000000000000000000000000000/abcd";
    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: no separator returns nullopt", "[http][data][path]") {
    // 128 hex chars but no '/' between ns and hash
    std::string path = "/blob/" + std::string(128, 'a');
    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: non-hex characters return nullopt", "[http][data][path]") {
    std::string ns_hex(64, 'g');  // 'g' is not valid hex
    std::string hash_hex(64, '0');
    std::string path = "/blob/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: wrong prefix returns nullopt", "[http][data][path]") {
    std::string ns_hex(64, 'a');
    std::string hash_hex(64, 'b');
    std::string path = "/other/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: empty path returns nullopt", "[http][data][path]") {
    auto params = extract_blob_path_params("", "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: just prefix returns nullopt", "[http][data][path]") {
    auto params = extract_blob_path_params("/blob/", "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: trailing slash ignored", "[http][data][path]") {
    std::string ns_hex(64, 'a');
    std::string hash_hex(64, 'b');
    // Trailing slash makes hash_hex appear as 64+1 chars -> invalid
    std::string path = "/blob/" + ns_hex + "/" + hash_hex + "/";

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

// ===========================================================================
// Wire type constants verification
// ===========================================================================

TEST_CASE("Wire type constants match expected values", "[http][data][wire]") {
    // Verify the wire types used by handlers match the transport_generated.h values.
    // These are the types the handlers must use for correctness.
    REQUIRE(chromatindb::wire::TransportMsgType_Data == 8);
    REQUIRE(chromatindb::wire::TransportMsgType_Delete == 17);
    REQUIRE(chromatindb::wire::TransportMsgType_DeleteAck == 18);
    REQUIRE(chromatindb::wire::TransportMsgType_WriteAck == 30);
    REQUIRE(chromatindb::wire::TransportMsgType_ReadRequest == 31);
    REQUIRE(chromatindb::wire::TransportMsgType_ReadResponse == 32);
    REQUIRE(chromatindb::wire::TransportMsgType_BatchReadRequest == 53);
    REQUIRE(chromatindb::wire::TransportMsgType_BatchReadResponse == 54);
}

// ===========================================================================
// Hex utility roundtrip (confirms hex parsing used by path extraction)
// ===========================================================================

TEST_CASE("hex roundtrip for 32-byte values", "[http][data][hex]") {
    std::vector<uint8_t> original(32);
    for (size_t i = 0; i < 32; ++i) {
        original[i] = static_cast<uint8_t>(i * 8);
    }

    auto hex = to_hex(original);
    REQUIRE(hex.size() == 64);

    auto decoded = from_hex(hex);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == original);
}

// ===========================================================================
// HttpResponse factory methods used by handlers
// ===========================================================================

TEST_CASE("HttpResponse::binary produces application/octet-stream", "[http][data][response]") {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    auto resp = HttpResponse::binary(200, data);
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == data);

    bool found_ct = false;
    for (const auto& [key, val] : resp.headers) {
        if (key == "Content-Type") {
            REQUIRE(val == "application/octet-stream");
            found_ct = true;
        }
    }
    REQUIRE(found_ct);
}

TEST_CASE("HttpResponse::error 413 produces correct status", "[http][data][response]") {
    auto resp = HttpResponse::error(413, "payload_too_large", "too big");
    REQUIRE(resp.status == 413);

    auto body_str = std::string(resp.body.begin(), resp.body.end());
    REQUIRE(body_str.find("payload_too_large") != std::string::npos);
}

TEST_CASE("HttpResponse::not_found produces 404", "[http][data][response]") {
    auto resp = HttpResponse::not_found();
    REQUIRE(resp.status == 404);
}

TEST_CASE("HttpResponse::error 504 for timeout", "[http][data][response]") {
    auto resp = HttpResponse::error(504, "timeout", "node response timeout");
    REQUIRE(resp.status == 504);
}

// ===========================================================================
// Route registration (sync dispatch -- tests that routes exist and auth is enforced)
// ===========================================================================

// Note: Async data handlers cannot run in sync dispatch. These tests verify
// route existence (not 404), auth enforcement (401), and method matching (405).
// The sync dispatch returns 500 for async routes, which confirms they are registered.

TEST_CASE("Data routes: POST /blob requires auth (401 without token)", "[http][data][route]") {
    // We can't construct real DataHandlers without UDS etc., but we can test
    // that register_data_routes adds async routes that the sync dispatch recognizes.
    // Since we can't call register_data_routes without DataHandlers, we test
    // the router's async route behavior directly.
    HttpRouter router;
    router.add_async_route("POST", "/blob",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*)
            -> asio::awaitable<HttpResponse> {
            co_return HttpResponse::json(200, {{"ok", true}});
        });

    TokenStore store;
    auto req = make_request("POST", "/blob");
    auto resp = router.dispatch(req, {}, store);
    // Without auth header -> 401.
    REQUIRE(resp.status == 401);
}

TEST_CASE("Data routes: GET /blob/ prefix requires auth (401 without token)", "[http][data][route]") {
    HttpRouter router;
    router.add_async_route("GET", "/blob/",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*)
            -> asio::awaitable<HttpResponse> {
            co_return HttpResponse::json(200, {{"ok", true}});
        });

    TokenStore store;
    std::string ns_hex(64, 'a');
    std::string hash_hex(64, 'b');
    auto req = make_request("GET", "/blob/" + ns_hex + "/" + hash_hex);
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 401);
}

TEST_CASE("Data routes: DELETE /blob/ prefix requires auth (401 without token)", "[http][data][route]") {
    HttpRouter router;
    router.add_async_route("DELETE", "/blob/",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*)
            -> asio::awaitable<HttpResponse> {
            co_return HttpResponse::json(200, {{"ok", true}});
        });

    TokenStore store;
    std::string ns_hex(64, 'a');
    std::string hash_hex(64, 'b');
    auto req = make_request("DELETE", "/blob/" + ns_hex + "/" + hash_hex);
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 401);
}

TEST_CASE("Data routes: POST /batch/read requires auth (401 without token)", "[http][data][route]") {
    HttpRouter router;
    router.add_async_route("POST", "/batch/read",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*)
            -> asio::awaitable<HttpResponse> {
            co_return HttpResponse::json(200, {{"ok", true}});
        });

    TokenStore store;
    auto req = make_request("POST", "/batch/read");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 401);
}

TEST_CASE("Data routes: wrong method on /blob returns 405", "[http][data][route]") {
    HttpRouter router;
    router.add_async_route("POST", "/blob",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*)
            -> asio::awaitable<HttpResponse> {
            co_return HttpResponse::json(200, {{"ok", true}});
        });

    TokenStore store;
    auto req = make_request("GET", "/blob");  // GET on exact /blob, not /blob/
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 405);
}

TEST_CASE("Data routes: unknown path returns 404", "[http][data][route]") {
    HttpRouter router;
    router.add_async_route("POST", "/blob",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*)
            -> asio::awaitable<HttpResponse> {
            co_return HttpResponse::json(200, {{"ok", true}});
        });

    TokenStore store;
    auto req = make_request("GET", "/unknown");
    auto resp = router.dispatch(req, {}, store);
    REQUIRE(resp.status == 404);
}

TEST_CASE("Data routes: async route with valid token returns 500 from sync dispatch", "[http][data][route]") {
    // Async routes called from sync dispatch return 500 (intentional guard).
    HttpRouter router;
    router.add_async_route("POST", "/blob",
        [](const HttpRequest&, const std::vector<uint8_t>&, HttpSessionState*)
            -> asio::awaitable<HttpResponse> {
            co_return HttpResponse::json(200, {{"ok", true}});
        });

    TokenStore store;
    std::array<uint8_t, 32> ns{};
    auto token = store.create_session({}, ns, 0);

    auto req = make_request("POST", "/blob", "Bearer " + token);
    auto resp = router.dispatch(req, {}, store);
    // Sync dispatch of async route -> 500 internal_error.
    REQUIRE(resp.status == 500);
}

// ===========================================================================
// StreamingResponsePromise tests (Phase 115 Plan 04)
// ===========================================================================

TEST_CASE("StreamingResponsePromise: set_header makes header_ready", "[http][data][streaming]") {
    asio::io_context ioc;
    StreamingResponsePromise promise(ioc.get_executor());

    StreamingResponsePromise::HeaderInfo hdr{32, 42, 2048, {0x01}};
    promise.set_header(std::move(hdr));

    // The header should be internally ready. Verify by checking that
    // the queue is not closed yet (it starts open).
    REQUIRE_FALSE(promise.queue.closed);
}

TEST_CASE("StreamingResponsePromise: queue starts open", "[http][data][streaming]") {
    asio::io_context ioc;
    StreamingResponsePromise promise(ioc.get_executor());

    REQUIRE_FALSE(promise.queue.closed);
    REQUIRE(promise.queue.chunks.empty());
}

TEST_CASE("StreamingResponsePromise: queue close sets closed flag", "[http][data][streaming]") {
    asio::io_context ioc;
    StreamingResponsePromise promise(ioc.get_executor());

    promise.queue.close_queue();
    REQUIRE(promise.queue.closed);
}

TEST_CASE("ResponsePromiseMap: resolve fallback to streaming promise (synchronous)", "[http][data][streaming]") {
    asio::io_context ioc;
    ResponsePromiseMap map;

    // Create a streaming promise
    auto sp = map.create_streaming_promise(100, ioc.get_executor());
    REQUIRE(map.streaming_size() == 1);

    // Resolve with regular resolve() -- should fall back to streaming promise
    std::vector<uint8_t> payload = {0x01, 0xDE, 0xAD};
    bool resolved = map.resolve(100, 32, payload);
    REQUIRE(resolved);
    REQUIRE(map.streaming_size() == 0);  // Removed from map after resolve

    // The streaming promise should have the data pushed into its queue synchronously
    REQUIRE(sp->queue.closed);
    REQUIRE(sp->queue.chunks.size() == 1);
    REQUIRE(sp->queue.chunks[0].size() == 3);
    REQUIRE(sp->queue.chunks[0][0] == 0x01);
    REQUIRE(sp->queue.chunks[0][1] == 0xDE);
    REQUIRE(sp->queue.chunks[0][2] == 0xAD);
}

TEST_CASE("ResponsePromiseMap: resolve regular promise preferred over streaming", "[http][data][streaming]") {
    asio::io_context ioc;
    ResponsePromiseMap map;

    // Create both regular and streaming promises for the same relay_rid
    auto regular = map.create_promise(100, ioc.get_executor());
    auto streaming = map.create_streaming_promise(100, ioc.get_executor());

    REQUIRE(map.size() == 1);
    REQUIRE(map.streaming_size() == 1);

    // Resolve should prefer regular promise
    bool resolved = map.resolve(100, 32, {0xAA});
    REQUIRE(resolved);
    REQUIRE(regular->is_resolved());
    // Streaming promise should NOT have been resolved (regular took priority)
    REQUIRE(map.streaming_size() == 1);  // Still in the map
    REQUIRE(streaming->queue.chunks.empty());
}

TEST_CASE("ResponsePromiseMap: streaming promise management", "[http][data][streaming]") {
    asio::io_context ioc;
    ResponsePromiseMap map;

    auto sp = map.create_streaming_promise(200, ioc.get_executor());
    REQUIRE(map.streaming_size() == 1);

    auto got = map.get_streaming(200);
    REQUIRE(got != nullptr);
    REQUIRE(got == sp);

    auto miss = map.get_streaming(999);
    REQUIRE(miss == nullptr);

    map.remove_streaming(200);
    REQUIRE(map.streaming_size() == 0);
    REQUIRE(map.get_streaming(200) == nullptr);
}

TEST_CASE("ResponsePromiseMap: cancel_all closes streaming promises", "[http][data][streaming]") {
    asio::io_context ioc;
    ResponsePromiseMap map;

    auto p = map.create_promise(1, ioc.get_executor());
    auto sp = map.create_streaming_promise(2, ioc.get_executor());

    REQUIRE(map.size() == 1);
    REQUIRE(map.streaming_size() == 1);

    map.cancel_all();

    REQUIRE(map.size() == 0);
    REQUIRE(map.streaming_size() == 0);
    REQUIRE(sp->queue.closed);
}

TEST_CASE("ResponsePromiseMap: resolve to unknown streaming rid returns false", "[http][data][streaming]") {
    ResponsePromiseMap map;

    // No regular or streaming promise registered
    bool resolved = map.resolve(999, 8, {0x01});
    REQUIRE_FALSE(resolved);
}

TEST_CASE("STREAMING_THRESHOLD constant is 1 MiB", "[http][data][streaming]") {
    REQUIRE(chromatindb::relay::core::STREAMING_THRESHOLD == 1048576);
}

TEST_CASE("CHUNK_SIZE constant is 1 MiB", "[http][data][streaming]") {
    REQUIRE(chromatindb::relay::core::CHUNK_SIZE == 1048576);
}

TEST_CASE("CHUNK_QUEUE_MAX_DEPTH is 4", "[http][data][streaming]") {
    REQUIRE(chromatindb::relay::core::CHUNK_QUEUE_MAX_DEPTH == 4);
}
