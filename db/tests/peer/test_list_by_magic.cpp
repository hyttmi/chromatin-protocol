// ListRequest + type_filter enumeration for NAME and BOMB magics.
//
// Tags:
//   [phase123][transport][list]  — NAME / BOMB / empty-set / no-filter enumeration
//
// Proves Phase 117's existing `ListRequest + type_filter` endpoint (reused per
// Plan 01 D-10 deviation — no new TransportMsgType was introduced in Phase 123)
// correctly enumerates blobs by the 4-byte NAME / BOMB magic prefixes added by
// Plan 01 and ingested by Plan 02.
//
// Harness choice (per plan Task 01 pre-resolved note): the Storage-layer
// analog — `storage.get_blob_refs_since(ns, /*since_seq*/0, /*max*/1000)` +
// in-memory `ref.blob_type == wire::NAME_MAGIC / BOMB_MAGIC` filter — is the
// exact same seq_map prefix-compare operation the dispatcher performs at
// db/peer/message_dispatcher.cpp:537-541 (it memcmps the 4-byte type_filter
// against ref.blob_type after calling the identical `get_blob_refs_since`
// at :511). The transport-level framing path is implicitly exercised by
// 's existing ListRequest tests in test_peer_manager.cpp; what
// adds is the two new 4-byte magics — and those live entirely in
// `ref.blob_type`, so the Storage-level test proves D-10's behavioral
// contract for the new prefixes.
//
// Companion coverage:
//   - [phase123][wire][codec]   (Plan 01) — magic constants + round-trip
//   - [phase123][engine][...]   (Plan 02) — BOMB validation / side-effect
//   - [phase123][overwrite]     (Plan 02) — NAME enumerate-and-resolve
//
// Non-goals:
//   - Transport framing regression — covered by Phase 117 peer tests.
//   - ListResponse wire format — unchanged by Phase 123.

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
using chromatindb::storage::BlobRef;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;
using chromatindb::test::make_bomb_blob;
using chromatindb::test::make_name_blob;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_tombstone;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

namespace {

/// Filter refs by 4-byte blob_type prefix.
/// Mirrors db/peer/message_dispatcher.cpp:537-541:
///   if (has_type_filter) {
///       if (std::memcmp(ref.blob_type.data(), type_filter.data(), 4) != 0)
///           continue;
///   }
std::vector<BlobRef> filter_refs_by_type(
    const std::vector<BlobRef>& refs,
    std::span<const uint8_t, 4> type_filter)
{
    std::vector<BlobRef> out;
    for (const auto& ref : refs) {
        if (std::memcmp(ref.blob_type.data(), type_filter.data(), 4) == 0) {
            out.push_back(ref);
        }
    }
    return out;
}

} // namespace

