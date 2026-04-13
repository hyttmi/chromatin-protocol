#include <catch2/catch_test_macros.hpp>

#include "relay/http/handlers_query.h"
#include "relay/http/http_parser.h"
#include "relay/translate/translator.h"
#include "relay/translate/type_registry.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

using chromatindb::relay::http::parse_query_param;
using chromatindb::relay::http::extract_path_segment;
using chromatindb::relay::http::extract_two_segments;
using chromatindb::relay::http::is_valid_hex32;

// =============================================================================
// Valid 64-char hex constant for test use (represents a 32-byte namespace/hash)
// =============================================================================

static constexpr auto VALID_NS = "aabbccdd00112233445566778899aabbccddeeff00112233445566778899aabb";
static constexpr auto VALID_HASH = "1122334455667788990011223344556677889900112233445566778899001122";

// =============================================================================
// Query string parsing
// =============================================================================

TEST_CASE("parse_query_param extracts key from query string", "[query_handlers]") {
    REQUIRE(parse_query_param("since_seq=5&limit=100", "since_seq").value() == "5");
    REQUIRE(parse_query_param("since_seq=5&limit=100", "limit").value() == "100");
}

TEST_CASE("parse_query_param returns nullopt for missing key", "[query_handlers]") {
    REQUIRE(!parse_query_param("since_seq=5&limit=100", "offset").has_value());
}

TEST_CASE("parse_query_param returns nullopt for empty query", "[query_handlers]") {
    REQUIRE(!parse_query_param("", "key").has_value());
}

TEST_CASE("parse_query_param handles single key-value pair", "[query_handlers]") {
    REQUIRE(parse_query_param("limit=50", "limit").value() == "50");
}

TEST_CASE("parse_query_param handles key without value", "[query_handlers]") {
    // "key=" should return empty string value
    // "key" without = should not match
    REQUIRE(!parse_query_param("foo", "foo").has_value());
}

TEST_CASE("parse_query_param handles empty value", "[query_handlers]") {
    REQUIRE(parse_query_param("key=", "key").value() == "");
}

TEST_CASE("parse_query_param handles multiple ampersands", "[query_handlers]") {
    REQUIRE(parse_query_param("a=1&b=2&c=3", "c").value() == "3");
}

// =============================================================================
// Path segment extraction
// =============================================================================

TEST_CASE("extract_path_segment extracts namespace from /list/ path", "[query_handlers]") {
    std::string path = std::string("/list/") + VALID_NS;
    auto segment = extract_path_segment(path, "/list/");
    REQUIRE(segment == VALID_NS);
}

TEST_CASE("extract_path_segment returns empty for exact prefix match", "[query_handlers]") {
    auto segment = extract_path_segment("/list/", "/list/");
    REQUIRE(segment.empty());
}

TEST_CASE("extract_path_segment returns empty for path shorter than prefix", "[query_handlers]") {
    auto segment = extract_path_segment("/li", "/list/");
    REQUIRE(segment.empty());
}

TEST_CASE("extract_path_segment strips trailing slash", "[query_handlers]") {
    std::string path = std::string("/list/") + VALID_NS + "/";
    auto segment = extract_path_segment(path, "/list/");
    REQUIRE(segment == VALID_NS);
}

TEST_CASE("extract_path_segment returns empty for non-matching prefix", "[query_handlers]") {
    auto segment = extract_path_segment("/stats/aabb", "/list/");
    REQUIRE(segment.empty());
}

// =============================================================================
// Two-segment extraction
// =============================================================================

TEST_CASE("extract_two_segments extracts namespace and hash", "[query_handlers]") {
    std::string path = std::string("/exists/") + VALID_NS + "/" + VALID_HASH;
    auto [ns, hash] = extract_two_segments(path, "/exists/");
    REQUIRE(ns == VALID_NS);
    REQUIRE(hash == VALID_HASH);
}

TEST_CASE("extract_two_segments returns empty second on single segment", "[query_handlers]") {
    std::string path = std::string("/exists/") + VALID_NS;
    auto [ns, hash] = extract_two_segments(path, "/exists/");
    REQUIRE(ns == VALID_NS);
    REQUIRE(hash.empty());
}

TEST_CASE("extract_two_segments strips trailing slash from second", "[query_handlers]") {
    std::string path = std::string("/metadata/") + VALID_NS + "/" + VALID_HASH + "/";
    auto [ns, hash] = extract_two_segments(path, "/metadata/");
    REQUIRE(ns == VALID_NS);
    REQUIRE(hash == VALID_HASH);
}

