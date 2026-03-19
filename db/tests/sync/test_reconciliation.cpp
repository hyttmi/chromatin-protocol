#include <catch2/catch_test_macros.hpp>

#include "db/sync/reconciliation.h"

#include <algorithm>
#include <random>
#include <set>

namespace {

using Hash32 = chromatindb::sync::Hash32;

/// Create a hash filled with a single byte value.
Hash32 make_hash(uint8_t fill) {
    Hash32 h;
    h.fill(fill);
    return h;
}

/// Generate n random 32-byte hashes, sorted lexicographically.
std::vector<Hash32> make_random_hashes(size_t n, uint32_t seed = 42) {
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<Hash32> hashes;
    hashes.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Hash32 h;
        for (auto& byte : h) {
            byte = static_cast<uint8_t>(dist(gen));
        }
        hashes.push_back(h);
    }
    std::sort(hashes.begin(), hashes.end());
    // Remove duplicates (extremely unlikely but be safe)
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    return hashes;
}

} // anonymous namespace

using namespace chromatindb::sync;

// ============================================================================
// XOR fingerprint
// ============================================================================

TEST_CASE("xor_fingerprint: empty range returns all-zero", "[reconciliation]") {
    std::vector<Hash32> hashes;
    auto fp = xor_fingerprint(hashes, 0, 0);
    REQUIRE(fp == Hash32{});
}

TEST_CASE("xor_fingerprint: single hash returns that hash", "[reconciliation]") {
    auto h = make_hash(0xAB);
    std::vector<Hash32> hashes = {h};
    auto fp = xor_fingerprint(hashes, 0, 1);
    REQUIRE(fp == h);
}

TEST_CASE("xor_fingerprint: two identical hashes cancel to zero", "[reconciliation]") {
    auto h = make_hash(0xFF);
    std::vector<Hash32> hashes = {h, h};
    auto fp = xor_fingerprint(hashes, 0, 2);
    REQUIRE(fp == Hash32{});
}

TEST_CASE("xor_fingerprint: known test vectors", "[reconciliation]") {
    auto h1 = make_hash(0x0F);  // 0x0F repeated
    auto h2 = make_hash(0xF0);  // 0xF0 repeated
    std::vector<Hash32> hashes = {h1, h2};
    auto fp = xor_fingerprint(hashes, 0, 2);
    // 0x0F ^ 0xF0 = 0xFF
    REQUIRE(fp == make_hash(0xFF));
}

TEST_CASE("xor_fingerprint: sub-range", "[reconciliation]") {
    auto h1 = make_hash(0x01);
    auto h2 = make_hash(0x02);
    auto h3 = make_hash(0x03);
    std::vector<Hash32> hashes = {h1, h2, h3};
    // Only XOR h2 (index 1..2)
    auto fp = xor_fingerprint(hashes, 1, 2);
    REQUIRE(fp == h2);
}

// ============================================================================
// range_indices
// ============================================================================

TEST_CASE("range_indices: full range", "[reconciliation]") {
    auto h1 = make_hash(0x10);
    auto h2 = make_hash(0x20);
    auto h3 = make_hash(0x30);
    std::vector<Hash32> sorted = {h1, h2, h3};

    Hash32 lower{};   // all zeros
    Hash32 upper;
    upper.fill(0xFF); // max
    auto [begin_idx, end_idx] = range_indices(sorted, lower, upper);
    REQUIRE(begin_idx == 0);
    REQUIRE(end_idx == 3);
}

TEST_CASE("range_indices: sub-range", "[reconciliation]") {
    auto h1 = make_hash(0x10);
    auto h2 = make_hash(0x20);
    auto h3 = make_hash(0x30);
    std::vector<Hash32> sorted = {h1, h2, h3};

    // Range [0x15..., 0x25...) should include only h2
    Hash32 lower;
    lower.fill(0x15);
    Hash32 upper;
    upper.fill(0x25);
    auto [begin_idx, end_idx] = range_indices(sorted, lower, upper);
    REQUIRE(begin_idx == 1);
    REQUIRE(end_idx == 2);
}

TEST_CASE("range_indices: empty range", "[reconciliation]") {
    auto h1 = make_hash(0x10);
    auto h2 = make_hash(0x20);
    std::vector<Hash32> sorted = {h1, h2};

    // Range [0x30..., 0x40...) contains nothing
    Hash32 lower;
    lower.fill(0x30);
    Hash32 upper;
    upper.fill(0x40);
    auto [begin_idx, end_idx] = range_indices(sorted, lower, upper);
    REQUIRE(begin_idx == end_idx);
}