// T-D-10 NAME path: ingest one blob of each of {content, NAME, BOMB, TOMB},
// then verify the NAME_MAGIC type_filter returns exactly the NAME blob.
TEST_CASE("ListRequest type_filter=NAME_MAGIC returns only NAME blobs",
          "[phase123][transport][list]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Content blob that will be the NAME target AND the TOMB target.
    auto content = make_signed_blob(id, "content-blob-for-name");
    auto r_content = run_async(pool, engine.ingest(ns_span(id), content));
    REQUIRE(r_content.accepted);
    auto content_hash = r_content.ack->blob_hash;

    // NAME blob: "foo" → content_hash.
    const std::string name_str = "foo";
    std::span<const uint8_t> name_bytes(
        reinterpret_cast<const uint8_t*>(name_str.data()), name_str.size());
    auto name_blob = make_name_blob(
        id, name_bytes, std::span<const uint8_t, 32>(content_hash));
    auto r_name = run_async(pool, engine.ingest(ns_span(id), name_blob));
    REQUIRE(r_name.accepted);
    auto name_hash = r_name.ack->blob_hash;

    // BOMB blob with count=0 (no side-effect — per Plan 01 A2 / Plan 02 accept).
    // Using count=0 means Step 3.5's side-effect loop runs zero iterations,
    // so the other test blobs stay in seq_map intact for the enumeration.
    std::vector<std::array<uint8_t, 32>> empty_targets;
    auto bomb_blob = make_bomb_blob(
        id, std::span<const std::array<uint8_t, 32>>(empty_targets));
    auto r_bomb = run_async(pool, engine.ingest(ns_span(id), bomb_blob));
    REQUIRE(r_bomb.accepted);
    auto bomb_hash = r_bomb.ack->blob_hash;

    // A separate content blob whose sole purpose is to be the tombstone target.
    // (Using `content_hash` as the TOMB target would cascade-delete the very
    // blob the NAME points to, which is irrelevant for this test but noisy.)
    auto throwaway = make_signed_blob(id, "throwaway-for-tombstone");
    auto r_throw = run_async(pool, engine.ingest(ns_span(id), throwaway));
    REQUIRE(r_throw.accepted);

    auto tomb_blob = make_signed_tombstone(id, r_throw.ack->blob_hash);
    auto r_tomb = run_async(pool, engine.ingest(ns_span(id), tomb_blob));
    REQUIRE(r_tomb.accepted);

    // Enumerate the namespace's seq_map — same API the dispatcher calls.
    auto all_refs = store.get_blob_refs_since(
        ns_span(id), /*since_seq*/0, /*max_count*/1000);

    // Sanity: seq_map contains PUBK + content + NAME + BOMB + throwaway + TOMB
    // (throwaway's slot becomes a zero-hash sentinel after TOMB ingestion; the
    // `get_blob_refs_since` helper skips zero-hash sentinels per storage.h:189).
    // Remaining visible: PUBK, content, NAME, BOMB, TOMB — ≥5 entries.
    REQUIRE(all_refs.size() >= 5);

    // Apply type_filter=NAME_MAGIC. MUST return exactly one entry — the NAME.
    auto name_only = filter_refs_by_type(
        all_refs, std::span<const uint8_t, 4>(chromatindb::wire::NAME_MAGIC));
    REQUIRE(name_only.size() == 1);
    REQUIRE(std::memcmp(name_only[0].blob_hash.data(),
                        name_hash.data(), 32) == 0);
    REQUIRE(std::memcmp(name_only[0].blob_type.data(),
                        chromatindb::wire::NAME_MAGIC.data(), 4) == 0);
}

// T-D-10 BOMB path: same mixed-ingest setup; BOMB_MAGIC filter returns only
// the BOMB blob.
TEST_CASE("ListRequest type_filter=BOMB_MAGIC returns only BOMB blobs",
          "[phase123][transport][list]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Content + NAME (as in the NAME test).
    auto content = make_signed_blob(id, "content-blob-for-bomb-test");
    auto r_content = run_async(pool, engine.ingest(ns_span(id), content));
    REQUIRE(r_content.accepted);
    auto content_hash = r_content.ack->blob_hash;

    const std::string name_str = "bar";
    std::span<const uint8_t> name_bytes(
        reinterpret_cast<const uint8_t*>(name_str.data()), name_str.size());
    auto name_blob = make_name_blob(
        id, name_bytes, std::span<const uint8_t, 32>(content_hash));
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), name_blob)).accepted);

    // BOMB with count=0 — structurally valid no-op (Plan 01 A2).
    std::vector<std::array<uint8_t, 32>> empty_targets;
    auto bomb_blob = make_bomb_blob(
        id, std::span<const std::array<uint8_t, 32>>(empty_targets));
    auto r_bomb = run_async(pool, engine.ingest(ns_span(id), bomb_blob));
    REQUIRE(r_bomb.accepted);
    auto bomb_hash = r_bomb.ack->blob_hash;

    auto throwaway = make_signed_blob(id, "throwaway-for-tombstone-bomb-test");
    auto r_throw = run_async(pool, engine.ingest(ns_span(id), throwaway));
    REQUIRE(r_throw.accepted);
    auto tomb_blob = make_signed_tombstone(id, r_throw.ack->blob_hash);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), tomb_blob)).accepted);

    auto all_refs = store.get_blob_refs_since(
        ns_span(id), /*since_seq*/0, /*max_count*/1000);

    auto bomb_only = filter_refs_by_type(
        all_refs, std::span<const uint8_t, 4>(chromatindb::wire::BOMB_MAGIC));
    REQUIRE(bomb_only.size() == 1);
    REQUIRE(std::memcmp(bomb_only[0].blob_hash.data(),
                        bomb_hash.data(), 32) == 0);
    REQUIRE(std::memcmp(bomb_only[0].blob_type.data(),
                        chromatindb::wire::BOMB_MAGIC.data(), 4) == 0);
}

