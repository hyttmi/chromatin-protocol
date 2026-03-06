#include "db/sync/sync_protocol.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <unordered_set>

namespace chromatindb::sync {

SyncProtocol::SyncProtocol(engine::BlobEngine& engine, storage::Clock clock)
    : engine_(engine), clock_(clock) {}

// =============================================================================
// Expiry check
// =============================================================================

bool SyncProtocol::is_blob_expired(const wire::BlobData& blob, uint64_t now) {
    if (blob.ttl == 0) return false;  // Permanent blob
    return (blob.timestamp + blob.ttl) <= now;
}

// =============================================================================
// Hash collection
// =============================================================================

std::vector<std::array<uint8_t, 32>> SyncProtocol::collect_namespace_hashes(
    std::span<const uint8_t, 32> namespace_id) {
    uint64_t now = clock_();
    auto blobs = engine_.get_blobs_since(namespace_id, 0);

    std::vector<std::array<uint8_t, 32>> hashes;
    hashes.reserve(blobs.size());

    for (const auto& blob : blobs) {
        if (is_blob_expired(blob, now)) {
            continue;  // Skip expired blobs (SYNC-03)
        }
        auto encoded = wire::encode_blob(blob);
        auto hash = wire::blob_hash(encoded);
        hashes.push_back(hash);
    }

    return hashes;
}

// =============================================================================
// Hash-list diff
// =============================================================================

std::vector<std::array<uint8_t, 32>> SyncProtocol::diff_hashes(
    const std::vector<std::array<uint8_t, 32>>& ours,
    const std::vector<std::array<uint8_t, 32>>& theirs) {
    // Build a set of our hashes for O(1) lookup
    std::unordered_set<std::string> our_set;
    our_set.reserve(ours.size());
    for (const auto& h : ours) {
        our_set.insert(std::string(reinterpret_cast<const char*>(h.data()), h.size()));
    }

    // Find hashes in theirs not in ours
    std::vector<std::array<uint8_t, 32>> missing;
    for (const auto& h : theirs) {
        std::string key(reinterpret_cast<const char*>(h.data()), h.size());
        if (our_set.find(key) == our_set.end()) {
            missing.push_back(h);
        }
    }

    return missing;
}

// =============================================================================
// Blob retrieval by hash
// =============================================================================

std::vector<wire::BlobData> SyncProtocol::get_blobs_by_hashes(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<std::array<uint8_t, 32>>& hashes) {
    std::vector<wire::BlobData> blobs;
    blobs.reserve(hashes.size());

    for (const auto& hash : hashes) {
        auto blob = engine_.get_blob(namespace_id, hash);
        if (blob.has_value()) {
            blobs.push_back(std::move(*blob));
        }
    }

    return blobs;
}

// =============================================================================
// Blob ingestion
// =============================================================================

SyncStats SyncProtocol::ingest_blobs(const std::vector<wire::BlobData>& blobs) {
    SyncStats stats;
    uint64_t now = clock_();

    for (const auto& blob : blobs) {
        // Skip expired blobs (SYNC-03)
        if (is_blob_expired(blob, now)) {
            spdlog::debug("skipping expired blob during sync ingest");
            continue;
        }

        auto result = engine_.ingest(blob);
        if (result.accepted) {
            stats.blobs_received++;
        } else if (result.error.has_value()) {
            spdlog::warn("sync ingest rejected blob: {}",
                         result.error_detail.empty() ? "unknown" : result.error_detail);
        }
    }

    return stats;
}

// =============================================================================
// Big-endian encoding helpers
// =============================================================================

namespace {

void write_u32_be(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void write_u64_be(std::vector<uint8_t>& buf, uint64_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 56) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t read_u64_be(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
           static_cast<uint64_t>(p[7]);
}

} // anonymous namespace

// =============================================================================
// Message encoding/decoding
// =============================================================================

std::vector<uint8_t> SyncProtocol::encode_namespace_list(
    const std::vector<storage::NamespaceInfo>& namespaces) {
    // Format: [count:u32BE][ns1:32B][seq1:u64BE]...
    std::vector<uint8_t> buf;
    buf.reserve(4 + namespaces.size() * 40);  // 32 + 8 per entry

    write_u32_be(buf, static_cast<uint32_t>(namespaces.size()));
    for (const auto& ns : namespaces) {
        buf.insert(buf.end(), ns.namespace_id.begin(), ns.namespace_id.end());
        write_u64_be(buf, ns.latest_seq_num);
    }

    return buf;
}

std::vector<storage::NamespaceInfo> SyncProtocol::decode_namespace_list(
    std::span<const uint8_t> payload) {
    if (payload.size() < 4) return {};

    uint32_t count = read_u32_be(payload.data());
    size_t expected = 4 + count * 40;
    if (payload.size() < expected) return {};

    std::vector<storage::NamespaceInfo> result;
    result.reserve(count);

    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        storage::NamespaceInfo info;
        std::memcpy(info.namespace_id.data(), payload.data() + offset, 32);
        offset += 32;
        info.latest_seq_num = read_u64_be(payload.data() + offset);
        offset += 8;
        result.push_back(info);
    }