TEST_CASE("range_indices: boundary items", "[reconciliation]") {
    auto h1 = make_hash(0x10);
    auto h2 = make_hash(0x20);
    auto h3 = make_hash(0x30);
    std::vector<Hash32> sorted = {h1, h2, h3};

    // Lower bound exactly matches h2, upper bound exactly matches h3
    // h2 should be included (lower inclusive), h3 excluded (upper exclusive)
    auto [begin_idx, end_idx] = range_indices(sorted, h2, h3);
    REQUIRE(begin_idx == 1);
    REQUIRE(end_idx == 2);
}

// ============================================================================
// count_in_range
// ============================================================================

TEST_CASE("count_in_range matches range_indices", "[reconciliation]") {
    auto h1 = make_hash(0x10);
    auto h2 = make_hash(0x20);
    auto h3 = make_hash(0x30);
    std::vector<Hash32> sorted = {h1, h2, h3};

    Hash32 lower{};
    Hash32 upper;
    upper.fill(0xFF);
    auto count = count_in_range(sorted, lower, upper);
    auto [b, e] = range_indices(sorted, lower, upper);
    REQUIRE(count == (e - b));
    REQUIRE(count == 3);
}

// ============================================================================
// Encode/decode round-trip: ReconcileInit
// ============================================================================

TEST_CASE("encode/decode reconcile_init round-trip", "[reconciliation]") {
    ReconcileInit init;
    init.version = RECONCILE_VERSION;
    init.namespace_id = make_hash(0xAA);
    init.count = 42;
    init.fingerprint = make_hash(0xBB);

    auto encoded = encode_reconcile_init(init);

    // First byte must be version 0x01 (SYNC-09)
    REQUIRE(encoded[0] == 0x01);

    auto decoded = decode_reconcile_init(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->version == RECONCILE_VERSION);
    REQUIRE(decoded->namespace_id == init.namespace_id);
    REQUIRE(decoded->count == 42);
    REQUIRE(decoded->fingerprint == init.fingerprint);
}

