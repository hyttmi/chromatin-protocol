#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::sync {

/// Protocol version for ReconcileInit messages (SYNC-09).
constexpr uint8_t RECONCILE_VERSION = 0x01;

/// Below this item count, send items directly instead of recursing.
constexpr uint32_t SPLIT_THRESHOLD = 16;

/// Safety limit on reconciliation rounds to prevent runaway recursion.
constexpr uint32_t MAX_RECONCILE_ROUNDS = 64;

/// 32-byte hash type used throughout reconciliation.
using Hash32 = std::array<uint8_t, 32>;

/// Range entry mode for ReconcileRanges wire format.
enum class RangeMode : uint8_t {
    Skip = 0,        // Range is identical, no action needed
    Fingerprint = 1, // Range fingerprint for comparison
    ItemList = 2     // Direct item list (leaf range)
};

/// A range in the reconciliation protocol.
/// Lower bound is implicit (previous range's upper_bound, or MIN for first).
struct RangeEntry {
    Hash32 upper_bound;    // Exclusive upper bound hash
    RangeMode mode;
    uint32_t count = 0;    // Item count in range (for Fingerprint/ItemList modes)
    Hash32 fingerprint{};  // Only valid when mode == Fingerprint
    std::vector<Hash32> items; // Only valid when mode == ItemList
};

/// Decoded ReconcileInit message.
struct ReconcileInit {
    uint8_t version;
    Hash32 namespace_id;
    uint32_t count;        // Sender's total item count
    Hash32 fingerprint;    // XOR of all sender's items
};

/// Result of processing received ranges against local data.
struct ReconcileResult {
    std::vector<RangeEntry> response_ranges;  // Ranges to send back
    std::vector<Hash32> have_items;           // Items from received ItemLists
    bool complete = false;                     // True if all ranges resolved
};

/// Decoded ReconcileRanges message.
struct DecodedRanges {
    Hash32 namespace_id;
    std::vector<RangeEntry> ranges;
};

/// Decoded ReconcileItems message.
struct DecodedItems {
    Hash32 namespace_id;
    std::vector<Hash32> items;
};

// =========================================================================
// Pure algorithm functions
// =========================================================================

/// XOR fingerprint over a range [begin, end) in a sorted hash vector.
/// Returns all-zero hash for empty ranges (identity element).
Hash32 xor_fingerprint(const std::vector<Hash32>& sorted, size_t begin, size_t end);

/// Get index range [begin, end) for items in [lower_bound, upper_bound)
/// using lexicographic comparison on sorted vector.
/// Lower bound inclusive, upper bound exclusive.
std::pair<size_t, size_t> range_indices(const std::vector<Hash32>& sorted,
                                        const Hash32& lower_bound,
                                        const Hash32& upper_bound);

/// Count items in sorted vector that fall within [lower_bound, upper_bound).
size_t count_in_range(const std::vector<Hash32>& sorted,
                      const Hash32& lower_bound,
                      const Hash32& upper_bound);

/// Process received ranges against our local sorted hashes.
/// For each received range:
///   - If our fingerprint+count matches: Skip (identical)
///   - If our count <= SPLIT_THRESHOLD: respond with ItemList
///   - Else: split at midpoint, respond with two Fingerprint sub-ranges
ReconcileResult process_ranges(
    const std::vector<Hash32>& our_sorted,
    const std::vector<RangeEntry>& received_ranges);

/// Full local reconciliation simulation (for testing).
/// Runs the multi-round protocol locally between two sorted hash vectors.
/// Returns (items_in_b_not_in_a, items_in_a_not_in_b).
std::pair<std::vector<Hash32>, std::vector<Hash32>> reconcile_local(
    const std::vector<Hash32>& sorted_a,
    const std::vector<Hash32>& sorted_b);

// =========================================================================
// Encode/decode functions
// =========================================================================

/// Encode a ReconcileInit message.
/// Wire: [version:1][namespace_id:32][count:u32BE][fingerprint:32]
std::vector<uint8_t> encode_reconcile_init(const ReconcileInit& init);

/// Decode a ReconcileInit message. Returns nullopt on error or version mismatch.
std::optional<ReconcileInit> decode_reconcile_init(std::span<const uint8_t> payload);

/// Encode ReconcileRanges message.
/// Wire: [namespace_id:32][range_count:u32BE][for each: upper_bound:32, mode:1, (mode data)]
std::vector<uint8_t> encode_reconcile_ranges(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<RangeEntry>& ranges);

/// Decode ReconcileRanges message.
std::optional<DecodedRanges> decode_reconcile_ranges(std::span<const uint8_t> payload);

/// Encode ReconcileItems message.
/// Wire: [namespace_id:32][count:u32BE][hash:32]*count
std::vector<uint8_t> encode_reconcile_items(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<Hash32>& items);

/// Decode ReconcileItems message.
std::optional<DecodedItems> decode_reconcile_items(std::span<const uint8_t> payload);

} // namespace chromatindb::sync
