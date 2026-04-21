#include <catch2/catch_test_macros.hpp>
#include "cli/src/commands_internal.h"
#include "cli/src/envelope.h"
#include "cli/src/wire.h"
#include "cli/src/identity.h"
#include "cli/tests/pipeline_test_support.h"
#include <flatbuffers/flatbuffers.h>
#include <oqs/oqs.h>
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

using namespace chromatindb::cli;
using chromatindb::cli::testing::ScriptedSource;
using chromatindb::cli::testing::make_reply;

TEST_CASE("wire: BE encode/decode roundtrip", "[wire]") {
    // u16
    {
        uint8_t buf[2];
        store_u16_be(buf, 0x1234);
        REQUIRE(buf[0] == 0x12);
        REQUIRE(buf[1] == 0x34);
        REQUIRE(load_u16_be(buf) == 0x1234);
    }
    // u16 boundary
    {
        uint8_t buf[2];
        store_u16_be(buf, 0);
        REQUIRE(load_u16_be(buf) == 0);
        store_u16_be(buf, 0xFFFF);
        REQUIRE(load_u16_be(buf) == 0xFFFF);
    }
    // u32
    {
        uint8_t buf[4];
        store_u32_be(buf, 0xDEADBEEF);
        REQUIRE(buf[0] == 0xDE);
        REQUIRE(buf[1] == 0xAD);
        REQUIRE(buf[2] == 0xBE);
        REQUIRE(buf[3] == 0xEF);
        REQUIRE(load_u32_be(buf) == 0xDEADBEEF);
    }
    // u64
    {
        uint8_t buf[8];
        store_u64_be(buf, 0x0102030405060708ULL);
        REQUIRE(buf[0] == 0x01);
        REQUIRE(buf[1] == 0x02);
        REQUIRE(buf[2] == 0x03);
        REQUIRE(buf[3] == 0x04);
        REQUIRE(buf[4] == 0x05);
        REQUIRE(buf[5] == 0x06);
        REQUIRE(buf[6] == 0x07);
        REQUIRE(buf[7] == 0x08);
        REQUIRE(load_u64_be(buf) == 0x0102030405060708ULL);
    }
}

TEST_CASE("wire: encode_transport roundtrips", "[wire]") {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t type = static_cast<uint8_t>(MsgType::ReadRequest);
    uint32_t req_id = 42;

    auto encoded = encode_transport(type, payload, req_id);
    REQUIRE(!encoded.empty());

    auto decoded = decode_transport(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == type);
    REQUIRE(decoded->request_id == req_id);
    REQUIRE(decoded->payload == payload);
}

TEST_CASE("wire: encode_transport with empty payload", "[wire]") {
    auto encoded = encode_transport(
        static_cast<uint8_t>(MsgType::StatsRequest), {}, 99);
    auto decoded = decode_transport(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == static_cast<uint8_t>(MsgType::StatsRequest));
    REQUIRE(decoded->payload.empty());
    REQUIRE(decoded->request_id == 99);
}

