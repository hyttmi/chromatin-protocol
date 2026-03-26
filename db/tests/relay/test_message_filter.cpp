#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "relay/core/message_filter.h"
#include "relay/identity/relay_identity.h"
#include "db/identity/identity.h"
#include "db/crypto/signing.h"

using namespace chromatindb::relay::core;
using namespace chromatindb::wire;
using Catch::Matchers::ContainsSubstring;

// ===== Message filter tests =====

TEST_CASE("is_client_allowed permits client operation types", "[message_filter]") {
    // 32 client-allowed types (16 per RELAY-03 + 4 query extensions + 6 node-level queries + 6 blob-level queries)
    CHECK(is_client_allowed(TransportMsgType_Data));
    CHECK(is_client_allowed(TransportMsgType_WriteAck));
    CHECK(is_client_allowed(TransportMsgType_Delete));
    CHECK(is_client_allowed(TransportMsgType_DeleteAck));
    CHECK(is_client_allowed(TransportMsgType_ReadRequest));
    CHECK(is_client_allowed(TransportMsgType_ReadResponse));
    CHECK(is_client_allowed(TransportMsgType_ListRequest));
    CHECK(is_client_allowed(TransportMsgType_ListResponse));
    CHECK(is_client_allowed(TransportMsgType_StatsRequest));
    CHECK(is_client_allowed(TransportMsgType_StatsResponse));
    CHECK(is_client_allowed(TransportMsgType_ExistsRequest));
    CHECK(is_client_allowed(TransportMsgType_ExistsResponse));
    CHECK(is_client_allowed(TransportMsgType_NodeInfoRequest));
    CHECK(is_client_allowed(TransportMsgType_NodeInfoResponse));
    CHECK(is_client_allowed(TransportMsgType_NamespaceListRequest));
    CHECK(is_client_allowed(TransportMsgType_NamespaceListResponse));
    CHECK(is_client_allowed(TransportMsgType_StorageStatusRequest));
    CHECK(is_client_allowed(TransportMsgType_StorageStatusResponse));
    CHECK(is_client_allowed(TransportMsgType_NamespaceStatsRequest));
    CHECK(is_client_allowed(TransportMsgType_NamespaceStatsResponse));
    CHECK(is_client_allowed(TransportMsgType_MetadataRequest));
    CHECK(is_client_allowed(TransportMsgType_MetadataResponse));
    CHECK(is_client_allowed(TransportMsgType_BatchExistsRequest));
    CHECK(is_client_allowed(TransportMsgType_BatchExistsResponse));
    CHECK(is_client_allowed(TransportMsgType_DelegationListRequest));
    CHECK(is_client_allowed(TransportMsgType_DelegationListResponse));
    CHECK(is_client_allowed(TransportMsgType_Subscribe));
    CHECK(is_client_allowed(TransportMsgType_Unsubscribe));
    CHECK(is_client_allowed(TransportMsgType_Notification));
    CHECK(is_client_allowed(TransportMsgType_Ping));
    CHECK(is_client_allowed(TransportMsgType_Pong));
    CHECK(is_client_allowed(TransportMsgType_Goodbye));
}

TEST_CASE("is_client_allowed blocks peer operation types", "[message_filter]") {
    // 14 peer-only types
    CHECK_FALSE(is_client_allowed(TransportMsgType_SyncRequest));
    CHECK_FALSE(is_client_allowed(TransportMsgType_SyncAccept));
    CHECK_FALSE(is_client_allowed(TransportMsgType_SyncComplete));
    CHECK_FALSE(is_client_allowed(TransportMsgType_ReconcileInit));
    CHECK_FALSE(is_client_allowed(TransportMsgType_ReconcileRanges));
    CHECK_FALSE(is_client_allowed(TransportMsgType_ReconcileItems));
    CHECK_FALSE(is_client_allowed(TransportMsgType_SyncRejected));
    CHECK_FALSE(is_client_allowed(TransportMsgType_PeerListRequest));
    CHECK_FALSE(is_client_allowed(TransportMsgType_PeerListResponse));
    CHECK_FALSE(is_client_allowed(TransportMsgType_NamespaceList));
    CHECK_FALSE(is_client_allowed(TransportMsgType_BlobRequest));
    CHECK_FALSE(is_client_allowed(TransportMsgType_BlobTransfer));
    CHECK_FALSE(is_client_allowed(TransportMsgType_StorageFull));
    CHECK_FALSE(is_client_allowed(TransportMsgType_TrustedHello));
}

TEST_CASE("is_client_allowed blocks handshake and internal types", "[message_filter]") {
    CHECK_FALSE(is_client_allowed(TransportMsgType_None));
    CHECK_FALSE(is_client_allowed(TransportMsgType_KemPubkey));
    CHECK_FALSE(is_client_allowed(TransportMsgType_KemCiphertext));
    CHECK_FALSE(is_client_allowed(TransportMsgType_AuthSignature));
    CHECK_FALSE(is_client_allowed(TransportMsgType_AuthPubkey));
    CHECK_FALSE(is_client_allowed(TransportMsgType_PQRequired));
    CHECK_FALSE(is_client_allowed(TransportMsgType_QuotaExceeded));
}

TEST_CASE("is_client_allowed default-denies unknown types", "[message_filter]") {
    CHECK_FALSE(is_client_allowed(static_cast<TransportMsgType>(100)));
    CHECK_FALSE(is_client_allowed(static_cast<TransportMsgType>(-1)));
}

TEST_CASE("type_name returns human-readable names", "[message_filter]") {
    CHECK(type_name(TransportMsgType_Data) == "Data");
    CHECK(type_name(TransportMsgType_SyncRequest) == "SyncRequest");
    CHECK(type_name(TransportMsgType_Ping) == "Ping");
    CHECK(type_name(TransportMsgType_WriteAck) == "WriteAck");
}

TEST_CASE("type_name returns Unknown for invalid values", "[message_filter]") {
    CHECK(type_name(static_cast<TransportMsgType>(100)) == "Unknown(100)");
    CHECK(type_name(static_cast<TransportMsgType>(-1)) == "Unknown(-1)");
}

// ===== Identity adapter tests =====

TEST_CASE("NodeIdentity::from_keys round-trips with RelayIdentity", "[message_filter]") {
    auto relay_id = chromatindb::relay::identity::RelayIdentity::generate();
    auto node_id = relay_id.to_node_identity();

    // Public keys must match
    auto relay_pk = relay_id.public_key();
    auto node_pk = node_id.public_key();
    REQUIRE(relay_pk.size() == node_pk.size());
    CHECK(std::equal(relay_pk.begin(), relay_pk.end(), node_pk.begin()));
}

TEST_CASE("RelayIdentity::to_node_identity signs identically", "[message_filter]") {
    auto relay_id = chromatindb::relay::identity::RelayIdentity::generate();
    auto node_id = relay_id.to_node_identity();

    // Sign the same data with both identities
    std::vector<uint8_t> test_data = {1, 2, 3, 4, 5, 6, 7, 8};
    auto relay_sig = relay_id.sign(test_data);
    auto node_sig = node_id.sign(test_data);

    // Both signatures should verify with the relay's public key
    auto pk = relay_id.public_key();
    CHECK(chromatindb::crypto::Signer::verify(test_data, relay_sig, pk));
    CHECK(chromatindb::crypto::Signer::verify(test_data, node_sig, pk));
}
