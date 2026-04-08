#include "db/sync/reconciliation.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "db/util/endian.h"

namespace chromatindb::sync {

namespace {

/// Sentinel for minimum hash value (all zeros).
const Hash32 MIN_HASH{};

/// Sentinel for maximum hash value (all 0xFF).
Hash32 make_max_hash() {
    Hash32 h;
    h.fill(0xFF);
    return h;
}

} // anonymous namespace

// =============================================================================
// Pure algorithm functions
// =============================================================================

Hash32 xor_fingerprint(const std::vector<Hash32>& sorted, size_t begin, size_t end) {
    Hash32 fp{};
    for (size_t i = begin; i < end; ++i) {
        for (size_t b = 0; b < 32; ++b) {
            fp[b] ^= sorted[i][b];
        }
    }
    return fp;
}

std::pair<size_t, size_t> range_indices(const std::vector<Hash32>& sorted,
                                        const Hash32& lower_bound,
                                        const Hash32& upper_bound) {
    // lower_bound inclusive: first element >= lower_bound
    auto begin_it = std::lower_bound(sorted.begin(), sorted.end(), lower_bound);
    // upper_bound exclusive: first element >= upper_bound
    auto end_it = std::lower_bound(sorted.begin(), sorted.end(), upper_bound);
    return {static_cast<size_t>(begin_it - sorted.begin()),
            static_cast<size_t>(end_it - sorted.begin())};
}

size_t count_in_range(const std::vector<Hash32>& sorted,
                      const Hash32& lower_bound,
                      const Hash32& upper_bound) {
    auto [b, e] = range_indices(sorted, lower_bound, upper_bound);
    return e - b;
}

ReconcileResult process_ranges(
    const std::vector<Hash32>& our_sorted,
    const std::vector<RangeEntry>& received_ranges) {
    ReconcileResult result;
    result.complete = true;  // Assume complete until we find an unresolved range

    Hash32 lower_bound = MIN_HASH;

    for (const auto& range : received_ranges) {
        const Hash32& upper_bound = range.upper_bound;

        if (range.mode == RangeMode::Skip) {
            // Peer already confirmed this range matches; echo skip
            RangeEntry skip;
            skip.upper_bound = upper_bound;
            skip.mode = RangeMode::Skip;
            result.response_ranges.push_back(skip);
            lower_bound = upper_bound;
            continue;
        }

        if (range.mode == RangeMode::ItemList) {
            // Peer sent items directly; collect them
            result.have_items.insert(result.have_items.end(),
                                     range.items.begin(), range.items.end());

            // Send our items in this range back
            auto [b, e] = range_indices(our_sorted, lower_bound, upper_bound);
            RangeEntry response;
            response.upper_bound = upper_bound;
            response.mode = RangeMode::ItemList;
            response.count = static_cast<uint32_t>(e - b);
            response.items.assign(our_sorted.begin() + static_cast<ptrdiff_t>(b),
                                  our_sorted.begin() + static_cast<ptrdiff_t>(e));
            result.response_ranges.push_back(std::move(response));
            lower_bound = upper_bound;
            continue;
        }

        // mode == Fingerprint: compare our fingerprint+count with theirs
        auto [b, e] = range_indices(our_sorted, lower_bound, upper_bound);
        auto our_count = static_cast<uint32_t>(e - b);
        auto our_fp = xor_fingerprint(our_sorted, b, e);

        if (our_fp == range.fingerprint && our_count == range.count) {
            // Range matches -- skip
            RangeEntry skip;
            skip.upper_bound = upper_bound;
            skip.mode = RangeMode::Skip;
            result.response_ranges.push_back(skip);
        } else if (our_count <= SPLIT_THRESHOLD) {
            // Small enough to send items directly
            RangeEntry item_response;
            item_response.upper_bound = upper_bound;
            item_response.mode = RangeMode::ItemList;
            item_response.count = our_count;
            item_response.items.assign(our_sorted.begin() + static_cast<ptrdiff_t>(b),
                                       our_sorted.begin() + static_cast<ptrdiff_t>(e));
            result.response_ranges.push_back(std::move(item_response));
            result.complete = false;
        } else {
            // Split at midpoint and send two sub-range fingerprints
            size_t mid = b + (e - b) / 2;
            // Use the hash at mid as the split point (boundary)
            const Hash32& split_hash = our_sorted[mid];

            auto lower_fp = xor_fingerprint(our_sorted, b, mid);
            auto upper_fp = xor_fingerprint(our_sorted, mid, e);

            RangeEntry lower_range;
            lower_range.upper_bound = split_hash;
            lower_range.mode = RangeMode::Fingerprint;
            lower_range.count = static_cast<uint32_t>(mid - b);
            lower_range.fingerprint = lower_fp;
            result.response_ranges.push_back(lower_range);

            RangeEntry upper_range;
            upper_range.upper_bound = upper_bound;
            upper_range.mode = RangeMode::Fingerprint;
            upper_range.count = static_cast<uint32_t>(e - mid);
            upper_range.fingerprint = upper_fp;
            result.response_ranges.push_back(upper_range);

            result.complete = false;
        }

        lower_bound = upper_bound;
    }

    return result;
}

std::pair<std::vector<Hash32>, std::vector<Hash32>> reconcile_local(
    const std::vector<Hash32>& sorted_a,
    const std::vector<Hash32>& sorted_b) {
    // Collect all items exchanged by both sides during the protocol
    std::set<Hash32> items_from_a;  // Items A revealed via ItemList
    std::set<Hash32> items_from_b;  // Items B revealed via ItemList

    // Start: A sends full-range fingerprint to B
    Hash32 max_hash = make_max_hash();

    RangeEntry init_range;
    init_range.upper_bound = max_hash;
    init_range.mode = RangeMode::Fingerprint;
    init_range.count = static_cast<uint32_t>(sorted_a.size());
    init_range.fingerprint = xor_fingerprint(sorted_a, 0, sorted_a.size());

    std::vector<RangeEntry> current_ranges = {init_range};
    bool a_turn = false;  // B processes first (responds to A's init)

    for (uint32_t round = 0; round < MAX_RECONCILE_ROUNDS; ++round) {
        const auto& processor_sorted = a_turn ? sorted_a : sorted_b;
        auto result = process_ranges(processor_sorted, current_ranges);

        // Collect items from the received ranges
        for (const auto& range : current_ranges) {
            if (range.mode == RangeMode::ItemList) {
                if (a_turn) {
                    // A received these from B
                    items_from_b.insert(range.items.begin(), range.items.end());
                } else {
                    // B received these from A
                    items_from_a.insert(range.items.begin(), range.items.end());
                }
            }
        }

        // Collect items from the response ItemLists
        for (const auto& range : result.response_ranges) {
            if (range.mode == RangeMode::ItemList) {
                if (a_turn) {
                    items_from_a.insert(range.items.begin(), range.items.end());
                } else {
                    items_from_b.insert(range.items.begin(), range.items.end());
                }
            }
        }

        // Also collect have_items (items from received ItemLists stored in result)
        if (a_turn) {
            items_from_b.insert(result.have_items.begin(), result.have_items.end());
        } else {
            items_from_a.insert(result.have_items.begin(), result.have_items.end());
        }

        if (result.complete) break;

        current_ranges = std::move(result.response_ranges);
        a_turn = !a_turn;
    }

    // Compute bidirectional diff from collected items
    std::set<Hash32> set_a(sorted_a.begin(), sorted_a.end());
    std::set<Hash32> set_b(sorted_b.begin(), sorted_b.end());

    // items_in_b_not_in_a: items B has that A doesn't
    std::vector<Hash32> b_not_in_a;
    for (const auto& h : items_from_b) {
        if (set_a.find(h) == set_a.end()) {
            b_not_in_a.push_back(h);
        }
    }

    // items_in_a_not_in_b: items A has that B doesn't
    std::vector<Hash32> a_not_in_b;
    for (const auto& h : items_from_a) {
        if (set_b.find(h) == set_b.end()) {
            a_not_in_b.push_back(h);
        }
    }

    return {b_not_in_a, a_not_in_b};
}

// =============================================================================
// Encode/decode: ReconcileInit
// =============================================================================

std::vector<uint8_t> encode_reconcile_init(const ReconcileInit& init) {
    std::vector<uint8_t> buf;
    buf.reserve(1 + 32 + 4 + 32);  // version + ns + count + fp
    buf.push_back(init.version);
    buf.insert(buf.end(), init.namespace_id.begin(), init.namespace_id.end());
    chromatindb::util::write_u32_be(buf, init.count);
    buf.insert(buf.end(), init.fingerprint.begin(), init.fingerprint.end());
    return buf;
}

std::optional<ReconcileInit> decode_reconcile_init(std::span<const uint8_t> payload) {
    constexpr size_t MIN_SIZE = 1 + 32 + 4 + 32;  // 69 bytes
    if (payload.size() < MIN_SIZE) return std::nullopt;

    ReconcileInit init;
    init.version = payload[0];
    if (init.version != RECONCILE_VERSION) return std::nullopt;

    std::memcpy(init.namespace_id.data(), payload.data() + 1, 32);
    init.count = chromatindb::util::read_u32_be(payload.data() + 33);
    std::memcpy(init.fingerprint.data(), payload.data() + 37, 32);

    return init;
}

// =============================================================================
// Encode/decode: ReconcileRanges
// =============================================================================

std::vector<uint8_t> encode_reconcile_ranges(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<RangeEntry>& ranges) {
    std::vector<uint8_t> buf;
    // Estimate size: ns(32) + count(4) + per range(32 + 1 + variable)
    auto reserve_size = chromatindb::util::checked_mul(ranges.size(), size_t{69});
    if (reserve_size) {
        auto total = chromatindb::util::checked_add(size_t{36}, *reserve_size);
        if (total) buf.reserve(*total);
    }

    buf.insert(buf.end(), namespace_id.begin(), namespace_id.end());
    chromatindb::util::write_u32_be(buf, static_cast<uint32_t>(ranges.size()));

    for (const auto& range : ranges) {
        // upper_bound: 32 bytes
        buf.insert(buf.end(), range.upper_bound.begin(), range.upper_bound.end());
        // mode: 1 byte
        buf.push_back(static_cast<uint8_t>(range.mode));

        switch (range.mode) {
            case RangeMode::Skip:
                // No additional data
                break;
            case RangeMode::Fingerprint:
                // count: u32BE + fingerprint: 32 bytes
                chromatindb::util::write_u32_be(buf, range.count);
                buf.insert(buf.end(), range.fingerprint.begin(), range.fingerprint.end());
                break;
            case RangeMode::ItemList:
                // count: u32BE + items: count * 32 bytes
                chromatindb::util::write_u32_be(buf, range.count);
                for (const auto& item : range.items) {
                    buf.insert(buf.end(), item.begin(), item.end());
                }
                break;
        }
    }

    return buf;
}

std::optional<DecodedRanges> decode_reconcile_ranges(std::span<const uint8_t> payload) {
    constexpr size_t HEADER_SIZE = 32 + 4;  // ns + count
    if (payload.size() < HEADER_SIZE) return std::nullopt;

    DecodedRanges result;
    std::memcpy(result.namespace_id.data(), payload.data(), 32);
    uint32_t range_count = chromatindb::util::read_u32_be(payload.data() + 32);

    size_t offset = HEADER_SIZE;
    result.ranges.reserve(range_count);

    for (uint32_t i = 0; i < range_count; ++i) {
        // Need at least 33 bytes (32 upper_bound + 1 mode)
        if (offset + 33 > payload.size()) return std::nullopt;

        RangeEntry entry;
        std::memcpy(entry.upper_bound.data(), payload.data() + offset, 32);
        offset += 32;
        entry.mode = static_cast<RangeMode>(payload[offset]);
        offset += 1;

        switch (entry.mode) {
            case RangeMode::Skip:
                break;
            case RangeMode::Fingerprint:
                if (offset + 4 + 32 > payload.size()) return std::nullopt;
                entry.count = chromatindb::util::read_u32_be(payload.data() + offset);
                offset += 4;
                std::memcpy(entry.fingerprint.data(), payload.data() + offset, 32);
                offset += 32;
                break;
            case RangeMode::ItemList: {
                if (offset + 4 > payload.size()) return std::nullopt;
                entry.count = chromatindb::util::read_u32_be(payload.data() + offset);
                offset += 4;
                auto items_size = chromatindb::util::checked_mul(static_cast<size_t>(entry.count), size_t{32});
                if (!items_size) return std::nullopt;
                auto items_end = chromatindb::util::checked_add(offset, *items_size);
                if (!items_end || *items_end > payload.size()) return std::nullopt;
                entry.items.reserve(entry.count);
                for (uint32_t j = 0; j < entry.count; ++j) {
                    Hash32 item;
                    std::memcpy(item.data(), payload.data() + offset, 32);
                    offset += 32;
                    entry.items.push_back(item);
                }
                break;
            }
            default:
                return std::nullopt;  // Unknown mode
        }

        result.ranges.push_back(std::move(entry));
    }

    return result;
}

// =============================================================================
// Encode/decode: ReconcileItems
// =============================================================================

std::vector<uint8_t> encode_reconcile_items(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<Hash32>& items) {
    std::vector<uint8_t> buf;
    auto reserve_size = chromatindb::util::checked_mul(items.size(), size_t{32});
    if (reserve_size) {
        auto total = chromatindb::util::checked_add(size_t{36}, *reserve_size);
        if (total) buf.reserve(*total);
    }

    buf.insert(buf.end(), namespace_id.begin(), namespace_id.end());
    chromatindb::util::write_u32_be(buf, static_cast<uint32_t>(items.size()));
    for (const auto& item : items) {
        buf.insert(buf.end(), item.begin(), item.end());
    }

    return buf;
}

std::optional<DecodedItems> decode_reconcile_items(std::span<const uint8_t> payload) {
    constexpr size_t HEADER_SIZE = 32 + 4;  // ns + count
    if (payload.size() < HEADER_SIZE) return std::nullopt;

    DecodedItems result;
    std::memcpy(result.namespace_id.data(), payload.data(), 32);
    uint32_t count = chromatindb::util::read_u32_be(payload.data() + 32);

    auto items_size = chromatindb::util::checked_mul(static_cast<size_t>(count), size_t{32});
    if (!items_size) return std::nullopt;
    auto total = chromatindb::util::checked_add(HEADER_SIZE, *items_size);
    if (!total || payload.size() < *total) return std::nullopt;

    result.items.reserve(count);
    size_t offset = HEADER_SIZE;
    for (uint32_t i = 0; i < count; ++i) {
        Hash32 item;
        std::memcpy(item.data(), payload.data() + offset, 32);
        offset += 32;
        result.items.push_back(item);
    }

    return result;
}

} // namespace chromatindb::sync