TEST_CASE("extract_two_segments returns both empty for no match", "[query_handlers]") {
    auto [a, b] = extract_two_segments("/other/path", "/exists/");
    REQUIRE(a.empty());
    REQUIRE(b.empty());
}

// =============================================================================
// Hex validation
// =============================================================================

TEST_CASE("is_valid_hex32 accepts 64-char lowercase hex", "[query_handlers]") {
    REQUIRE(is_valid_hex32(VALID_NS));
}

TEST_CASE("is_valid_hex32 accepts 64-char uppercase hex", "[query_handlers]") {
    REQUIRE(is_valid_hex32("AABBCCDD00112233445566778899AABBCCDDEEFF00112233445566778899AABB"));
}

TEST_CASE("is_valid_hex32 accepts 64-char mixed-case hex", "[query_handlers]") {
    REQUIRE(is_valid_hex32("aAbBcCdD00112233445566778899aAbBcCdDeEfF00112233445566778899aAbB"));
}

TEST_CASE("is_valid_hex32 rejects too-short string", "[query_handlers]") {
    REQUIRE(!is_valid_hex32("aabb"));
}

TEST_CASE("is_valid_hex32 rejects too-long string", "[query_handlers]") {
    std::string s(65, 'a');
    REQUIRE(!is_valid_hex32(s));
}

TEST_CASE("is_valid_hex32 rejects non-hex characters", "[query_handlers]") {
    std::string bad = std::string(VALID_NS);
    bad[0] = 'g';
    REQUIRE(!is_valid_hex32(bad));
}

TEST_CASE("is_valid_hex32 rejects empty string", "[query_handlers]") {
    REQUIRE(!is_valid_hex32(""));
}

// =============================================================================
// JSON construction correctness
// =============================================================================

// These tests verify that the JSON objects built by each handler have the correct
// "type" field and all required fields for the translator. We construct the JSON
// the same way the handlers do, and verify structure.

TEST_CASE("list_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "list_request"},
        {"namespace", std::string(VALID_NS)},
        {"since_seq", "0"},
        {"limit", 100}
    };
    REQUIRE(j["type"] == "list_request");
    REQUIRE(j.contains("namespace"));
    REQUIRE(j.contains("since_seq"));
    REQUIRE(j.contains("limit"));
}

TEST_CASE("namespace_stats_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "namespace_stats_request"},
        {"namespace", std::string(VALID_NS)}
    };
    REQUIRE(j["type"] == "namespace_stats_request");
    REQUIRE(j.contains("namespace"));
}

TEST_CASE("exists_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "exists_request"},
        {"namespace", std::string(VALID_NS)},
        {"hash", std::string(VALID_HASH)}
    };
    REQUIRE(j["type"] == "exists_request");
    REQUIRE(j.contains("namespace"));
    REQUIRE(j.contains("hash"));
}

TEST_CASE("batch_exists_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "batch_exists_request"},
        {"namespace", std::string(VALID_NS)},
        {"hashes", nlohmann::json::array({std::string(VALID_HASH)})}
    };
    REQUIRE(j["type"] == "batch_exists_request");
    REQUIRE(j.contains("namespace"));
    REQUIRE(j.contains("hashes"));
    REQUIRE(j["hashes"].is_array());
}

TEST_CASE("node_info_request JSON has correct type", "[query_handlers][json]") {
    nlohmann::json j = {{"type", "node_info_request"}};
    REQUIRE(j["type"] == "node_info_request");
    REQUIRE(j.size() == 1);  // Only type field
}

TEST_CASE("peer_info_request JSON has correct type", "[query_handlers][json]") {
    nlohmann::json j = {{"type", "peer_info_request"}};
    REQUIRE(j["type"] == "peer_info_request");
    REQUIRE(j.size() == 1);
}

TEST_CASE("storage_status_request JSON has correct type", "[query_handlers][json]") {
    nlohmann::json j = {{"type", "storage_status_request"}};
    REQUIRE(j["type"] == "storage_status_request");
    REQUIRE(j.size() == 1);
}

TEST_CASE("metadata_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "metadata_request"},
        {"namespace", std::string(VALID_NS)},
        {"hash", std::string(VALID_HASH)}
    };
    REQUIRE(j["type"] == "metadata_request");
    REQUIRE(j.contains("namespace"));
    REQUIRE(j.contains("hash"));
}

