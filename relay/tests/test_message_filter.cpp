#include <catch2/catch_test_macros.hpp>

#include "relay/core/message_filter.h"

using namespace chromatindb::relay::core;

TEST_CASE("all 38 client types are allowed", "[message_filter]") {
    // All 38 types from the node's supported_types array.
    const char* allowed[] = {
        "batch_exists_request", "batch_exists_response",
        "batch_read_request", "batch_read_response",
        "data", "delete", "delete_ack",
        "delegation_list_request", "delegation_list_response",
        "exists_request", "exists_response",
        "goodbye",
        "list_request", "list_response",
        "metadata_request", "metadata_response",
        "namespace_list_request", "namespace_list_response",
        "namespace_stats_request", "namespace_stats_response",
        "node_info_request", "node_info_response",
        "notification",
        "peer_info_request", "peer_info_response",
        "ping", "pong",
        "read_request", "read_response",
        "stats_request", "stats_response",
        "storage_status_request", "storage_status_response",
        "subscribe",
        "time_range_request", "time_range_response",
        "unsubscribe",
        "write_ack",
    };
    REQUIRE(sizeof(allowed) / sizeof(allowed[0]) == ALLOWED_TYPE_COUNT);

    for (const auto& type : allowed) {
        INFO("type: " << type);
        REQUIRE(is_type_allowed(type));
    }
}

TEST_CASE("blocked peer-internal types are rejected", "[message_filter]") {
    // Sync/PEX/KEM/internal types that must never be forwarded.
    REQUIRE_FALSE(is_type_allowed("sync_request"));
    REQUIRE_FALSE(is_type_allowed("kem_hello"));
    REQUIRE_FALSE(is_type_allowed("pq_required"));
    REQUIRE_FALSE(is_type_allowed("blob_notify"));
    REQUIRE_FALSE(is_type_allowed("blob_fetch"));
    REQUIRE_FALSE(is_type_allowed("blob_fetch_response"));
    REQUIRE_FALSE(is_type_allowed("sync_namespace_announce"));
    REQUIRE_FALSE(is_type_allowed("reconcile_request"));
    REQUIRE_FALSE(is_type_allowed("peer_exchange"));
}

TEST_CASE("empty string rejected", "[message_filter]") {
    REQUIRE_FALSE(is_type_allowed(""));
}

TEST_CASE("auth types not in filter", "[message_filter]") {
    // Relay-only auth messages are not forwarded to node.
    REQUIRE_FALSE(is_type_allowed("challenge"));
    REQUIRE_FALSE(is_type_allowed("challenge_response"));
    REQUIRE_FALSE(is_type_allowed("auth_ok"));
    REQUIRE_FALSE(is_type_allowed("auth_error"));
    REQUIRE_FALSE(is_type_allowed("error"));
}

TEST_CASE("node-originated signals not in client-send allowlist", "[message_filter]") {
    // StorageFull and QuotaExceeded are node->client only.
    // Clients don't send them, so is_type_allowed rejects them.
    REQUIRE_FALSE(is_type_allowed("storage_full"));
    REQUIRE_FALSE(is_type_allowed("quota_exceeded"));
}

TEST_CASE("is_wire_type_allowed accepts 38 client types + node signals", "[message_filter]") {
    // All 38 client types by wire type.
    uint8_t allowed_wires[] = {
        5, 6, 7, 8,
        17, 18, 19, 20, 21,
        30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
    };
    for (auto wt : allowed_wires) {
        INFO("wire_type: " << static_cast<int>(wt));
        REQUIRE(is_wire_type_allowed(wt));
    }

    // Node-originated signals also allowed outbound.
    REQUIRE(is_wire_type_allowed(22));  // StorageFull
    REQUIRE(is_wire_type_allowed(25));  // QuotaExceeded
}

TEST_CASE("is_wire_type_allowed rejects blocked wire types", "[message_filter]") {
    uint8_t blocked[] = {
        0, 1, 2, 3, 4,      // Unused/internal
        9, 10, 11, 12, 13, 14, 15, 16,  // Gap
        23, 24, 26, 27, 28, 29,          // Gap between signals and WriteAck
        59, 60, 61, 62,                  // BlobNotify, BlobFetch, etc. (peer-internal)
    };
    for (auto wt : blocked) {
        INFO("wire_type: " << static_cast<int>(wt));
        REQUIRE_FALSE(is_wire_type_allowed(wt));
    }
}
