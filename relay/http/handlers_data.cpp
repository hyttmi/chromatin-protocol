#include "relay/http/handlers_data.h"
#include "relay/http/http_connection.h"
#include "relay/http/http_parser.h"
#include "relay/http/http_router.h"
#include "relay/http/token_store.h"
#include "relay/core/chunked_stream.h"
#include "relay/core/request_router.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/core/write_tracker.h"
#include "relay/translate/translator.h"
#include "relay/util/hex.h"
#include "relay/util/offload_if_large.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

namespace chromatindb::relay::http {

// ---------------------------------------------------------------------------
// Path parameter extraction
// ---------------------------------------------------------------------------

std::optional<BlobPathParams> extract_blob_path_params(std::string_view path,
                                                        std::string_view prefix) {
    // path must start with prefix, e.g. "/blob/" + "{ns}/{hash}"
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }

    auto remainder = path.substr(prefix.size());

    // Find separator between namespace and hash.
    auto sep = remainder.find('/');
    if (sep == std::string_view::npos) {
        return std::nullopt;
    }

    auto ns_hex = remainder.substr(0, sep);
    auto hash_hex = remainder.substr(sep + 1);

    // Both must be exactly 64 hex chars (32 bytes each).
    if (ns_hex.size() != 64 || hash_hex.size() != 64) {
        return std::nullopt;
    }

    auto ns_bytes = util::from_hex(ns_hex);
    if (!ns_bytes) return std::nullopt;

    auto hash_bytes = util::from_hex(hash_hex);
    if (!hash_bytes) return std::nullopt;