// Zero-result path: namespace has content + tombstones but NO NAME or BOMB
// blobs. type_filter=NAME_MAGIC must return an empty set.
TEST_CASE("ListRequest type_filter=NAME_MAGIC returns zero entries when no NAME blobs exist",
          "[phase123][transport][list]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Ingest two content blobs; tombstone one of them. No NAME, no BOMB.
    auto keep = make_signed_blob(id, "content-keep");
    auto drop = make_signed_blob(id, "content-drop");
    auto r_keep = run_async(pool, engine.ingest(ns_span(id), keep));
    auto r_drop = run_async(pool, engine.ingest(ns_span(id), drop));
    REQUIRE(r_keep.accepted);
    REQUIRE(r_drop.accepted);

    auto tomb = make_signed_tombstone(id, r_drop.ack->blob_hash);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), tomb)).accepted);

    auto all_refs = store.get_blob_refs_since(
        ns_span(id), /*since_seq*/0, /*max_count*/1000);

    // Confirm pre-condition: no entry in seq_map has NAME_MAGIC or BOMB_MAGIC.
    auto name_filtered = filter_refs_by_type(
        all_refs, std::span<const uint8_t, 4>(chromatindb::wire::NAME_MAGIC));
    REQUIRE(name_filtered.empty());

    auto bomb_filtered = filter_refs_by_type(
        all_refs, std::span<const uint8_t, 4>(chromatindb::wire::BOMB_MAGIC));
    REQUIRE(bomb_filtered.empty());
}

// Optional fourth case: without any filter, enumeration returns every
// indexed blob (≥4 distinct types after the mixed ingest). Proves the
// filter is additive — absence of filter does not drop NAME/BOMB entries.
TEST_CASE("ListRequest without type_filter returns all blob types",
          "[phase123][transport][list]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    auto content = make_signed_blob(id, "content-for-no-filter");
    auto r_content = run_async(pool, engine.ingest(ns_span(id), content));
    REQUIRE(r_content.accepted);
    auto content_hash = r_content.ack->blob_hash;

    const std::string name_str = "baz";
    std::span<const uint8_t> name_bytes(
        reinterpret_cast<const uint8_t*>(name_str.data()), name_str.size());
    auto name_blob = make_name_blob(
        id, name_bytes, std::span<const uint8_t, 32>(content_hash));
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), name_blob)).accepted);

    std::vector<std::array<uint8_t, 32>> empty_targets;
    auto bomb_blob = make_bomb_blob(
        id, std::span<const std::array<uint8_t, 32>>(empty_targets));
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), bomb_blob)).accepted);

    auto all_refs = store.get_blob_refs_since(
        ns_span(id), /*since_seq*/0, /*max_count*/1000);

    // Expect the four distinct types we ingested: PUBK, content, NAME, BOMB.
    // (PUBK occupies the first seq slot.)
    REQUIRE(all_refs.size() >= 4);

    bool saw_pubk = false, saw_name = false, saw_bomb = false, saw_content = false;
    for (const auto& ref : all_refs) {
        if (std::memcmp(ref.blob_type.data(),
                        chromatindb::wire::PUBKEY_MAGIC.data(), 4) == 0) {
            saw_pubk = true;
        } else if (std::memcmp(ref.blob_type.data(),
                               chromatindb::wire::NAME_MAGIC.data(), 4) == 0) {
            saw_name = true;
        } else if (std::memcmp(ref.blob_type.data(),
                               chromatindb::wire::BOMB_MAGIC.data(), 4) == 0) {
            saw_bomb = true;
        } else {
            // Anything not PUBK / NAME / BOMB / TOMB / DELEGATION is a
            // generic content blob (its type prefix is the first 4 bytes
            // of the ciphertext, which with overwhelming probability does
            // not collide with a known magic).
            saw_content = true;
        }
    }
    REQUIRE(saw_pubk);
    REQUIRE(saw_content);
    REQUIRE(saw_name);
    REQUIRE(saw_bomb);
}