TEST_CASE("delegation_list_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "delegation_list_request"},
        {"namespace", std::string(VALID_NS)}
    };
    REQUIRE(j["type"] == "delegation_list_request");
    REQUIRE(j.contains("namespace"));
}

TEST_CASE("time_range_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "time_range_request"},
        {"namespace", std::string(VALID_NS)},
        {"since", "0"},
        {"until", std::to_string(UINT64_MAX)},
        {"limit", 100}
    };
    REQUIRE(j["type"] == "time_range_request");
    REQUIRE(j.contains("namespace"));
    REQUIRE(j.contains("since"));
    REQUIRE(j.contains("until"));
    REQUIRE(j.contains("limit"));
}

TEST_CASE("namespace_list_request JSON has correct type and fields", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "namespace_list_request"},
        {"limit", 100}
    };
    REQUIRE(j["type"] == "namespace_list_request");
    REQUIRE(j.contains("limit"));
}

TEST_CASE("namespace_list_request with after_namespace", "[query_handlers][json]") {
    nlohmann::json j = {
        {"type", "namespace_list_request"},
        {"after_namespace", std::string(VALID_NS)},
        {"limit", 50}
    };
    REQUIRE(j.contains("after_namespace"));
    REQUIRE(j["after_namespace"] == VALID_NS);
}

// =============================================================================
// JSON -> translator integration (verify type names match type_registry)
// =============================================================================

TEST_CASE("All query type names are valid in type registry", "[query_handlers][registry]") {
    // These are the exact type strings used by the handlers.
    std::vector<std::string> query_types = {
        "list_request",
        "namespace_stats_request",
        "exists_request",
        "batch_exists_request",
        "node_info_request",
        "peer_info_request",
        "storage_status_request",
        "metadata_request",
        "delegation_list_request",
        "time_range_request",
        "namespace_list_request",
    };

    for (const auto& name : query_types) {
        auto wire_type = chromatindb::relay::translate::type_from_string(name);
        REQUIRE(wire_type.has_value());
        INFO("Type name: " << name << " -> wire type: " << static_cast<int>(*wire_type));
    }
}

// =============================================================================
// json_to_binary integration for query types
// =============================================================================

TEST_CASE("json_to_binary succeeds for node_info_request", "[query_handlers][translate]") {
    nlohmann::json j = {{"type", "node_info_request"}};
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 39);
}

TEST_CASE("json_to_binary succeeds for exists_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "exists_request"},
        {"namespace", std::string(VALID_NS)},
        {"hash", std::string(VALID_HASH)}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 37);
    REQUIRE(result->payload.size() == 64);  // 32-byte ns + 32-byte hash
}

TEST_CASE("json_to_binary succeeds for list_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "list_request"},
        {"namespace", std::string(VALID_NS)},
        {"since_seq", "0"},
        {"limit", 100}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 33);
}

TEST_CASE("json_to_binary succeeds for storage_status_request", "[query_handlers][translate]") {
    nlohmann::json j = {{"type", "storage_status_request"}};
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 43);
}

TEST_CASE("json_to_binary succeeds for time_range_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "time_range_request"},
        {"namespace", std::string(VALID_NS)},
        {"since", "1000"},
        {"until", "2000"},
        {"limit", 50}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 57);
}

TEST_CASE("json_to_binary succeeds for namespace_stats_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "namespace_stats_request"},
        {"namespace", std::string(VALID_NS)}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 45);
}

TEST_CASE("json_to_binary succeeds for metadata_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "metadata_request"},
        {"namespace", std::string(VALID_NS)},
        {"hash", std::string(VALID_HASH)}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 47);
}

TEST_CASE("json_to_binary succeeds for delegation_list_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "delegation_list_request"},
        {"namespace", std::string(VALID_NS)}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 51);
}

TEST_CASE("json_to_binary succeeds for peer_info_request", "[query_handlers][translate]") {
    nlohmann::json j = {{"type", "peer_info_request"}};
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 55);
}

TEST_CASE("json_to_binary succeeds for batch_exists_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "batch_exists_request"},
        {"namespace", std::string(VALID_NS)},
        {"hashes", nlohmann::json::array({std::string(VALID_HASH)})}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 49);
}

TEST_CASE("json_to_binary succeeds for namespace_list_request", "[query_handlers][translate]") {
    nlohmann::json j = {
        {"type", "namespace_list_request"},
        {"limit", 100}
    };
    auto result = chromatindb::relay::translate::json_to_binary(j);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 41);
}
