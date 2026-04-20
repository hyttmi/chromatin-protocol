// Phase 123-02: NAME overwrite resolution (D-01 timestamp, D-02 content_hash tiebreak).
//
// Tags:
//   [phase123][overwrite]            — higher-timestamp wins
//   [phase123][overwrite][tiebreak]  — equal-timestamp content_hash DESC tiebreak
//
// Proves: deterministic NAME resolution over an enumerate-and-sort flow
// (D-09 stateless CLI + D-10 reuse of ListRequest+type_filter at node).
// The sort order is the CLI's resolution rule:
//   (blob.timestamp DESC, content_hash DESC)

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"

using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;
using chromatindb::test::make_name_blob;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

namespace {

/// NAME candidate as an enumerate-and-resolve tuple:
///   (outer blob.timestamp, stored blob's content_hash, parsed target_hash)
/// Mirrors what the CLI would compute over the wire.
struct NameCandidate {
    uint64_t timestamp;
    std::array<uint8_t, 32> content_hash;
    std::array<uint8_t, 32> target_hash;
};

/// Enumerate NAME blobs in a namespace by walking the seq_map, filtering
/// ref.blob_type == NAME_MAGIC, and pulling the full blob to read timestamp
/// + parsed NAME payload. This is the Storage-layer analog of the CLI's
/// ListRequest+type_filter=NAME_MAGIC enumeration.
std::vector<NameCandidate> enumerate_name_candidates(
    Storage& store,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t> name_query)
{
    std::vector<NameCandidate> candidates;
    auto refs = store.get_blob_refs_since(ns, /*since_seq*/0, /*max_count*/4096);
    for (const auto& ref : refs) {
        if (ref.blob_type != chromatindb::wire::NAME_MAGIC) continue;
        auto blob = store.get_blob(
            ns, std::span<const uint8_t, 32>(ref.blob_hash));
        if (!blob.has_value()) continue;
        auto parsed = chromatindb::wire::parse_name_payload(blob->data);
        if (!parsed.has_value()) continue;
        // Only consider candidates whose `name` matches the query (opaque bytes).
        if (parsed->name.size() != name_query.size()) continue;
        if (std::memcmp(parsed->name.data(), name_query.data(),
                        name_query.size()) != 0) continue;

        NameCandidate c;
        c.timestamp = blob->timestamp;
        c.content_hash = ref.blob_hash;
        c.target_hash = parsed->target_hash;
        candidates.push_back(c);
    }
    return candidates;
}

/// Apply the D-01 + D-02 resolution rule: pick the candidate with the
/// highest blob.timestamp; tiebreak on content_hash lexicographically DESC.
std::optional<NameCandidate> resolve_name(std::vector<NameCandidate> cands) {
    if (cands.empty()) return std::nullopt;
    std::sort(cands.begin(), cands.end(),
              [](const NameCandidate& a, const NameCandidate& b) {
                  if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
                  // content_hash DESC (lex-greater wins)
                  return std::memcmp(a.content_hash.data(),
                                     b.content_hash.data(), 32) > 0;
              });
    return cands.front();
}

} // namespace

// D-01: Among NAME blobs with the same `name`, the blob with the highest
// outer `blob.timestamp` wins. A later NAME overwrites an earlier one.
TEST_CASE("NAME with higher timestamp wins",
          "[phase123][overwrite]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Two content targets — the NAME at ts=2000 points to `hash_b`.
    auto content_a = make_signed_blob(id, "content-A-for-name");
    auto content_b = make_signed_blob(id, "content-B-for-name");
    auto r_a = run_async(pool, engine.ingest(ns_span(id), content_a));
    auto r_b = run_async(pool, engine.ingest(ns_span(id), content_b));
    REQUIRE(r_a.accepted); REQUIRE(r_b.accepted);
    std::array<uint8_t, 32> hash_a = r_a.ack->blob_hash;
    std::array<uint8_t, 32> hash_b = r_b.ack->blob_hash;

    const std::string name_str = "foo";
    std::span<const uint8_t> name_bytes(
        reinterpret_cast<const uint8_t*>(name_str.data()), name_str.size());

    // Earlier NAME: foo → hash_a at ts=1000.
    auto name1 = make_name_blob(
        id, name_bytes, std::span<const uint8_t, 32>(hash_a),
        /*ttl*/0, /*ts*/1000);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), name1)).accepted);

    // Later NAME: foo → hash_b at ts=2000.
    auto name2 = make_name_blob(
        id, name_bytes, std::span<const uint8_t, 32>(hash_b),
        /*ttl*/0, /*ts*/2000);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), name2)).accepted);

    auto cands = enumerate_name_candidates(store, ns_span(id), name_bytes);
    REQUIRE(cands.size() == 2);
    auto winner = resolve_name(cands);
    REQUIRE(winner.has_value());
    REQUIRE(winner->timestamp == 2000);
    REQUIRE(std::memcmp(winner->target_hash.data(), hash_b.data(), 32) == 0);
}