TEST_CASE("decode_reconcile_init rejects unknown version", "[reconciliation]") {
    ReconcileInit init;
    init.version = RECONCILE_VERSION;
    init.namespace_id = make_hash(0xAA);
    init.count = 1;
    init.fingerprint = make_hash(0xBB);

    auto encoded = encode_reconcile_init(init);
    // Corrupt version byte
    encoded[0] = 0x02;

    auto decoded = decode_reconcile_init(encoded);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("decode_reconcile_init rejects truncated data", "[reconciliation]") {
    // Less than minimum size (1 + 32 + 4 + 32 = 69 bytes)
    std::vector<uint8_t> truncated(10, 0x01);
    auto decoded = decode_reconcile_init(truncated);
    REQUIRE_FALSE(decoded.has_value());
}

// ============================================================================
// Encode/decode round-trip: ReconcileRanges
// ============================================================================

TEST_CASE("encode/decode reconcile_ranges round-trip", "[reconciliation]") {
    Hash32 ns = make_hash(0xCC);
    std::vector<RangeEntry> ranges;

    // Skip mode
    RangeEntry skip;
    skip.upper_bound = make_hash(0x40);
    skip.mode = RangeMode::Skip;
    ranges.push_back(skip);

    // Fingerprint mode
    RangeEntry fp_entry;
    fp_entry.upper_bound = make_hash(0x80);
    fp_entry.mode = RangeMode::Fingerprint;
    fp_entry.count = 10;
    fp_entry.fingerprint = make_hash(0xDD);
    ranges.push_back(fp_entry);

    // ItemList mode
    RangeEntry items_entry;
    items_entry.upper_bound = make_hash(0xFF);
    items_entry.mode = RangeMode::ItemList;
    items_entry.count = 2;
    items_entry.items = {make_hash(0xA1), make_hash(0xA2)};
    ranges.push_back(items_entry);

    auto encoded = encode_reconcile_ranges(ns, ranges);
    auto decoded = decode_reconcile_ranges(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->namespace_id == ns);
    REQUIRE(decoded->ranges.size() == 3);

    // Verify skip
    REQUIRE(decoded->ranges[0].upper_bound == make_hash(0x40));
    REQUIRE(decoded->ranges[0].mode == RangeMode::Skip);

    // Verify fingerprint
    REQUIRE(decoded->ranges[1].upper_bound == make_hash(0x80));
    REQUIRE(decoded->ranges[1].mode == RangeMode::Fingerprint);
    REQUIRE(decoded->ranges[1].count == 10);
    REQUIRE(decoded->ranges[1].fingerprint == make_hash(0xDD));

    // Verify item list
    REQUIRE(decoded->ranges[2].upper_bound == make_hash(0xFF));
    REQUIRE(decoded->ranges[2].mode == RangeMode::ItemList);
    REQUIRE(decoded->ranges[2].count == 2);
    REQUIRE(decoded->ranges[2].items.size() == 2);
    REQUIRE(decoded->ranges[2].items[0] == make_hash(0xA1));
    REQUIRE(decoded->ranges[2].items[1] == make_hash(0xA2));
}

TEST_CASE("decode_reconcile_ranges rejects truncated data", "[reconciliation]") {
    std::vector<uint8_t> truncated(10, 0);
    auto decoded = decode_reconcile_ranges(truncated);
    REQUIRE_FALSE(decoded.has_value());
}

// ============================================================================
// Encode/decode round-trip: ReconcileItems
// ============================================================================

TEST_CASE("encode/decode reconcile_items round-trip", "[reconciliation]") {
    Hash32 ns = make_hash(0xEE);
    std::vector<Hash32> items = {make_hash(0x11), make_hash(0x22), make_hash(0x33)};

    auto encoded = encode_reconcile_items(ns, items);
    auto decoded = decode_reconcile_items(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->namespace_id == ns);
    REQUIRE(decoded->items.size() == 3);
    REQUIRE(decoded->items[0] == make_hash(0x11));
    REQUIRE(decoded->items[1] == make_hash(0x22));
    REQUIRE(decoded->items[2] == make_hash(0x33));
}

TEST_CASE("decode_reconcile_items rejects truncated data", "[reconciliation]") {
    std::vector<uint8_t> truncated(10, 0);
    auto decoded = decode_reconcile_items(truncated);
    REQUIRE_FALSE(decoded.has_value());
}

// ============================================================================
// reconcile_local: full simulation
// ============================================================================

TEST_CASE("reconcile_local: identical sets produce empty diffs", "[reconciliation]") {
    auto hashes = make_random_hashes(100);
    auto [diff_a, diff_b] = reconcile_local(hashes, hashes);
    REQUIRE(diff_a.empty());
    REQUIRE(diff_b.empty());
}

TEST_CASE("reconcile_local: completely disjoint sets", "[reconciliation]") {
    auto set_a = make_random_hashes(50, 1);
    auto set_b = make_random_hashes(50, 2);

    // Ensure disjoint (extremely likely with random 32-byte hashes, but verify)
    for (const auto& h : set_a) {
        REQUIRE(!std::binary_search(set_b.begin(), set_b.end(), h));
    }

    auto [diff_a, diff_b] = reconcile_local(set_a, set_b);
    // diff_a = items in B not in A (everything in B)
    // diff_b = items in A not in B (everything in A)
    std::sort(diff_a.begin(), diff_a.end());
    std::sort(diff_b.begin(), diff_b.end());
    REQUIRE(diff_a.size() == set_b.size());
    REQUIRE(diff_b.size() == set_a.size());
    REQUIRE(diff_a == set_b);
    REQUIRE(diff_b == set_a);
}

TEST_CASE("reconcile_local: one side has extra hash", "[reconciliation]") {
    auto base = make_random_hashes(50, 3);
    auto extended = base;
    // Add one extra unique hash
    Hash32 extra;
    extra.fill(0xFE);
    extended.push_back(extra);
    std::sort(extended.begin(), extended.end());

    auto [diff_a, diff_b] = reconcile_local(base, extended);
    // diff_a = items in extended not in base (the extra hash)
    // diff_b = items in base not in extended (empty)
    REQUIRE(diff_a.size() == 1);
    REQUIRE(diff_a[0] == extra);
    REQUIRE(diff_b.empty());
}

TEST_CASE("reconcile_local: one side empty", "[reconciliation]") {
    auto set_a = make_random_hashes(30, 4);
    std::vector<Hash32> empty_set;

    auto [diff_a, diff_b] = reconcile_local(empty_set, set_a);
    // diff_a = items in set_a not in empty = all of set_a
    // diff_b = items in empty not in set_a = empty
    std::sort(diff_a.begin(), diff_a.end());
    REQUIRE(diff_a.size() == set_a.size());
    REQUIRE(diff_a == set_a);
    REQUIRE(diff_b.empty());
}

TEST_CASE("reconcile_local: large sets with small diff", "[reconciliation]") {
    auto base = make_random_hashes(1000, 5);

    // Create set_a = base minus 5 items + 5 unique items
    auto set_a = base;
    // Remove items at indices 100, 300, 500, 700, 900
    std::vector<Hash32> removed_from_a;
    for (int idx : {900, 700, 500, 300, 100}) {
        removed_from_a.push_back(set_a[idx]);
        set_a.erase(set_a.begin() + idx);
    }
    // Add 5 unique items
    std::vector<Hash32> added_to_a;
    for (uint8_t i = 0; i < 5; ++i) {
        Hash32 h;
        h.fill(0);
        h[0] = 0xFE;
        h[1] = i;
        added_to_a.push_back(h);
    }
    set_a.insert(set_a.end(), added_to_a.begin(), added_to_a.end());
    std::sort(set_a.begin(), set_a.end());

    auto [diff_a, diff_b] = reconcile_local(set_a, base);
    // diff_a = items in base not in set_a = removed_from_a
    // diff_b = items in set_a not in base = added_to_a
    std::sort(diff_a.begin(), diff_a.end());
    std::sort(diff_b.begin(), diff_b.end());
    std::sort(removed_from_a.begin(), removed_from_a.end());
    std::sort(added_to_a.begin(), added_to_a.end());

    REQUIRE(diff_a.size() == 5);
    REQUIRE(diff_b.size() == 5);
    REQUIRE(diff_a == removed_from_a);
    REQUIRE(diff_b == added_to_a);
}

TEST_CASE("reconcile_local: single item difference in large set", "[reconciliation]") {
    auto base = make_random_hashes(500, 6);
    auto modified = base;
    // Replace one item
    Hash32 replacement;
    replacement.fill(0xFD);
    Hash32 removed = modified[250];
    modified[250] = replacement;
    std::sort(modified.begin(), modified.end());

    auto [diff_a, diff_b] = reconcile_local(base, modified);
    // diff_a = items in modified not in base = replacement
    // diff_b = items in base not in modified = removed
    REQUIRE(diff_a.size() == 1);
    REQUIRE(diff_b.size() == 1);
    REQUIRE(diff_a[0] == replacement);
    REQUIRE(diff_b[0] == removed);
}

// ============================================================================
// process_ranges: fingerprint+count match -> skip
// ============================================================================

TEST_CASE("process_ranges: fingerprint+count match produces skip", "[reconciliation]") {
    auto hashes = make_random_hashes(20, 7);
    auto fp = xor_fingerprint(hashes, 0, hashes.size());

    // Simulate a received range that matches our data
    RangeEntry range;
    range.upper_bound.fill(0xFF);
    range.mode = RangeMode::Fingerprint;
    range.count = static_cast<uint32_t>(hashes.size());
    range.fingerprint = fp;

    auto result = process_ranges(hashes, {range});
    // All ranges should be resolved (skip)
    REQUIRE(result.complete);
    // Response should have a single Skip range
    REQUIRE(result.response_ranges.size() == 1);
    REQUIRE(result.response_ranges[0].mode == RangeMode::Skip);
}

TEST_CASE("process_ranges: fingerprint matches but count differs - no skip", "[reconciliation]") {
    // Create two sets with same XOR fingerprint but different counts.
    // XOR of {A, B} == XOR of {A, B, C, C} (C cancels itself).
    // But the counts are different (2 vs 4), so they should NOT match.
    Hash32 h1 = make_hash(0x0F);
    Hash32 h2 = make_hash(0xF0);
    std::vector<Hash32> our_set = {h1, h2};
    std::sort(our_set.begin(), our_set.end());

    // Received: same fingerprint but count=4
    auto fp = xor_fingerprint(our_set, 0, our_set.size());
    RangeEntry range;
    range.upper_bound.fill(0xFF);
    range.mode = RangeMode::Fingerprint;
    range.count = 4;  // Different from our count of 2
    range.fingerprint = fp;

    auto result = process_ranges(our_set, {range});
    // Should NOT be complete (count mismatch prevents skip)
    REQUIRE_FALSE(result.complete);
}

TEST_CASE("process_ranges: empty set vs fingerprint range", "[reconciliation]") {
    // We have nothing; they have items.
    std::vector<Hash32> our_set;
    Hash32 their_fp = make_hash(0xAB);

    RangeEntry range;
    range.upper_bound.fill(0xFF);
    range.mode = RangeMode::Fingerprint;
    range.count = 5;
    range.fingerprint = their_fp;

    auto result = process_ranges(our_set, {range});
    // We have 0 items in range, which is <= SPLIT_THRESHOLD, so we respond with ItemList (empty)
    REQUIRE(result.response_ranges.size() == 1);
    REQUIRE(result.response_ranges[0].mode == RangeMode::ItemList);
    REQUIRE(result.response_ranges[0].items.empty());
}

// ============================================================================
// Integration: network-style reconciliation simulation
// ============================================================================

namespace {

/// Simulate network-style reconciliation between two sorted hash sets.
/// Unlike reconcile_local (which is a clean simulation), this mirrors the
/// actual network protocol used by peer_manager.cpp:
///   1. "Initiator" sends full-range fingerprint
///   2. Sides exchange ReconcileRanges back and forth
///   3. When one side receives all-resolved ranges (no Fingerprint), it sends
///      ReconcileItems as the final exchange
///
/// Returns (initiator_missing, responder_missing).
std::pair<std::vector<Hash32>, std::vector<Hash32>> reconcile_network_style(
    const std::vector<Hash32>& init_sorted,
    const std::vector<Hash32>& resp_sorted)
{
    Hash32 max_hash;
    max_hash.fill(0xFF);

    // Step 1: Initiator sends ReconcileInit
    auto init_fp = xor_fingerprint(init_sorted, 0, init_sorted.size());
    auto init_count = static_cast<uint32_t>(init_sorted.size());

    // Step 2: Responder processes init as a single full-range Fingerprint
    RangeEntry init_range;
    init_range.upper_bound = max_hash;
    init_range.mode = RangeMode::Fingerprint;
    init_range.count = init_count;
    init_range.fingerprint = init_fp;

    auto resp_result = process_ranges(resp_sorted, {init_range});
    std::vector<Hash32> init_peer_items;  // Items initiator learns from responder
    std::vector<Hash32> resp_peer_items;  // Items responder learns from initiator

    if (resp_result.complete) {
        // Fingerprints matched -- no diff
        return {{}, {}};
    }

    // Responder sends its ranges; initiator processes; back and forth
    std::vector<RangeEntry> current_ranges = std::move(resp_result.response_ranges);
    bool initiator_turn = true;  // Initiator processes next

    for (uint32_t round = 0; round < MAX_RECONCILE_ROUNDS; ++round) {
        const auto& processor = initiator_turn ? init_sorted : resp_sorted;

        // Check if all ranges are resolved (no Fingerprint)
        bool has_fingerprint = false;
        for (const auto& r : current_ranges) {
            if (r.mode == RangeMode::Fingerprint) {
                has_fingerprint = true;
                break;
            }
        }

        if (!has_fingerprint) {
            // Final exchange: extract items and send our items back
            auto& peer_items = initiator_turn ? init_peer_items : resp_peer_items;
            auto& our_items_out = initiator_turn ? resp_peer_items : init_peer_items;

            for (const auto& r : current_ranges) {
                if (r.mode == RangeMode::ItemList) {
                    peer_items.insert(peer_items.end(), r.items.begin(), r.items.end());
                }
            }

            // Send our items for the ItemList ranges
            Hash32 lower{};
            for (const auto& r : current_ranges) {
                if (r.mode == RangeMode::ItemList) {
                    auto [b, e] = range_indices(processor, lower, r.upper_bound);
                    for (size_t idx = b; idx < e; ++idx) {
                        our_items_out.push_back(processor[idx]);
                    }
                }
                lower = r.upper_bound;
            }
            break;
        }

        auto result = process_ranges(processor, current_ranges);

        auto& peer_items = initiator_turn ? init_peer_items : resp_peer_items;
        peer_items.insert(peer_items.end(), result.have_items.begin(), result.have_items.end());

        current_ranges = std::move(result.response_ranges);
        initiator_turn = !initiator_turn;
    }

    // Compute diffs
    std::set<Hash32> init_set(init_sorted.begin(), init_sorted.end());
    std::set<Hash32> resp_set(resp_sorted.begin(), resp_sorted.end());

    std::vector<Hash32> init_missing;
    for (const auto& h : init_peer_items) {
        if (init_set.find(h) == init_set.end()) init_missing.push_back(h);
    }
    std::vector<Hash32> resp_missing;
    for (const auto& h : resp_peer_items) {
        if (resp_set.find(h) == resp_set.end()) resp_missing.push_back(h);
    }
    return {init_missing, resp_missing};
}

}  // anonymous namespace

TEST_CASE("network-style reconciliation: identical sets", "[reconciliation][integration]") {
    auto hashes = make_random_hashes(100);
    auto [init_miss, resp_miss] = reconcile_network_style(hashes, hashes);
    REQUIRE(init_miss.empty());
    REQUIRE(resp_miss.empty());
}

TEST_CASE("network-style reconciliation: one side empty", "[reconciliation][integration]") {
    auto set_a = make_random_hashes(30, 10);
    std::vector<Hash32> empty_set;

    auto [init_miss, resp_miss] = reconcile_network_style(empty_set, set_a);
    std::sort(init_miss.begin(), init_miss.end());
    REQUIRE(init_miss.size() == set_a.size());
    REQUIRE(init_miss == set_a);
    REQUIRE(resp_miss.empty());
}

TEST_CASE("network-style reconciliation: disjoint sets", "[reconciliation][integration]") {
    auto set_a = make_random_hashes(20, 20);
    auto set_b = make_random_hashes(20, 21);

    auto [init_miss, resp_miss] = reconcile_network_style(set_a, set_b);
    std::sort(init_miss.begin(), init_miss.end());
    std::sort(resp_miss.begin(), resp_miss.end());
    REQUIRE(init_miss.size() == set_b.size());
    REQUIRE(init_miss == set_b);
    REQUIRE(resp_miss.size() == set_a.size());
    REQUIRE(resp_miss == set_a);
}

TEST_CASE("network-style reconciliation: small diff in large sets", "[reconciliation][integration]") {
    auto base = make_random_hashes(500, 30);
    auto set_a = base;
    auto set_b = base;

    // Remove 3 items from set_a, add 2 unique items
    std::vector<Hash32> removed_a = {set_a[100], set_a[250], set_a[400]};
    set_a.erase(set_a.begin() + 400);
    set_a.erase(set_a.begin() + 250);
    set_a.erase(set_a.begin() + 100);

    Hash32 extra_a1, extra_a2;
    extra_a1.fill(0);
    extra_a1[0] = 0xFE;
    extra_a1[1] = 0x01;
    extra_a2.fill(0);
    extra_a2[0] = 0xFE;
    extra_a2[1] = 0x02;
    set_a.push_back(extra_a1);
    set_a.push_back(extra_a2);
    std::sort(set_a.begin(), set_a.end());

    auto [init_miss, resp_miss] = reconcile_network_style(set_a, set_b);
    std::sort(init_miss.begin(), init_miss.end());
    std::sort(resp_miss.begin(), resp_miss.end());
    std::sort(removed_a.begin(), removed_a.end());
    std::vector<Hash32> expected_resp_miss = {extra_a1, extra_a2};
    std::sort(expected_resp_miss.begin(), expected_resp_miss.end());

    REQUIRE(init_miss.size() == 3);
    REQUIRE(init_miss == removed_a);
    REQUIRE(resp_miss.size() == 2);
    REQUIRE(resp_miss == expected_resp_miss);
}

TEST_CASE("network-style reconciliation: single item each side", "[reconciliation][integration]") {
    Hash32 h_a;
    h_a.fill(0x11);
    Hash32 h_b;
    h_b.fill(0x22);
    std::vector<Hash32> set_a = {h_a};
    std::vector<Hash32> set_b = {h_b};

    auto [init_miss, resp_miss] = reconcile_network_style(set_a, set_b);
    REQUIRE(init_miss.size() == 1);
    REQUIRE(init_miss[0] == h_b);
    REQUIRE(resp_miss.size() == 1);
    REQUIRE(resp_miss[0] == h_a);
}