TEST_CASE("wire: decode_transport rejects garbage", "[wire]") {
    std::vector<uint8_t> garbage = {0xFF, 0xFE, 0xFD};
    auto decoded = decode_transport(garbage);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("wire: aead_frame encrypt/decrypt roundtrip", "[wire]") {
    // Generate a random key
    std::array<uint8_t, 32> key{};
    randombytes_buf(key.data(), key.size());

    std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t counter = 1;

    auto ciphertext = encrypt_frame(plaintext, key, counter);
    REQUIRE(ciphertext.size() == plaintext.size() + 16);  // +16 for tag

    auto result = decrypt_frame(ciphertext, key, counter);
    REQUIRE(result.has_value());
    REQUIRE(*result == plaintext);
}

TEST_CASE("wire: aead_frame nonce is 4 zeros + 8BE counter", "[wire]") {
    // Counter = 1
    {
        auto nonce = make_aead_nonce(1);
        REQUIRE(nonce.size() == 12);
        // First 4 bytes: zeros
        REQUIRE(nonce[0] == 0);
        REQUIRE(nonce[1] == 0);
        REQUIRE(nonce[2] == 0);
        REQUIRE(nonce[3] == 0);
        // Last 8 bytes: big-endian 1
        REQUIRE(nonce[4] == 0);
        REQUIRE(nonce[5] == 0);
        REQUIRE(nonce[6] == 0);
        REQUIRE(nonce[7] == 0);
        REQUIRE(nonce[8] == 0);
        REQUIRE(nonce[9] == 0);
        REQUIRE(nonce[10] == 0);
        REQUIRE(nonce[11] == 1);
    }
    // Counter = 256
    {
        auto nonce = make_aead_nonce(256);
        REQUIRE(nonce[4] == 0);
        REQUIRE(nonce[5] == 0);
        REQUIRE(nonce[6] == 0);
        REQUIRE(nonce[7] == 0);
        REQUIRE(nonce[8] == 0);
        REQUIRE(nonce[9] == 0);
        REQUIRE(nonce[10] == 1);
        REQUIRE(nonce[11] == 0);
    }
}

TEST_CASE("wire: aead_frame wrong counter fails decrypt", "[wire]") {
    std::array<uint8_t, 32> key{};
    randombytes_buf(key.data(), key.size());

    std::vector<uint8_t> plaintext = {10, 20, 30};

    auto ciphertext = encrypt_frame(plaintext, key, 1);
    auto result = decrypt_frame(ciphertext, key, 2);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("wire: encode_blob/decode_blob roundtrip", "[wire]") {
    BlobData blob;
    blob.signer_hint.fill(0xAB);
    blob.data = {0xCA, 0xFE, 0xBA, 0xBE};
    blob.ttl = 3600;
    blob.timestamp = 1700000000;
    blob.signature.resize(4627, 0x02);

    auto encoded = encode_blob(blob);
    REQUIRE(!encoded.empty());

    auto decoded = decode_blob(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->signer_hint == blob.signer_hint);
    REQUIRE(decoded->data == blob.data);
    REQUIRE(decoded->ttl == blob.ttl);
    REQUIRE(decoded->timestamp == blob.timestamp);
    REQUIRE(decoded->signature == blob.signature);
}

TEST_CASE("wire: encode_blob/decode_blob with zero ttl", "[wire]") {
    BlobData blob;
    blob.signer_hint.fill(0x00);
    blob.data = {0x42};
    blob.ttl = 0;
    blob.timestamp = 0;
    blob.signature.resize(10, 0x22);

    auto encoded = encode_blob(blob);
    auto decoded = decode_blob(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->ttl == 0);
    REQUIRE(decoded->timestamp == 0);
}

TEST_CASE("wire: decode_blob rejects garbage", "[wire]") {
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02};
    auto decoded = decode_blob(garbage);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("wire: build_signing_input deterministic", "[wire]") {
    std::array<uint8_t, 32> ns{};
    ns.fill(0xAA);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    uint32_t ttl = 3600;
    uint64_t timestamp = 1700000000;

    auto hash1 = build_signing_input(ns, data, ttl, timestamp);
    auto hash2 = build_signing_input(ns, data, ttl, timestamp);
    REQUIRE(hash1 == hash2);

    // Different data produces different hash
    std::vector<uint8_t> data2 = {1, 2, 3, 4, 6};
    auto hash3 = build_signing_input(ns, data2, ttl, timestamp);
    REQUIRE(hash1 != hash3);
}

TEST_CASE("wire: build_signing_input golden vector", "[wire][golden]") {
    // HARDCODED cross-check: any drift between this digest and the value
    // baked below means CLI↔node canonical signing form has desynced.
    // Inputs: ns = {0x00 × 32}, data = {0x01, 0x02, 0x03}, ttl = 3600,
    // timestamp = 1700000000. Digest captured by building + running once on
    // x86_64 / liboqs SHA3 (Phase 124 plan 01, D-03).
    std::array<uint8_t, 32> ns{};   // all zero
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto digest = build_signing_input(ns, data, 3600, 1700000000);

    // SHA3-256(ns || data || ttl_be32 || ts_be64) = hex below.
    static const std::array<uint8_t, 32> expected = {
        0xE9, 0x0A, 0xB5, 0x35, 0x63, 0x1D, 0xB5, 0x4C,
        0xE4, 0xF0, 0x89, 0x2F, 0x10, 0x42, 0x6E, 0x72,
        0xCA, 0x14, 0x70, 0xF3, 0xFF, 0xEB, 0xD3, 0x08,
        0x38, 0x28, 0xD1, 0xC5, 0x06, 0x13, 0xDB, 0x89,
    };
    REQUIRE(digest == expected);
}

TEST_CASE("wire: make_tombstone_data format", "[wire]") {
    std::array<uint8_t, 32> target{};
    target.fill(0xBB);

    auto tombstone = make_tombstone_data(target);
    REQUIRE(tombstone.size() == 36);

    // Magic prefix
    REQUIRE(tombstone[0] == 0xDE);
    REQUIRE(tombstone[1] == 0xAD);
    REQUIRE(tombstone[2] == 0xBE);
    REQUIRE(tombstone[3] == 0xEF);

    // Target hash follows
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(tombstone[4 + i] == 0xBB);
    }
}

TEST_CASE("wire: hex roundtrip", "[wire]") {
    std::vector<uint8_t> bytes = {0x00, 0x0A, 0xFF, 0xDE, 0xAD};
    auto hex = to_hex(bytes);
    REQUIRE(hex == "000affdead");

    auto back = from_hex(hex);
    REQUIRE(back.has_value());
    REQUIRE(*back == bytes);
}

TEST_CASE("wire: hex uppercase input accepted", "[wire]") {
    auto result = from_hex("DEADBEEF");
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 4);
    REQUIRE((*result)[0] == 0xDE);
    REQUIRE((*result)[1] == 0xAD);
    REQUIRE((*result)[2] == 0xBE);
    REQUIRE((*result)[3] == 0xEF);
}

