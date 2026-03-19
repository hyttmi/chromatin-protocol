#include "db/sync/reconciliation.h"

#include <algorithm>
#include <cstring>

namespace chromatindb::sync {

// Stub implementations for TDD RED phase

Hash32 xor_fingerprint(const std::vector<Hash32>& /*sorted*/, size_t /*begin*/, size_t /*end*/) {
    return Hash32{};
}

std::pair<size_t, size_t> range_indices(const std::vector<Hash32>& /*sorted*/,
                                        const Hash32& /*lower_bound*/,
                                        const Hash32& /*upper_bound*/) {
    return {0, 0};
}

size_t count_in_range(const std::vector<Hash32>& /*sorted*/,
                      const Hash32& /*lower_bound*/,
                      const Hash32& /*upper_bound*/) {
    return 0;
}

ReconcileResult process_ranges(
    const std::vector<Hash32>& /*our_sorted*/,
    const std::vector<RangeEntry>& /*received_ranges*/) {
    return {};
}

std::pair<std::vector<Hash32>, std::vector<Hash32>> reconcile_local(
    const std::vector<Hash32>& /*sorted_a*/,
    const std::vector<Hash32>& /*sorted_b*/) {
    return {{}, {}};
}

std::vector<uint8_t> encode_reconcile_init(const ReconcileInit& /*init*/) {
    return {};
}

std::optional<ReconcileInit> decode_reconcile_init(std::span<const uint8_t> /*payload*/) {
    return std::nullopt;
}

std::vector<uint8_t> encode_reconcile_ranges(
    std::span<const uint8_t, 32> /*namespace_id*/,
    const std::vector<RangeEntry>& /*ranges*/) {
    return {};
}

std::optional<DecodedRanges> decode_reconcile_ranges(std::span<const uint8_t> /*payload*/) {
    return std::nullopt;
}

std::vector<uint8_t> encode_reconcile_items(
    std::span<const uint8_t, 32> /*namespace_id*/,
    const std::vector<Hash32>& /*items*/) {
    return {};
}

std::optional<DecodedItems> decode_reconcile_items(std::span<const uint8_t> /*payload*/) {
    return std::nullopt;
}

} // namespace chromatindb::sync