// D-02: Two NAME blobs with IDENTICAL timestamps tiebreak on the outer
// blob's `content_hash` lexicographically DESC. The lex-greater hash wins,
// deterministically for all readers.
TEST_CASE("Equal timestamps tiebreak on content_hash DESC",
          "[phase123][overwrite][tiebreak]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    auto content_1 = make_signed_blob(id, "tiebreak-target-one");
    auto content_2 = make_signed_blob(id, "tiebreak-target-two");
    auto r_1 = run_async(pool, engine.ingest(ns_span(id), content_1));
    auto r_2 = run_async(pool, engine.ingest(ns_span(id), content_2));
    REQUIRE(r_1.accepted); REQUIRE(r_2.accepted);
    std::array<uint8_t, 32> hash_1 = r_1.ack->blob_hash;
    std::array<uint8_t, 32> hash_2 = r_2.ack->blob_hash;

    const std::string name_str = "bar";
    std::span<const uint8_t> name_bytes(
        reinterpret_cast<const uint8_t*>(name_str.data()), name_str.size());

    // Two NAME blobs at identical ts=1500. ML-DSA-87 non-determinism makes
    // their content_hashes differ, so D-02 tiebreak produces a deterministic
    // winner. We ingest both, then compute the expected winner from the
    // actual stored content_hashes — the test does NOT hardcode which body
    // wins, only that the resolution follows `content_hash DESC`.
    const uint64_t ts = 1500;
    auto n1 = make_name_blob(id, name_bytes,
                             std::span<const uint8_t, 32>(hash_1),
                             /*ttl*/0, /*ts*/ts);
    auto n2 = make_name_blob(id, name_bytes,
                             std::span<const uint8_t, 32>(hash_2),
                             /*ttl*/0, /*ts*/ts);

    auto nr1 = run_async(pool, engine.ingest(ns_span(id), n1));
    auto nr2 = run_async(pool, engine.ingest(ns_span(id), n2));
    REQUIRE(nr1.accepted); REQUIRE(nr2.accepted);
    auto ch1 = nr1.ack->blob_hash;
    auto ch2 = nr2.ack->blob_hash;

    // Tiebreak guard: require that the two NAME blobs produced distinct
    // content_hashes. If ML-DSA-87 ever produced identical bytes for two
    // distinct signings the assertion would flag it — but the project
    // memory states signatures are non-deterministic, so this holds.
    REQUIRE(std::memcmp(ch1.data(), ch2.data(), 32) != 0);

    // Expected winner: whichever content_hash is lex-greater, pointing to
    // the corresponding target.
    std::array<uint8_t, 32> expected_target;
    if (std::memcmp(ch1.data(), ch2.data(), 32) > 0) {
        expected_target = hash_1;
    } else {
        expected_target = hash_2;
    }

    auto cands = enumerate_name_candidates(store, ns_span(id), name_bytes);
    REQUIRE(cands.size() == 2);
    auto winner = resolve_name(cands);
    REQUIRE(winner.has_value());
    REQUIRE(winner->timestamp == ts);
    REQUIRE(std::memcmp(winner->target_hash.data(),
                        expected_target.data(), 32) == 0);
}