TEST_CASE("wire: from_hex rejects odd length", "[wire]") {
    auto result = from_hex("abc");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("wire: from_hex rejects invalid chars", "[wire]") {
    auto result = from_hex("zz");
    REQUIRE_FALSE(result.has_value());
}

// =============================================================================
// D-03 build_owned_blob + D-04 encode_blob_write_body coverage
// =============================================================================

// Test-local BlobWriteBody vtable constants (wire.cpp keeps them file-local).
namespace bwb_test_vt {
    constexpr flatbuffers::voffset_t TARGET_NAMESPACE = 4;
    constexpr flatbuffers::voffset_t BLOB             = 6;
}
namespace blob_test_vt {
    constexpr flatbuffers::voffset_t SIGNER_HINT = 4;
    constexpr flatbuffers::voffset_t DATA        = 6;
    constexpr flatbuffers::voffset_t TTL         = 8;
    constexpr flatbuffers::voffset_t TIMESTAMP   = 10;
    constexpr flatbuffers::voffset_t SIGNATURE   = 12;
}

TEST_CASE("wire: build_owned_blob owner sets signer_hint == id.namespace_id()", "[wire]") {
    auto id = Identity::generate();
    std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
    auto ns_span = id.namespace_id();

    auto ns_blob = build_owned_blob(id, ns_span, data, 3600, 1700000000);

    // signer_hint MUST equal SHA3(signing_pubkey) == id.namespace_id() for owner writes.
    auto own_ns = id.namespace_id();
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns_blob.blob.signer_hint[i] == own_ns[i]);
    }
    // Target namespace round-trips verbatim.
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns_blob.target_namespace[i] == own_ns[i]);
    }
    REQUIRE(ns_blob.blob.data == data);
    REQUIRE(ns_blob.blob.ttl == 3600u);
    REQUIRE(ns_blob.blob.timestamp == 1700000000ull);
    REQUIRE(ns_blob.blob.signature.size() > 0);
}

TEST_CASE("wire: build_owned_blob delegate -- signer_hint differs from target_namespace", "[wire]") {
    auto id = Identity::generate();
    // Delegate write: target_namespace is NOT id.namespace_id().
    std::array<uint8_t, 32> other_ns{};
    other_ns.fill(0x42);
    std::vector<uint8_t> data = {0x01, 0x02};

    auto ns_blob = build_owned_blob(
        id,
        std::span<const uint8_t, 32>(other_ns.data(), 32),
        data, 0, 1700000001);

    // target_namespace faithfully echoes the caller-supplied value.
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns_blob.target_namespace[i] == 0x42);
    }
    // signer_hint is forced to SHA3(own_signing_pk) — MUST NOT equal target_namespace.
    // Structural T-124-02 mitigation: delegates cannot impersonate the owner.
    bool differs = false;
    for (size_t i = 0; i < 32; ++i) {
        if (ns_blob.blob.signer_hint[i] != ns_blob.target_namespace[i]) {
            differs = true;
            break;
        }
    }
    REQUIRE(differs);
    // And signer_hint DOES equal id.namespace_id() (== SHA3(own_sp)).
    auto own_ns = id.namespace_id();
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns_blob.blob.signer_hint[i] == own_ns[i]);
    }
}

