#pragma once

#include "relay/http/http_response.h"
#include "relay/http/http_router.h"

#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chromatindb::relay::core {
class RequestRouter;
class UdsMultiplexer;
} // namespace chromatindb::relay::core

namespace chromatindb::relay::http {

class ResponsePromiseMap;

// =============================================================================
// Query string parsing utility
// =============================================================================

/// Parse a single query parameter value from a query string (e.g., "key=val&foo=bar").
/// Returns nullopt if the key is not found.
std::optional<std::string> parse_query_param(std::string_view query, std::string_view key);

// =============================================================================
// Path parameter extraction
// =============================================================================

/// Extract the first path segment after a prefix.
/// e.g., path="/list/abcd..." prefix="/list/" -> "abcd..."
/// Returns empty string_view if the prefix doesn't match or nothing follows it.
std::string_view extract_path_segment(std::string_view path, std::string_view prefix);

/// Extract two path segments after a prefix: /{prefix}/{seg1}/{seg2}
/// e.g., path="/exists/aabb/ccdd" prefix="/exists/" -> {"aabb", "ccdd"}
/// Returns {segment1, segment2}. Either may be empty on error.
std::pair<std::string_view, std::string_view> extract_two_segments(
    std::string_view path, std::string_view prefix);

// =============================================================================
// Hex validation
// =============================================================================

/// Validate that a string is exactly 64 hex characters (32 bytes).
bool is_valid_hex32(std::string_view s);

// =============================================================================
// Query handlers registration
// =============================================================================

/// Strand type alias for query handler strand confinement.
using Strand = asio::strand<asio::io_context::executor_type>;

/// Configuration for query handler dependencies.
struct QueryHandlerDeps {
    core::UdsMultiplexer& uds_mux;
    core::RequestRouter& router;
    ResponsePromiseMap& promises;
    asio::io_context& ioc;
    const std::atomic<uint32_t>* request_timeout = nullptr;  // SIGHUP-reloadable
    Strand* strand = nullptr;  // Required: global strand for shared state serialization
};

/// Register all query endpoint routes on the router.
/// All query routes require authentication (Bearer token).
///
/// Routes registered:
///   GET  /list/           -> list blobs in namespace
///   GET  /stats/          -> namespace stats (alias for /namespace-stats/)
///   GET  /exists/         -> check if blob exists
///   POST /batch/exists    -> batch existence check
///   GET  /node-info       -> node info
///   GET  /peer-info       -> peer info
///   GET  /storage-status  -> storage status
///   GET  /namespace-stats/ -> namespace stats
///   GET  /metadata/       -> blob metadata
///   GET  /delegations/    -> delegation list
///   GET  /time-range/     -> time range query
///   GET  /namespace-list  -> list all namespaces
void register_query_routes(HttpRouter& router, QueryHandlerDeps deps);

} // namespace chromatindb::relay::http
