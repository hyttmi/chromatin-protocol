#pragma once

#include "relay/http/http_response.h"
#include "relay/http/response_promise.h"

#include <asio.hpp>
#include <cstdint>
#include <atomic>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace chromatindb::relay::core {
class UdsMultiplexer;
class RequestRouter;
class WriteTracker;
}  // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

struct HttpRequest;
struct HttpSessionState;

/// Parsed path parameters from /blob/{namespace}/{hash} URLs.
struct BlobPathParams {
    std::vector<uint8_t> namespace_bytes;  // 32 bytes
    std::vector<uint8_t> hash_bytes;       // 32 bytes
};

/// Parse namespace and hash hex from a path suffix after a prefix.
/// path must start with prefix; the remainder is "/{64-hex-ns}/{64-hex-hash}".
/// Returns nullopt if hex is invalid or wrong length.
std::optional<BlobPathParams> extract_blob_path_params(std::string_view path,
                                                        std::string_view prefix);

/// Data operation HTTP handlers: blob write, read, delete, batch read.
///
/// These handlers are coroutines that co_await ResponsePromise for UDS responses.
/// Raw binary pass-through for blob data -- no JSON wrapping for write/read bodies.
/// WriteAck/DeleteAck translated to JSON via binary_to_json for responses.
///
/// Per D-07, D-08, D-09, D-14, D-26.
class DataHandlers {
public:
    DataHandlers(core::UdsMultiplexer& uds_mux,
                 core::RequestRouter& router,
                 ResponsePromiseMap& promises,
                 core::WriteTracker& write_tracker,
                 const std::atomic<uint32_t>& max_blob_size,
                 const std::atomic<uint32_t>& request_timeout);

    /// POST /blob -- raw FlatBuffer body, returns JSON WriteAck.
    asio::awaitable<HttpResponse> handle_blob_write(
        const HttpRequest& req, const std::vector<uint8_t>& body,
        HttpSessionState* session);

    /// GET /blob/{ns}/{hash} -- returns raw binary ReadResponse (application/octet-stream).
    asio::awaitable<HttpResponse> handle_blob_read(
        const HttpRequest& req, const std::vector<uint8_t>& body,
        HttpSessionState* session);

    /// DELETE /blob/{ns}/{hash} -- raw FlatBuffer tombstone body, returns JSON DeleteAck.
    asio::awaitable<HttpResponse> handle_blob_delete(
        const HttpRequest& req, const std::vector<uint8_t>& body,
        HttpSessionState* session);

    /// POST /batch/read -- JSON body, returns JSON BatchReadResponse with base64 blobs.
    asio::awaitable<HttpResponse> handle_batch_read(
        const HttpRequest& req, const std::vector<uint8_t>& body,
        HttpSessionState* session);

private:
    /// Send encoded message via UDS and co_await response with timeout.
    /// Returns ResponseData on success, nullopt on timeout or UDS down.
    asio::awaitable<std::optional<ResponseData>> send_and_await(
        std::vector<uint8_t> transport_msg, uint32_t relay_rid);

    core::UdsMultiplexer& uds_mux_;
    core::RequestRouter& router_;
    ResponsePromiseMap& promises_;
    core::WriteTracker& write_tracker_;
    const std::atomic<uint32_t>& max_blob_size_;
    const std::atomic<uint32_t>& request_timeout_;
};

class HttpRouter;

/// Register data operation routes on the HTTP router.
/// Uses add_async_route() for coroutine-based handlers.
/// Routes:
///   POST   /blob       -> handle_blob_write (auth required)
///   GET    /blob/      -> handle_blob_read  (auth required, prefix match)
///   DELETE /blob/      -> handle_blob_delete (auth required, prefix match)
///   POST   /batch/read -> handle_batch_read (auth required)
void register_data_routes(HttpRouter& router, DataHandlers& handlers);

}  // namespace chromatindb::relay::http