TEST_CASE("wire: build_owned_blob signature verifies via OQS_SIG_verify", "[wire]") {
    auto id = Identity::generate();
    std::array<uint8_t, 32> target_ns{};
    target_ns.fill(0x11);
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    uint32_t ttl = 0;
    uint64_t ts = 1700000002;

    auto ns_blob = build_owned_blob(
        id,
        std::span<const uint8_t, 32>(target_ns.data(), 32),
        data, ttl, ts);

    // Reconstruct the canonical signing input and verify the signature.
    auto digest = build_signing_input(
        std::span<const uint8_t, 32>(target_ns.data(), 32),
        data, ttl, ts);

    OQS_SIG* sig_alg = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    REQUIRE(sig_alg != nullptr);
    auto sp = id.signing_pubkey();
    OQS_STATUS rc = OQS_SIG_verify(
        sig_alg,
        digest.data(), digest.size(),
        ns_blob.blob.signature.data(), ns_blob.blob.signature.size(),
        sp.data());
    OQS_SIG_free(sig_alg);
    REQUIRE(rc == OQS_SUCCESS);
}

TEST_CASE("wire: encode_blob_write_body roundtrip", "[wire]") {
    std::array<uint8_t, 32> ns{};
    ns.fill(0x11);
    BlobData blob{};
    blob.signer_hint.fill(0x22);
    blob.data = {0x01, 0x02};
    blob.ttl = 0;
    blob.timestamp = 0;
    blob.signature.resize(100, 0x33);

    auto bytes = encode_blob_write_body(
        std::span<const uint8_t, 32>(ns.data(), 32), blob);
    REQUIRE(!bytes.empty());

    // Walk the outer BlobWriteBody table by hand.
    const auto* outer = flatbuffers::GetRoot<flatbuffers::Table>(bytes.data());
    REQUIRE(outer != nullptr);

    const auto* ns_vec = outer->GetPointer<const flatbuffers::Vector<uint8_t>*>(
        bwb_test_vt::TARGET_NAMESPACE);
    REQUIRE(ns_vec != nullptr);
    REQUIRE(ns_vec->size() == 32u);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(ns_vec->Get(i) == 0x11);
    }

    const auto* inner = outer->GetPointer<const flatbuffers::Table*>(
        bwb_test_vt::BLOB);
    REQUIRE(inner != nullptr);

    const auto* sh = inner->GetPointer<const flatbuffers::Vector<uint8_t>*>(
        blob_test_vt::SIGNER_HINT);
    REQUIRE(sh != nullptr);
    REQUIRE(sh->size() == 32u);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(sh->Get(i) == 0x22);
    }

    const auto* dt = inner->GetPointer<const flatbuffers::Vector<uint8_t>*>(
        blob_test_vt::DATA);
    REQUIRE(dt != nullptr);
    REQUIRE(dt->size() == 2u);
    REQUIRE(dt->Get(0) == 0x01);
    REQUIRE(dt->Get(1) == 0x02);

    REQUIRE(inner->GetField<uint32_t>(blob_test_vt::TTL, 0xFFFFFFFFu) == 0u);
    REQUIRE(inner->GetField<uint64_t>(blob_test_vt::TIMESTAMP, 0xFFFFFFFFFFFFFFFFull) == 0ull);

    const auto* sig = inner->GetPointer<const flatbuffers::Vector<uint8_t>*>(
        blob_test_vt::SIGNATURE);
    REQUIRE(sig != nullptr);
    REQUIRE(sig->size() == 100u);
    for (size_t i = 0; i < 100; ++i) {
        REQUIRE(sig->Get(i) == 0x33);
    }
}

