#include "relay/http/handlers_data.h"
#include "relay/http/http_parser.h"
#include "relay/http/token_store.h"
#include "relay/core/request_router.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/core/write_tracker.h"
#include "relay/translate/translator.h"
#include "relay/util/hex.h"
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
                           const std::atomic<uint32_t>& max_blob_size,
                           const std::atomic<uint32_t>& request_timeout)
    : uds_mux_(uds_mux)
    , router_(router)
    , promises_(promises)
    , write_tracker_(write_tracker)
    , max_blob_size_(max_blob_size)
    , request_timeout_(request_timeout) {}

// ---------------------------------------------------------------------------
// Common: send transport message and co_await UDS response
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<ResponseData>> DataHandlers::send_and_await(
    std::vector<uint8_t> transport_msg, uint32_t relay_rid) {

    auto executor = co_await asio::this_coro::executor;
    ResponsePromise promise(executor);
    promises_.register_promise(relay_rid, &promise);

    // Send via UDS.
    bool sent = uds_mux_.send(std::move(transport_msg));
    if (!sent) {
        promises_.remove(relay_rid);
        co_return std::nullopt;
    }

    // Await response with timeout.
    auto timeout_sec = request_timeout_.load(std::memory_order_relaxed);
    if (timeout_sec == 0) timeout_sec = 10;  // Default fallback.
    auto result = co_await promise.wait(std::chrono::seconds(timeout_sec));

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

    // 2. Size limit check (FEAT-02).
    auto max_size = max_blob_size_.load(std::memory_order_relaxed);
    if (max_size > 0 && body.size() > max_size) {
        co_return HttpResponse::error(413, "payload_too_large",
            "body size " + std::to_string(body.size()) + " exceeds limit " + std::to_string(max_size));
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
    auto json_opt = translate::binary_to_json(result->type,
        std::span<const uint8_t>(result->payload));
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
    auto json_opt = translate::binary_to_json(result->type,
        std::span<const uint8_t>(result->payload));
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
    auto tr = translate::json_to_binary(translate_input);
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
    auto json_opt = translate::binary_to_json(result->type,
        std::span<const uint8_t>(result->payload));
    if (!json_opt) {
        co_return HttpResponse::error(502, "decode_error",
            "failed to decode BatchReadResponse");
    }

    co_return HttpResponse::json(200, *json_opt);
}

}  // namespace chromatindb::relay::http
