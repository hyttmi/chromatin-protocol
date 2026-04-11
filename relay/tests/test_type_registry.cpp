#include <catch2/catch_test_macros.hpp>

#include "relay/translate/type_registry.h"
#include "relay/translate/json_schema.h"

using namespace chromatindb::relay::translate;

TEST_CASE("TYPE_REGISTRY is sorted by json_name", "[type_registry]") {
    for (size_t i = 1; i < TYPE_REGISTRY_SIZE; ++i) {
        INFO("entry " << i - 1 << " '" << TYPE_REGISTRY[i - 1].json_name
             << "' should be < entry " << i << " '" << TYPE_REGISTRY[i].json_name << "'");
        REQUIRE(TYPE_REGISTRY[i - 1].json_name < TYPE_REGISTRY[i].json_name);
    }
}

TEST_CASE("registry has expected size", "[type_registry]") {
    REQUIRE(TYPE_REGISTRY_SIZE == 41);
    REQUIRE(registry_size() == 41);
}

TEST_CASE("type_from_string returns correct wire type for all entries", "[type_registry]") {
    for (size_t i = 0; i < TYPE_REGISTRY_SIZE; ++i) {
        auto result = type_from_string(TYPE_REGISTRY[i].json_name);
        INFO("type: " << TYPE_REGISTRY[i].json_name);
        REQUIRE(result.has_value());
        REQUIRE(*result == TYPE_REGISTRY[i].wire_type);
    }
}

TEST_CASE("type_to_string returns correct name for all entries", "[type_registry]") {
    for (size_t i = 0; i < TYPE_REGISTRY_SIZE; ++i) {
        auto result = type_to_string(TYPE_REGISTRY[i].wire_type);
        INFO("wire_type: " << static_cast<int>(TYPE_REGISTRY[i].wire_type));
        REQUIRE(result.has_value());
        REQUIRE(*result == TYPE_REGISTRY[i].json_name);
    }
}

TEST_CASE("type_from_string returns nullopt for unknown type", "[type_registry]") {
    REQUIRE_FALSE(type_from_string("sync_request").has_value());
    REQUIRE_FALSE(type_from_string("kem_hello").has_value());
    REQUIRE_FALSE(type_from_string("blob_notify").has_value());
    REQUIRE_FALSE(type_from_string("").has_value());
    REQUIRE_FALSE(type_from_string("nonexistent").has_value());
}

TEST_CASE("type_to_string returns nullopt for unknown wire type", "[type_registry]") {
    REQUIRE_FALSE(type_to_string(0).has_value());
    REQUIRE_FALSE(type_to_string(1).has_value());
    REQUIRE_FALSE(type_to_string(2).has_value());
    REQUIRE_FALSE(type_to_string(3).has_value());
    REQUIRE_FALSE(type_to_string(4).has_value());
    REQUIRE_FALSE(type_to_string(9).has_value());
    REQUIRE_FALSE(type_to_string(255).has_value());
}

TEST_CASE("roundtrip all types", "[type_registry]") {
    for (size_t i = 0; i < TYPE_REGISTRY_SIZE; ++i) {
        auto name = TYPE_REGISTRY[i].json_name;
        auto wire = type_from_string(name);
        REQUIRE(wire.has_value());
        auto back = type_to_string(*wire);
        REQUIRE(back.has_value());
        REQUIRE(*back == name);
    }
}

TEST_CASE("ErrorResponse type 63 maps to 'error'", "[type_registry]") {
    auto name = type_to_string(63);
    REQUIRE(name.has_value());
    CHECK(*name == "error");

    auto wire = type_from_string("error");
    REQUIRE(wire.has_value());
    CHECK(*wire == 63);
}

TEST_CASE("specific type mappings", "[type_registry]") {
    REQUIRE(type_from_string("read_request").value() == 31);
    REQUIRE(type_from_string("batch_read_response").value() == 54);
    REQUIRE(type_from_string("data").value() == 8);
    REQUIRE(type_from_string("subscribe").value() == 19);
    REQUIRE(type_from_string("notification").value() == 21);
    REQUIRE(type_from_string("storage_full").value() == 22);
    REQUIRE(type_from_string("quota_exceeded").value() == 25);

    REQUIRE(type_to_string(31).value() == "read_request");
    REQUIRE(type_to_string(54).value() == "batch_read_response");
    REQUIRE(type_to_string(8).value() == "data");
}

TEST_CASE("schema_for_type returns correct schema", "[json_schema]") {
    auto s = schema_for_type(31);
    REQUIRE(s != nullptr);
    REQUIRE(s->type_name == "read_request");
    REQUIRE(s->wire_type == 31);
    REQUIRE_FALSE(s->is_flatbuffer);
    REQUIRE(s->fields.size() == 3);  // request_id, namespace, hash
}

TEST_CASE("schema_for_type identifies FlatBuffer types", "[json_schema]") {
    // Data (8)
    auto data = schema_for_type(8);
    REQUIRE(data != nullptr);
    REQUIRE(data->is_flatbuffer);
    REQUIRE(data->fields.empty());

    // ReadResponse (32)
    auto read_resp = schema_for_type(32);
    REQUIRE(read_resp != nullptr);
    REQUIRE(read_resp->is_flatbuffer);

    // BatchReadResponse (54)
    auto batch = schema_for_type(54);
    REQUIRE(batch != nullptr);
    REQUIRE(batch->is_flatbuffer);
}

TEST_CASE("schema_for_type returns nullptr for unknown", "[json_schema]") {
    REQUIRE(schema_for_type(0) == nullptr);
    REQUIRE(schema_for_type(1) == nullptr);
    REQUIRE(schema_for_type(59) == nullptr);
    REQUIRE(schema_for_type(255) == nullptr);
}

TEST_CASE("schema_for_name delegates through type_registry", "[json_schema]") {
    auto s = schema_for_name("notification");
    REQUIRE(s != nullptr);
    REQUIRE(s->wire_type == 21);
    REQUIRE(s->fields.size() == 5);

    REQUIRE(schema_for_name("nonexistent") == nullptr);
}

TEST_CASE("all registry entries have a corresponding schema", "[json_schema]") {
    for (size_t i = 0; i < TYPE_REGISTRY_SIZE; ++i) {
        auto s = schema_for_type(TYPE_REGISTRY[i].wire_type);
        INFO("wire_type: " << static_cast<int>(TYPE_REGISTRY[i].wire_type)
             << " name: " << TYPE_REGISTRY[i].json_name);
        REQUIRE(s != nullptr);
        REQUIRE(s->type_name == TYPE_REGISTRY[i].json_name);
    }
}

TEST_CASE("FieldEncoding enum covers expected values", "[json_schema]") {
    // Verify the encoding enum has the expected values via schema inspection.
    auto read = schema_for_name("read_request");
    REQUIRE(read != nullptr);
    bool found_hex32 = false;
    bool found_request_id = false;
    for (const auto& f : read->fields) {
        if (f.encoding == FieldEncoding::HEX_32) found_hex32 = true;
        if (f.encoding == FieldEncoding::REQUEST_ID) found_request_id = true;
    }
    REQUIRE(found_hex32);
    REQUIRE(found_request_id);

    // StatsResponse is now compound (no FieldSpec entries). Check write_ack for UINT64_STRING.
    auto write_ack = schema_for_name("write_ack");
    REQUIRE(write_ack != nullptr);
    bool found_uint64 = false;
    for (const auto& f : write_ack->fields) {
        if (f.encoding == FieldEncoding::UINT64_STRING) found_uint64 = true;
    }
    REQUIRE(found_uint64);
}