TEST_CASE("wire: make_bomb_data roundtrip", "[wire]") {
    std::array<std::array<uint8_t, 32>, 3> targets{};
    targets[0].fill(0x11);
    targets[1].fill(0x22);
    targets[2].fill(0x33);

    auto payload = make_bomb_data(
        std::span<const std::array<uint8_t, 32>>(targets.data(), 3));
    // Layout: [BOMB:4][count:4 BE][hash:32] * count = 4 + 4 + 96 = 104 bytes.
    REQUIRE(payload.size() == 104u);

    // Magic
    REQUIRE(payload[0] == 0x42);  // 'B'
    REQUIRE(payload[1] == 0x4F);  // 'O'
    REQUIRE(payload[2] == 0x4D);  // 'M'
    REQUIRE(payload[3] == 0x42);  // 'B'

    // Count = 3 big-endian
    REQUIRE(load_u32_be(payload.data() + 4) == 3u);

    // Three 32-byte hashes concatenated
    for (size_t i = 0; i < 32; ++i) REQUIRE(payload[8 + i]       == 0x11);
    for (size_t i = 0; i < 32; ++i) REQUIRE(payload[8 + 32 + i]  == 0x22);
    for (size_t i = 0; i < 32; ++i) REQUIRE(payload[8 + 64 + i]  == 0x33);
}

TEST_CASE("wire: parse_name_payload roundtrip", "[wire]") {
    std::array<uint8_t, 32> target{};
    target.fill(0x77);
    std::string name = "foo";
    std::span<const uint8_t> name_span(
        reinterpret_cast<const uint8_t*>(name.data()), name.size());

    auto payload = make_name_data(
        name_span, std::span<const uint8_t, 32>(target.data(), 32));

    auto parsed = parse_name_payload(payload);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name.size() == name.size());
    REQUIRE(std::memcmp(parsed->name.data(), name.data(), name.size()) == 0);
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(parsed->target_hash[i] == 0x77);
    }

    // Flip magic byte 0 -> parse must reject.
    auto corrupted = payload;
    corrupted[0] ^= 0xFF;
    auto bad = parse_name_payload(corrupted);
    REQUIRE_FALSE(bad.has_value());
}

// =============================================================================
// D-06 classify_rm_target_impl coverage ([cascade] tag)
// =============================================================================
//
// Tests call the TEMPLATE form directly with a CapturingSender +
// ScriptedSource so no real asio::io_context is involved (mirrors plan 02's
// ensure_pubk_impl test pattern).

namespace {

// CapturingSender for [cascade] tests. Same shape as test_auto_pubk.cpp's
// copy but local to this file to avoid a new shared header for one more
// consumer — pipeline_test_support.h is the natural promotion target if a
// third consumer ever appears.
struct CascadeSender {
    std::vector<std::tuple<MsgType, std::vector<uint8_t>, uint32_t>> calls;
    bool ok = true;

    bool operator()(MsgType t, std::span<const uint8_t> pl, uint32_t rid) {
        if (!ok) return false;
        calls.emplace_back(t, std::vector<uint8_t>(pl.begin(), pl.end()), rid);
        return true;
    }
};

// Build a ReadResponse payload as the classification helper expects it:
//   [0]      status byte (0x01 = hit)
//   [1..]    encoded Blob FlatBuffer
// The encoded Blob wraps `blob_data` (which includes the outer magic prefix
// for CDAT/CPAR cases) via the standard `build_owned_blob` + `encode_blob`
// pipeline used on the production write side.
std::vector<uint8_t> read_response_payload_for_blob(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t> blob_data) {
    auto ns_blob  = build_owned_blob(id, ns, blob_data, 0, 1700000000);
    auto encoded  = encode_blob(ns_blob.blob);
    std::vector<uint8_t> out;
    out.reserve(1 + encoded.size());
    out.push_back(0x01);  // status: hit
    out.insert(out.end(), encoded.begin(), encoded.end());
    return out;
}

} // namespace