    return BlobPathParams{std::move(*ns_bytes), std::move(*hash_bytes)};
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

DataHandlers::DataHandlers(core::UdsMultiplexer& uds_mux,
                           core::RequestRouter& router,
                           ResponsePromiseMap& promises,
                           core::WriteTracker& write_tracker,
                           const uint32_t& max_blob_size,
                           const uint32_t& request_timeout,
                           asio::io_context& ioc,
                           asio::thread_pool& pool)
    : uds_mux_(uds_mux)
    , router_(router)
    , promises_(promises)
    , write_tracker_(write_tracker)
    , max_blob_size_(max_blob_size)
    , request_timeout_(request_timeout)
    , ioc_(ioc)
    , pool_(pool) {}

// ---------------------------------------------------------------------------
// Common: send transport message and co_await UDS response
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<ResponseData>> DataHandlers::send_and_await(
    std::vector<uint8_t> transport_msg, uint32_t relay_rid) {

    // Single-threaded: use ioc executor for the promise timer.
    auto promise = promises_.create_promise(relay_rid, ioc_.get_executor());

    // Send via UDS.
    bool sent = uds_mux_.send(std::move(transport_msg));
    if (!sent) {
        promises_.remove(relay_rid);
        co_return std::nullopt;
    }

    // Await response with timeout.
    auto timeout_sec = request_timeout_;
    if (timeout_sec == 0) timeout_sec = 10;  // Default fallback.
    auto result = co_await promise->wait(std::chrono::seconds(timeout_sec));

    // Cleanup: promise may not have been removed if it timed out.
    promises_.remove(relay_rid);
    co_return result;
}

// ---------------------------------------------------------------------------
// POST /blob -- blob write
// ---------------------------------------------------------------------------

asio::awaitable<HttpResponse> DataHandlers::handle_blob_write(
    const HttpRequest& req, const std::vector<uint8_t>& body,
    HttpSessionState* session) {

    // 1. Body is the raw FlatBuffer-encoded Data blob. Must be non-empty.
    if (body.empty()) {
        co_return HttpResponse::error(400, "empty_body", "request body required");
    }

    // 2. Size limit check (FEAT-02). Single-threaded: plain dereference.
    if (max_blob_size_ > 0 && body.size() > max_blob_size_) {
        co_return HttpResponse::error(413, "payload_too_large",
            "body size " + std::to_string(body.size()) + " exceeds limit " + std::to_string(max_blob_size_));
    }

    // 3. Register request with RequestRouter.
    //    client_rid=0 (HTTP has no client-side request ID), type=8 (Data).
    uint32_t relay_rid = router_.register_request(
        session->session_id, 0, 8);

    // 4. Encode via TransportCodec.
    auto transport_msg = wire::TransportCodec::encode(
        chromatindb::wire::TransportMsgType_Data,
        std::span<const uint8_t>(body), relay_rid);

    // 5. Send and await UDS response.
    auto result = co_await send_and_await(std::move(transport_msg), relay_rid);
    if (!result) {
        co_return HttpResponse::error(504, "timeout", "node response timeout");
    }

    // 6. Expect WriteAck (type 30).
    if (result->type != 30) {
        spdlog::warn("http: blob write got unexpected type={} (expected WriteAck=30)",
                     result->type);
        co_return HttpResponse::error(502, "unexpected_response",
            "unexpected response type from node");
    }

    // 7. Source exclusion: record blob_hash in WriteTracker.
    //    WriteAck payload: [hash:32][seq_num:8BE][status:1] = 41 bytes.
    if (result->payload.size() >= 32) {
        core::BlobHash32 blob_hash{};
        std::memcpy(blob_hash.data(), result->payload.data(), 32);
        write_tracker_.record(blob_hash, session->session_id);
    }

    // 8. Translate WriteAck binary -> JSON.
    auto json_opt = co_await util::offload_if_large(pool_, ioc_,
        result->payload.size(),
        [&] { return translate::binary_to_json(result->type,
            std::span<const uint8_t>(result->payload)); });
    if (!json_opt) {
        co_return HttpResponse::error(502, "decode_error",
            "failed to decode WriteAck response");
    }

    co_return HttpResponse::json(200, *json_opt);
}

// ---------------------------------------------------------------------------
// GET /blob/{ns}/{hash} -- blob read
// ---------------------------------------------------------------------------

asio::awaitable<HttpResponse> DataHandlers::handle_blob_read(
    const HttpRequest& req, const std::vector<uint8_t>& body,
    HttpSessionState* session) {

    // 1. Extract namespace and hash from path.
    auto params = extract_blob_path_params(req.path, "/blob/");
    if (!params) {
        co_return HttpResponse::error(400, "invalid_path",
            "expected /blob/{namespace_hex_64}/{hash_hex_64}");
    }

    // 2. Build ReadRequest binary: [namespace:32][hash:32] = 64 bytes.
    //    Wire type = 31 (ReadRequest).
    std::vector<uint8_t> payload(64);
    std::memcpy(payload.data(), params->namespace_bytes.data(), 32);
    std::memcpy(payload.data() + 32, params->hash_bytes.data(), 32);

    // 3. Register request.
    uint32_t relay_rid = router_.register_request(
        session->session_id, 0, 31);

    // 4. Encode and send via UDS.
    auto transport_msg = wire::TransportCodec::encode(
        chromatindb::wire::TransportMsgType_ReadRequest,
        std::span<const uint8_t>(payload), relay_rid);

    auto result = co_await send_and_await(std::move(transport_msg), relay_rid);
    if (!result) {
        co_return HttpResponse::error(504, "timeout", "node response timeout");
    }

    // 5. Expect ReadResponse (type 32).
    if (result->type != 32) {
        spdlog::warn("http: blob read got unexpected type={} (expected ReadResponse=32)",
                     result->type);
        co_return HttpResponse::error(502, "unexpected_response",
            "unexpected response type from node");
    }

    // 6. Check status byte. ReadResponse format: [status:1][FlatBuffer blob data...]
    //    status 0x01 = found, anything else = not found.
    if (result->payload.empty()) {
        co_return HttpResponse::not_found();
    }

    uint8_t status = result->payload[0];
    if (status != 0x01) {
        co_return HttpResponse::not_found();
    }

    // 7. Return raw binary blob data (skip status byte).
    //    Application/octet-stream, no JSON wrapping.
    std::vector<uint8_t> blob_data(
        result->payload.begin() + 1, result->payload.end());
    co_return HttpResponse::binary(200, std::move(blob_data));
}

// ---------------------------------------------------------------------------
// DELETE /blob/{ns}/{hash} -- blob delete (tombstone)
// ---------------------------------------------------------------------------

asio::awaitable<HttpResponse> DataHandlers::handle_blob_delete(
    const HttpRequest& req, const std::vector<uint8_t>& body,
    HttpSessionState* session) {

    // 1. Path must have namespace and hash (for URL context).
    auto params = extract_blob_path_params(req.path, "/blob/");
    if (!params) {
        co_return HttpResponse::error(400, "invalid_path",
            "expected /blob/{namespace_hex_64}/{hash_hex_64}");
    }

    // 2. Body is the raw FlatBuffer-encoded tombstone blob. Must be non-empty.
    if (body.empty()) {
        co_return HttpResponse::error(400, "empty_body",
            "tombstone blob body required");
    }

    // 3. Register request. Wire type = 17 (Delete).
    uint32_t relay_rid = router_.register_request(
        session->session_id, 0, 17);

    // 4. Encode via TransportCodec.
    auto transport_msg = wire::TransportCodec::encode(
        chromatindb::wire::TransportMsgType_Delete,
        std::span<const uint8_t>(body), relay_rid);

    // 5. Send and await.
    auto result = co_await send_and_await(std::move(transport_msg), relay_rid);
    if (!result) {
        co_return HttpResponse::error(504, "timeout", "node response timeout");
    }

    // 6. Expect DeleteAck (type 18).
    if (result->type != 18) {
        spdlog::warn("http: blob delete got unexpected type={} (expected DeleteAck=18)",
                     result->type);
        co_return HttpResponse::error(502, "unexpected_response",
            "unexpected response type from node");
    }

    // 7. Source exclusion: record blob_hash in WriteTracker.
    //    DeleteAck payload: [hash:32][seq_num:8BE][status:1] = 41 bytes.
    if (result->payload.size() >= 32) {
        core::BlobHash32 blob_hash{};
        std::memcpy(blob_hash.data(), result->payload.data(), 32);
        write_tracker_.record(blob_hash, session->session_id);
    }

    // 8. Translate DeleteAck binary -> JSON.
    auto json_opt = co_await util::offload_if_large(pool_, ioc_,
        result->payload.size(),
        [&] { return translate::binary_to_json(result->type,
            std::span<const uint8_t>(result->payload)); });
    if (!json_opt) {
        co_return HttpResponse::error(502, "decode_error",
            "failed to decode DeleteAck response");
    }

    co_return HttpResponse::json(200, *json_opt);
}

// ---------------------------------------------------------------------------
// POST /batch/read -- batch blob read
// ---------------------------------------------------------------------------

asio::awaitable<HttpResponse> DataHandlers::handle_batch_read(
    const HttpRequest& req, const std::vector<uint8_t>& body,
    HttpSessionState* session) {

    // 1. Parse JSON body.
    nlohmann::json request_json;
    try {
        request_json = nlohmann::json::parse(body.begin(), body.end());
    } catch (const nlohmann::json::parse_error&) {
        co_return HttpResponse::error(400, "bad_json", "invalid JSON body");
    }

    // 2. Validate required fields: namespace, hashes.
    if (!request_json.contains("namespace") || !request_json["namespace"].is_string()) {
        co_return HttpResponse::error(400, "missing_field", "namespace field required");
    }
    if (!request_json.contains("hashes") || !request_json["hashes"].is_array()) {
        co_return HttpResponse::error(400, "missing_field", "hashes array required");
    }

    // 3. Build BatchReadRequest JSON for translator (json_to_binary handles encoding).
    //    Type must be "batch_read_request".
    nlohmann::json translate_input;
    translate_input["type"] = "batch_read_request";
    translate_input["namespace"] = request_json["namespace"];
    translate_input["hashes"] = request_json["hashes"];
    if (request_json.contains("cap_bytes") && request_json["cap_bytes"].is_number()) {
        translate_input["max_bytes"] = request_json["cap_bytes"];
    }

    // 4. Translate JSON -> binary via translator.
    auto tr = co_await util::offload_if_large(pool_, ioc_, body.size(),
        [&] { return translate::json_to_binary(translate_input); });
    if (!tr) {
        co_return HttpResponse::error(400, "translate_error",
            "failed to encode batch read request");
    }

    // 5. Register request.
    uint32_t relay_rid = router_.register_request(
        session->session_id, 0, tr->wire_type);

    // 6. Encode transport message and send.
    auto transport_msg = wire::TransportCodec::encode(
        static_cast<chromatindb::wire::TransportMsgType>(tr->wire_type),
        std::span<const uint8_t>(tr->payload), relay_rid);

    auto result = co_await send_and_await(std::move(transport_msg), relay_rid);
    if (!result) {
        co_return HttpResponse::error(504, "timeout", "node response timeout");
    }

    // 7. Expect BatchReadResponse (type 54).
    if (result->type != 54) {
        spdlog::warn("http: batch read got unexpected type={} (expected BatchReadResponse=54)",
                     result->type);
        co_return HttpResponse::error(502, "unexpected_response",
            "unexpected response type from node");
    }

    // 8. Translate BatchReadResponse binary -> JSON.
    //    Translator produces JSON with base64-encoded blobs.
    auto json_opt = co_await util::offload_if_large(pool_, ioc_,
        result->payload.size(),
        [&] { return translate::binary_to_json(result->type,
            std::span<const uint8_t>(result->payload)); });
    if (!json_opt) {
        co_return HttpResponse::error(502, "decode_error",
            "failed to decode BatchReadResponse");
    }

    co_return HttpResponse::json(200, *json_opt);
}

// ---------------------------------------------------------------------------
// POST /blob streaming -- incremental HTTP-to-UDS forwarding
// ---------------------------------------------------------------------------

asio::awaitable<HttpResponse> DataHandlers::handle_blob_write_streaming(
    const HttpRequest& req, HttpConnection& conn, HttpSessionState* session) {

    // 1. Size limit check
    if (max_blob_size_ > 0 && req.content_length > max_blob_size_) {
        // Must drain body to keep connection clean for keep-alive
        std::vector<uint8_t> discard;
        co_await conn.read_body(req.content_length, discard);
        co_return HttpResponse::error(413, "payload_too_large",
            "body size " + std::to_string(req.content_length) +
            " exceeds limit " + std::to_string(max_blob_size_));
    }

    // 2. Register request with RequestRouter
    uint32_t relay_rid = router_.register_request(session->session_id, 0, 8);

    // 3. Create promise BEFORE sending (same pattern as send_and_await)
    auto promise = promises_.create_promise(relay_rid, ioc_.get_executor());

    // 4. Begin chunked UDS send -- get a ChunkQueue to push HTTP chunks into.
    // The raw HTTP body IS the FlatBuffer blob data. The chunked header carries
    // type=8 (Data) and request_id. The node reassembles chunks and dispatches
    // to message_cb_ with type=Data and the full payload.
    auto send_queue = uds_mux_.send_chunked_stream(
        8, relay_rid, static_cast<uint64_t>(req.content_length));
    if (!send_queue) {
        promises_.remove(relay_rid);
        co_return HttpResponse::error(502, "uds_down", "node connection unavailable");
    }

    // 5. Read HTTP body in 1 MiB chunks and push each to UDS send queue.
    // This is the core streaming: each chunk flows HTTP -> ChunkQueue -> drain_send_queue
    // -> send_encrypted (AEAD per chunk) -> UDS wire. Relay holds at most 1 chunk.
    bool read_ok = co_await conn.read_body_chunked(
        req.content_length, core::CHUNK_SIZE,
        [&send_queue](std::span<const uint8_t> chunk) -> asio::awaitable<bool> {
            // Copy chunk data and push to UDS send queue
            std::vector<uint8_t> chunk_vec(chunk.begin(), chunk.end());
            co_return co_await send_queue->push(std::move(chunk_vec));
        });

    // 6. Close the send queue to signal end-of-data.
    // drain_send_queue will send the zero-length sentinel after consuming all chunks.
    send_queue->close_queue();

    if (!read_ok) {
        promises_.remove(relay_rid);
        co_return HttpResponse::error(400, "read_error", "failed to read request body");
    }

    // 7. Await WriteAck response from node
    auto timeout_sec = request_timeout_;
    if (timeout_sec == 0) timeout_sec = 10;
    auto result = co_await promise->wait(std::chrono::seconds(timeout_sec));
    promises_.remove(relay_rid);

    if (!result) {
        co_return HttpResponse::error(504, "timeout", "node response timeout");
    }

    // 8. Expect WriteAck (type 30)
    if (result->type != 30) {
        co_return HttpResponse::error(502, "unexpected_response",
            "unexpected response type from node");
    }

    // 9. Source exclusion tracking
    if (result->payload.size() >= 32) {
        core::BlobHash32 blob_hash{};
        std::memcpy(blob_hash.data(), result->payload.data(), 32);
        write_tracker_.record(blob_hash, session->session_id);
    }

    // 10. Translate WriteAck -> JSON response
    auto json_opt = co_await util::offload_if_large(pool_, ioc_,
        result->payload.size(),
        [&] { return translate::binary_to_json(result->type,
            std::span<const uint8_t>(result->payload)); });
    if (!json_opt) {
        co_return HttpResponse::error(502, "decode_error", "failed to decode WriteAck");
    }

    co_return HttpResponse::json(200, *json_opt);
}

// ---------------------------------------------------------------------------
// GET /blob/{ns}/{hash} streaming -- UDS chunked -> HTTP chunked-TE download
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<HttpResponse>>
DataHandlers::handle_blob_read_streaming(
    const HttpRequest& req, HttpConnection& conn, HttpSessionState* session) {

    // 1. Extract namespace and hash from path
    auto params = extract_blob_path_params(req.path, "/blob/");
    if (!params) {
        co_return HttpResponse::error(400, "invalid_path",
            "expected /blob/{namespace_hex_64}/{hash_hex_64}");
    }

    // 2. Build ReadRequest payload: [namespace:32][hash:32] = 64 bytes
    std::vector<uint8_t> payload(64);
    std::memcpy(payload.data(), params->namespace_bytes.data(), 32);
    std::memcpy(payload.data() + 32, params->hash_bytes.data(), 32);

    // 3. Register request
    uint32_t relay_rid = router_.register_request(session->session_id, 0, 31);

    // 4. Create STREAMING promise (not regular promise).
    // When the node responds with a chunked ReadResponse, read_loop() will
    // detect this streaming promise and push chunks to the ChunkQueue instead
    // of reassembling the full blob.
    auto streaming_promise = promises_.create_streaming_promise(
        relay_rid, ioc_.get_executor());

    // 5. Send ReadRequest via regular UDS send (small message, 64 bytes)
    auto transport_msg = wire::TransportCodec::encode(
        chromatindb::wire::TransportMsgType_ReadRequest,
        std::span<const uint8_t>(payload), relay_rid);
    bool sent = uds_mux_.send(std::move(transport_msg));
    if (!sent) {
        promises_.remove_streaming(relay_rid);
        co_return HttpResponse::error(502, "uds_down", "node connection unavailable");
    }

    // 6. Wait for chunked header (contains status byte in extra_metadata)
    // Per Pitfall 4: check status BEFORE committing to 200 + chunked-TE.
    auto timeout_sec = request_timeout_;
    if (timeout_sec == 0) timeout_sec = 10;
    auto hdr = co_await streaming_promise->wait_header(
        std::chrono::seconds(timeout_sec));

    if (!hdr) {
        promises_.remove_streaming(relay_rid);
        co_return HttpResponse::error(504, "timeout", "node response timeout");
    }

    // 7. Verify response type = ReadResponse (32)
    if (hdr->type != 32) {
        streaming_promise->queue.close_queue();
        promises_.remove_streaming(relay_rid);
        co_return HttpResponse::error(502, "unexpected_response",
            "unexpected response type from node");
    }

    // 8. Check status byte (per Pitfall 4 from research).
    // The node's send_message_chunked() can include the status byte as extra_metadata
    // in the chunked header. If extra_metadata is empty, the status byte is the
    // first byte of the first data chunk.
    uint8_t status = 0;
    std::vector<uint8_t> leftover;  // Remaining bytes from first chunk after status byte

    if (!hdr->extra_metadata.empty()) {
        status = hdr->extra_metadata[0];
    } else {
        // Read first chunk to get status byte
        auto first_chunk = co_await streaming_promise->queue.pop();
        if (!first_chunk || first_chunk->empty()) {
            promises_.remove_streaming(relay_rid);
            co_return HttpResponse::not_found();
        }
        status = (*first_chunk)[0];
        if (first_chunk->size() > 1) {
            leftover.assign(first_chunk->begin() + 1, first_chunk->end());
        }
    }

    if (status != 0x01) {
        // Blob not found -- drain remaining chunks and return 404
        streaming_promise->queue.close_queue();
        promises_.remove_streaming(relay_rid);
        co_return HttpResponse::not_found();
    }

    // 9. Small blob optimization: if total_size < STREAMING_THRESHOLD,
    // consume all chunks and return a normal HttpResponse::binary() with Content-Length.
    if (hdr->total_size < core::STREAMING_THRESHOLD) {
        std::vector<uint8_t> body_data;
        body_data.reserve(static_cast<size_t>(hdr->total_size));
        // Append leftover if any
        if (!leftover.empty()) {
            body_data.insert(body_data.end(), leftover.begin(), leftover.end());
        }
        while (true) {
            auto chunk = co_await streaming_promise->queue.pop();
            if (!chunk) break;
            if (chunk->empty()) break;
            body_data.insert(body_data.end(), chunk->begin(), chunk->end());
        }
        promises_.remove_streaming(relay_rid);
        co_return HttpResponse::binary(200, std::move(body_data));
    }

    // 10. Commit to 200 OK + chunked transfer encoding.
    // After this point, we cannot send a different status code.
    bool te_ok = co_await conn.write_chunked_te_start(200, "application/octet-stream");
    if (!te_ok) {
        streaming_promise->queue.close_queue();
        promises_.remove_streaming(relay_rid);
        co_return std::nullopt;  // Connection dead, no response to send
    }

    // 11. Write leftover bytes from first chunk (if status byte was inline)
    if (!leftover.empty()) {
        bool ok = co_await conn.write_chunked_te_chunk(
            std::span<const uint8_t>(leftover));
        if (!ok) {
            streaming_promise->queue.close_queue();
            promises_.remove_streaming(relay_rid);
            co_return std::nullopt;
        }
    }

    // 12. Stream remaining chunks: UDS ChunkQueue -> HTTP chunked-TE
    while (true) {
        auto chunk = co_await streaming_promise->queue.pop();
        if (!chunk) break;  // Queue closed = all chunks delivered (sentinel received)
        if (chunk->empty()) break;

        bool write_ok = co_await conn.write_chunked_te_chunk(*chunk);
        if (!write_ok) {
            // HTTP client disconnected mid-stream (Pitfall 2 from research).
            // Cannot send error -- HTTP headers already sent with 200.
            // Close the queue to stop read_loop from pushing more chunks.
            streaming_promise->queue.close_queue();
            break;
        }
    }

    // 13. End chunked-TE
    co_await conn.write_chunked_te_end();

    // 14. Cleanup
    promises_.remove_streaming(relay_rid);

    // Return nullopt = response already written via chunked-TE
    co_return std::nullopt;
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void register_data_routes(HttpRouter& router, DataHandlers& handlers) {
    // POST /blob -- blob write (auth required, exact match).
    router.add_async_route("POST", "/blob",
        [&handlers](const HttpRequest& req, const std::vector<uint8_t>& body,
                    HttpSessionState* session) -> asio::awaitable<HttpResponse> {
            co_return co_await handlers.handle_blob_write(req, body, session);
        });

    // GET /blob/{ns}/{hash} -- blob read (auth required, prefix match).
    router.add_async_route("GET", "/blob/",
        [&handlers](const HttpRequest& req, const std::vector<uint8_t>& body,
                    HttpSessionState* session) -> asio::awaitable<HttpResponse> {
            co_return co_await handlers.handle_blob_read(req, body, session);
        });

    // DELETE /blob/{ns}/{hash} -- blob delete (auth required, prefix match).
    router.add_async_route("DELETE", "/blob/",
        [&handlers](const HttpRequest& req, const std::vector<uint8_t>& body,
                    HttpSessionState* session) -> asio::awaitable<HttpResponse> {
            co_return co_await handlers.handle_blob_delete(req, body, session);
        });

    // POST /batch/read -- batch blob read (auth required, exact match).
    router.add_async_route("POST", "/batch/read",
        [&handlers](const HttpRequest& req, const std::vector<uint8_t>& body,
                    HttpSessionState* session) -> asio::awaitable<HttpResponse> {
            co_return co_await handlers.handle_batch_read(req, body, session);
        });
}

}  // namespace chromatindb::relay::http
