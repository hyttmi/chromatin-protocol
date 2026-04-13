#include <catch2/catch_test_macros.hpp>
#include "relay/core/write_tracker.h"

using chromatindb::relay::core::BlobHash32;
using chromatindb::relay::core::WriteTracker;

namespace {

/// Helper: create a BlobHash32 with a distinguishing byte at index 0.
BlobHash32 make_hash(uint8_t id) {
    BlobHash32 h{};
    h[0] = id;
    return h;
}

} // anonymous namespace

TEST_CASE("WriteTracker: record and lookup returns session_id", "[write_tracker]") {
    WriteTracker tracker;
    auto hash = make_hash(1);

    tracker.record(hash, 42);
    auto result = tracker.lookup_and_remove(hash);

    REQUIRE(result.has_value());
    CHECK(result.value() == 42);
}

TEST_CASE("WriteTracker: lookup_and_remove returns nullopt for unknown hash", "[write_tracker]") {
    WriteTracker tracker;
    auto hash = make_hash(99);

    auto result = tracker.lookup_and_remove(hash);

    CHECK_FALSE(result.has_value());
}

TEST_CASE("WriteTracker: lookup_and_remove removes the entry", "[write_tracker]") {
    WriteTracker tracker;
    auto hash = make_hash(2);

    tracker.record(hash, 10);

    // First lookup succeeds and removes
    auto first = tracker.lookup_and_remove(hash);
    REQUIRE(first.has_value());
    CHECK(first.value() == 10);

    // Second lookup returns nullopt -- entry was removed
    auto second = tracker.lookup_and_remove(hash);
    CHECK_FALSE(second.has_value());
}

TEST_CASE("WriteTracker: remove_session purges all entries for session", "[write_tracker]") {
    WriteTracker tracker;

    auto h1 = make_hash(10);
    auto h2 = make_hash(11);
    auto h3 = make_hash(12);

    // Two entries for session 10, one for session 20
    tracker.record(h1, 10);
    tracker.record(h2, 10);
    tracker.record(h3, 20);
    CHECK(tracker.size() == 3);

    tracker.remove_session(10);
    CHECK(tracker.size() == 1);

    // Session 10's entries are gone
    CHECK_FALSE(tracker.lookup_and_remove(h1).has_value());
    CHECK_FALSE(tracker.lookup_and_remove(h2).has_value());

    // Session 20's entry remains (re-add since lookup removes)
    // Actually h3 was already there, let's check it
    // Note: previous lookup_and_remove calls for h1/h2 returned nullopt (already removed)
    // h3 should still be findable -- but we need to check without removing to keep size check
    // Just verify via lookup_and_remove:
    auto result = tracker.lookup_and_remove(h3);
    REQUIRE(result.has_value());
    CHECK(result.value() == 20);
}

TEST_CASE("WriteTracker: record overwrites existing entry for same hash", "[write_tracker]") {
    WriteTracker tracker;
    auto hash = make_hash(5);

    tracker.record(hash, 1);
    tracker.record(hash, 2);

    CHECK(tracker.size() == 1);

    auto result = tracker.lookup_and_remove(hash);
    REQUIRE(result.has_value());
    CHECK(result.value() == 2);
}

TEST_CASE("WriteTracker: size reflects current entries", "[write_tracker]") {
    WriteTracker tracker;
    CHECK(tracker.size() == 0);

    auto h1 = make_hash(20);
    auto h2 = make_hash(21);
    auto h3 = make_hash(22);

    tracker.record(h1, 1);
    CHECK(tracker.size() == 1);

    tracker.record(h2, 2);
    CHECK(tracker.size() == 2);

    tracker.record(h3, 3);
    CHECK(tracker.size() == 3);

    // lookup_and_remove decreases size
    tracker.lookup_and_remove(h1);
    CHECK(tracker.size() == 2);

    // remove_session decreases size
    tracker.remove_session(2);
    CHECK(tracker.size() == 1);
}

TEST_CASE("WriteTracker: remove_session with no matching entries is a no-op", "[write_tracker]") {
    WriteTracker tracker;
    auto hash = make_hash(30);

    tracker.record(hash, 100);
    CHECK(tracker.size() == 1);

    // Remove session that doesn't exist -- should be no-op
    tracker.remove_session(999);
    CHECK(tracker.size() == 1);

    // Original entry still present
    auto result = tracker.lookup_and_remove(hash);
    REQUIRE(result.has_value());
    CHECK(result.value() == 100);
}