TEST_CASE("cascade: classify_rm_target_impl returns Plain for non-chunked target", "[cascade]") {
    auto id = Identity::generate();
    auto own_ns = id.namespace_id();
    std::array<uint8_t, 32> target_hash{};
    target_hash.fill(0x42);

    // Non-CDAT/non-CPAR data: 32 bytes of 0x42 — does not start with any
    // recognised magic prefix.
    std::vector<uint8_t> plain_data(32, 0x42);
    auto read_payload = read_response_payload_for_blob(
        id, std::span<const uint8_t, 32>(own_ns.data(), 32),
        std::span<const uint8_t>(plain_data));

    ScriptedSource src;
    src.queue.push_back(make_reply(
        /*rid=*/0,
        static_cast<uint8_t>(MsgType::ReadResponse),
        read_payload));
    CascadeSender sender;
    uint32_t rid = 0;
    auto recv_fn = [&] { return src(); };

    auto rc = classify_rm_target_impl(
        id,
        std::span<const uint8_t, 32>(own_ns.data(), 32),
        std::span<const uint8_t, 32>(target_hash.data(), 32),
        sender, recv_fn, rid);

    REQUIRE(rc.kind == RmClassification::Kind::Plain);
    REQUIRE(rc.cascade_targets.empty());
    // Exactly one outbound call: the ReadRequest probe.
    REQUIRE(sender.calls.size() == 1);
    REQUIRE(std::get<0>(sender.calls[0]) == MsgType::ReadRequest);
    REQUIRE(rid == 1u);
}

TEST_CASE("cascade: classify_rm_target_impl expands CPAR manifest into chunk hashes", "[cascade]") {
    auto id = Identity::generate();
    auto own_ns = id.namespace_id();
    std::array<uint8_t, 32> target_hash{};
    target_hash.fill(0x55);

    // Two fake chunk hashes — the classifier must extract these verbatim.
    std::array<uint8_t, 32> c1{}; c1.fill(0x11);
    std::array<uint8_t, 32> c2{}; c2.fill(0x22);

    // Build a minimal CPAR manifest payload via the production helpers.
    ManifestData m;
    m.version               = MANIFEST_VERSION_V1;
    m.chunk_size_bytes      = CHUNK_SIZE_BYTES_DEFAULT;
    m.segment_count         = 2;
    m.total_plaintext_bytes = 1024;
    m.plaintext_sha3.fill(0xAA);
    m.chunk_hashes.reserve(64);
    m.chunk_hashes.insert(m.chunk_hashes.end(), c1.begin(), c1.end());
    m.chunk_hashes.insert(m.chunk_hashes.end(), c2.begin(), c2.end());
    m.filename = "test.bin";

    auto manifest_payload = encode_manifest_payload(m);  // [CPAR magic][FB]

    // Envelope-encrypt to self (the classifier will decrypt with
    // id.kem_seckey() / id.kem_pubkey()).
    std::vector<std::span<const uint8_t>> recips;
    auto self_kem_pk = id.kem_pubkey();
    recips.emplace_back(self_kem_pk);
    auto cenv = envelope::encrypt(manifest_payload, recips);

    // Outer blob.data: [CPAR:4] + CENV-envelope bytes.
    std::vector<uint8_t> blob_data;
    blob_data.reserve(4 + cenv.size());
    blob_data.insert(blob_data.end(), CPAR_MAGIC.begin(), CPAR_MAGIC.end());
    blob_data.insert(blob_data.end(), cenv.begin(), cenv.end());

    auto read_payload = read_response_payload_for_blob(
        id, std::span<const uint8_t, 32>(own_ns.data(), 32),
        std::span<const uint8_t>(blob_data));

    ScriptedSource src;
    src.queue.push_back(make_reply(
        /*rid=*/0,
        static_cast<uint8_t>(MsgType::ReadResponse),
        read_payload));
    CascadeSender sender;
    uint32_t rid = 0;
    auto recv_fn = [&] { return src(); };

    auto rc = classify_rm_target_impl(
        id,
        std::span<const uint8_t, 32>(own_ns.data(), 32),
        std::span<const uint8_t, 32>(target_hash.data(), 32),
        sender, recv_fn, rid);

    REQUIRE(rc.kind == RmClassification::Kind::CparWithChunks);
    REQUIRE(rc.cascade_targets.size() == 2);
    REQUIRE(rc.cascade_targets[0] == c1);
    REQUIRE(rc.cascade_targets[1] == c2);
    REQUIRE(sender.calls.size() == 1);
    REQUIRE(std::get<0>(sender.calls[0]) == MsgType::ReadRequest);
    REQUIRE(rid == 1u);
}