    return result;
}

std::vector<uint8_t> SyncProtocol::encode_hash_list(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<std::array<uint8_t, 32>>& hashes) {
    // Format: [ns:32B][count:u32BE][hash1:32B]...[hashN:32B]
    std::vector<uint8_t> buf;
    buf.reserve(32 + 4 + hashes.size() * 32);

    buf.insert(buf.end(), namespace_id.begin(), namespace_id.end());
    write_u32_be(buf, static_cast<uint32_t>(hashes.size()));
    for (const auto& h : hashes) {
        buf.insert(buf.end(), h.begin(), h.end());
    }

    return buf;
}

std::pair<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>>
SyncProtocol::decode_hash_list(std::span<const uint8_t> payload) {
    std::array<uint8_t, 32> ns{};
    std::vector<std::array<uint8_t, 32>> hashes;

    if (payload.size() < 36) return {ns, hashes};  // 32 + 4 minimum

    std::memcpy(ns.data(), payload.data(), 32);
    uint32_t count = read_u32_be(payload.data() + 32);

    size_t expected = 36 + count * 32;
    if (payload.size() < expected) return {ns, hashes};

    hashes.reserve(count);
    size_t offset = 36;
    for (uint32_t i = 0; i < count; ++i) {
        std::array<uint8_t, 32> h;
        std::memcpy(h.data(), payload.data() + offset, 32);
        offset += 32;
        hashes.push_back(h);
    }

    return {ns, hashes};
}

std::vector<uint8_t> SyncProtocol::encode_blob_transfer(
    const std::vector<wire::BlobData>& blobs) {
    // Format: [count:u32BE][len1:u32BE][blob1_flatbuf]...[lenN:u32BE][blobN_flatbuf]
    std::vector<uint8_t> buf;
    write_u32_be(buf, static_cast<uint32_t>(blobs.size()));

    for (const auto& blob : blobs) {
        auto encoded = wire::encode_blob(blob);
        write_u32_be(buf, static_cast<uint32_t>(encoded.size()));
        buf.insert(buf.end(), encoded.begin(), encoded.end());
    }

    return buf;
}

std::vector<wire::BlobData> SyncProtocol::decode_blob_transfer(
    std::span<const uint8_t> payload) {
    if (payload.size() < 4) return {};

    uint32_t count = read_u32_be(payload.data());
    std::vector<wire::BlobData> blobs;
    blobs.reserve(count);

    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + 4 > payload.size()) break;
        uint32_t len = read_u32_be(payload.data() + offset);
        offset += 4;

        if (offset + len > payload.size()) break;
        auto blob = wire::decode_blob(payload.subspan(offset, len));
        blobs.push_back(std::move(blob));
        offset += len;
    }

    return blobs;
}

} // namespace chromatindb::sync
