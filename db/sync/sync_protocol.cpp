#include "db/sync/sync_protocol.h"

#include <spdlog/spdlog.h>

#include <cstring>

#include "db/util/endian.h"

namespace chromatindb::sync {

SyncProtocol::SyncProtocol(engine::BlobEngine& engine,
                           storage::Storage& storage,
                           asio::thread_pool& pool,
                           storage::Clock clock)
    : engine_(engine), storage_(storage), pool_(pool), clock_(clock) {}

// =============================================================================
// Hash collection
// =============================================================================

// SYNC-03: Snapshot consistency analysis (MDBX MVCC safety)
// get_hashes_by_namespace() opens an MDBX read transaction (start_read at storage.cpp:626),
// providing MVCC snapshot isolation for the hash list. Individual get_blob() calls open
// separate read transactions, but this is safe:
//   - A concurrent store_blob (new blob) won't appear in our hash list -> caught next sync round
//   - A concurrent delete (expired blob) -> get_blob returns nullopt, we skip it (line 31)
// The returned vector is by-value (independent snapshot). No code change needed.
std::vector<std::array<uint8_t, 32>> SyncProtocol::collect_namespace_hashes(
    std::span<const uint8_t, 32> namespace_id) {
    auto all_hashes = storage_.get_hashes_by_namespace(namespace_id);
    uint64_t now = clock_();

    std::vector<std::array<uint8_t, 32>> result;
    result.reserve(all_hashes.size());

    for (const auto& hash : all_hashes) {
        auto blob = storage_.get_blob(namespace_id, hash);
        if (!blob) continue;
        if (wire::is_blob_expired(*blob, now)) {
            spdlog::debug("filtered expired blob in collect_namespace_hashes");
            continue;
        }
        result.push_back(hash);
    }

    return result;
}

// =============================================================================
// Blob retrieval by hash
// =============================================================================

std::vector<wire::BlobData> SyncProtocol::get_blobs_by_hashes(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<std::array<uint8_t, 32>>& hashes) {
    std::vector<wire::BlobData> blobs;
    blobs.reserve(hashes.size());
    uint64_t now = clock_();

    for (const auto& hash : hashes) {
        auto blob = engine_.get_blob(namespace_id, hash);
        if (blob.has_value()) {
            if (wire::is_blob_expired(*blob, now)) {
                spdlog::debug("filtered expired blob in get_blobs_by_hashes");
                continue;
            }
            blobs.push_back(std::move(*blob));
        }
    }

    return blobs;
}

// =============================================================================
// Blob ingestion
// =============================================================================

void SyncProtocol::set_on_blob_ingested(OnBlobIngested callback) {
    on_blob_ingested_ = std::move(callback);
}

asio::awaitable<SyncStats> SyncProtocol::ingest_blobs(
    const std::vector<NamespacedBlob>& ns_blobs,
    std::shared_ptr<net::Connection> source) {
    SyncStats stats;
    uint64_t now = clock_();
    uint32_t storage_full_count = 0;
    uint32_t quota_exceeded_count = 0;

    for (const auto& nb : ns_blobs) {
        const auto& blob = nb.blob;
        const auto& target_namespace = nb.target_namespace;
        // Skip expired blobs (SYNC-03)
        if (wire::is_blob_expired(blob, now)) {
            spdlog::debug("skipping expired blob during sync ingest");
            continue;
        }

        // Phase 122: target_namespace is threaded per-blob. The PUBK-first gate
        // lives ONCE in engine.cpp Step 1.5 — sync delegates here, no duplicate
        // check in this file (feedback_no_duplicate_code.md).
        auto result = co_await engine_.ingest(
            std::span<const uint8_t, 32>(target_namespace),
            blob, source);
        if (result.accepted) {
            stats.blobs_received++;
            // Notify subscribers about successful sync-received ingest
            if (result.ack.has_value() &&
                result.ack->status == engine::IngestStatus::stored &&
                on_blob_ingested_) {
                uint64_t expiry_time = wire::saturating_expiry(blob.timestamp, blob.ttl);
                on_blob_ingested_(
                    target_namespace,
                    result.ack->blob_hash,
                    result.ack->seq_num,
                    static_cast<uint32_t>(blob.data.size()),
                    wire::is_tombstone(blob.data),
                    expiry_time,
                    source);
            }
        } else if (result.error.has_value()) {
            if (*result.error == engine::IngestError::storage_full) {
                // Skip blob silently -- do not count as received, do not fire callback
                storage_full_count++;
                spdlog::debug("Sync blob skipped: storage full");
            } else if (*result.error == engine::IngestError::quota_exceeded) {
                quota_exceeded_count++;
                spdlog::debug("Sync blob skipped: namespace quota exceeded");
            } else if (*result.error == engine::IngestError::timestamp_rejected) {
                spdlog::debug("Sync blob skipped: timestamp rejected ({})",
                              result.error_detail.empty() ? "unknown" : result.error_detail);
            } else {
                spdlog::warn("sync ingest rejected blob: {}",
                             result.error_detail.empty() ? "unknown" : result.error_detail);
            }
        }
    }

    stats.storage_full_count = storage_full_count;
    stats.quota_exceeded_count = quota_exceeded_count;
    co_return stats;
}

// =============================================================================
// Message encoding/decoding
// =============================================================================

std::vector<uint8_t> SyncProtocol::encode_namespace_list(
    const std::vector<storage::NamespaceInfo>& namespaces) {
    // Format: [count:u32BE][ns1:32B][seq1:u64BE]...
    std::vector<uint8_t> buf;
    auto reserve_size = chromatindb::util::checked_mul(namespaces.size(), size_t{40});
    if (reserve_size) {
        auto total = chromatindb::util::checked_add(size_t{4}, *reserve_size);
        if (total) buf.reserve(*total);
    }

    chromatindb::util::write_u32_be(buf, static_cast<uint32_t>(namespaces.size()));
    for (const auto& ns : namespaces) {
        buf.insert(buf.end(), ns.namespace_id.begin(), ns.namespace_id.end());
        chromatindb::util::write_u64_be(buf, ns.latest_seq_num);
    }

    return buf;
}

std::vector<storage::NamespaceInfo> SyncProtocol::decode_namespace_list(
    std::span<const uint8_t> payload) {
    if (payload.size() < 4) return {};

    uint32_t count = chromatindb::util::read_u32_be(payload.data());
    auto product = chromatindb::util::checked_mul(static_cast<size_t>(count), size_t{40});
    if (!product) return {};
    auto expected = chromatindb::util::checked_add(size_t{4}, *product);
    if (!expected || payload.size() < *expected) return {};

    std::vector<storage::NamespaceInfo> result;
    result.reserve(count);

    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        storage::NamespaceInfo info;
        std::memcpy(info.namespace_id.data(), payload.data() + offset, 32);
        offset += 32;
        info.latest_seq_num = chromatindb::util::read_u64_be(payload.data() + offset);
        offset += 8;
        result.push_back(info);
    }

    return result;
}

std::vector<uint8_t> SyncProtocol::encode_blob_request(
    std::span<const uint8_t, 32> namespace_id,
    const std::vector<std::array<uint8_t, 32>>& hashes) {
    // Format: [ns:32B][count:u32BE][hash1:32B]...[hashN:32B]
    std::vector<uint8_t> buf;
    auto reserve_size = chromatindb::util::checked_mul(hashes.size(), size_t{32});
    if (reserve_size) {
        auto total = chromatindb::util::checked_add(size_t{36}, *reserve_size);
        if (total) buf.reserve(*total);
    }

    buf.insert(buf.end(), namespace_id.begin(), namespace_id.end());
    chromatindb::util::write_u32_be(buf, static_cast<uint32_t>(hashes.size()));
    for (const auto& h : hashes) {
        buf.insert(buf.end(), h.begin(), h.end());
    }

    return buf;
}

std::pair<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>>
SyncProtocol::decode_blob_request(std::span<const uint8_t> payload) {
    std::array<uint8_t, 32> ns{};
    std::vector<std::array<uint8_t, 32>> hashes;

    if (payload.size() < 36) return {ns, hashes};  // 32 + 4 minimum

    std::memcpy(ns.data(), payload.data(), 32);
    uint32_t count = chromatindb::util::read_u32_be(payload.data() + 32);

    auto product = chromatindb::util::checked_mul(static_cast<size_t>(count), size_t{32});
    if (!product) return {ns, hashes};
    auto expected = chromatindb::util::checked_add(size_t{36}, *product);
    if (!expected || payload.size() < *expected) return {ns, hashes};

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
    const std::vector<NamespacedBlob>& ns_blobs) {
    // Phase 122 Pitfall #3: per-blob [ns:32B] prefix so the receiver can route
    // each blob without reading namespace_id from the blob schema (no longer
    // present post-122).
    // Format: [count:u32BE]([ns:32B][len:u32BE][blob_flatbuf])+
    std::vector<uint8_t> buf;
    chromatindb::util::write_u32_be(buf, static_cast<uint32_t>(ns_blobs.size()));

    for (const auto& nb : ns_blobs) {
        buf.insert(buf.end(),
                   nb.target_namespace.begin(), nb.target_namespace.end());
        auto encoded = wire::encode_blob(nb.blob);
        chromatindb::util::write_u32_be(buf, static_cast<uint32_t>(encoded.size()));
        buf.insert(buf.end(), encoded.begin(), encoded.end());
    }

    return buf;
}

std::vector<uint8_t> SyncProtocol::encode_single_blob_transfer(
    std::span<const uint8_t, 32> target_namespace,
    const wire::BlobData& blob) {
    // Phase 122 Pitfall #3: per-blob [ns:32B] prefix (see encode_blob_transfer).
    auto encoded = wire::encode_blob(blob);
    std::vector<uint8_t> buf;
    buf.reserve(4 + 32 + 4 + encoded.size());
    chromatindb::util::write_u32_be(buf, 1);  // count = 1
    buf.insert(buf.end(), target_namespace.begin(), target_namespace.end());
    chromatindb::util::write_u32_be(buf, static_cast<uint32_t>(encoded.size()));
    buf.insert(buf.end(), encoded.begin(), encoded.end());
    return buf;
}

std::vector<NamespacedBlob> SyncProtocol::decode_blob_transfer(
    std::span<const uint8_t> payload) {
    if (payload.size() < 4) return {};

    uint32_t count = chromatindb::util::read_u32_be(payload.data());
    std::vector<NamespacedBlob> ns_blobs;
    ns_blobs.reserve(count);

    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        // Read [ns:32B]
        auto ns_end = chromatindb::util::checked_add(offset, size_t{32});
        if (!ns_end || *ns_end > payload.size()) break;
        std::array<uint8_t, 32> ns{};
        std::memcpy(ns.data(), payload.data() + offset, 32);
        offset = *ns_end;

        // Read [len:u32BE]
        auto len_end = chromatindb::util::checked_add(offset, size_t{4});
        if (!len_end || *len_end > payload.size()) break;
        uint32_t len = chromatindb::util::read_u32_be(payload.data() + offset);
        offset = *len_end;

        // Read [blob_flatbuf]
        auto blob_end = chromatindb::util::checked_add(offset, static_cast<size_t>(len));
        if (!blob_end || *blob_end > payload.size()) break;
        auto blob = wire::decode_blob(payload.subspan(offset, len));
        ns_blobs.push_back({ns, std::move(blob)});
        offset = *blob_end;
    }

    return ns_blobs;
}

} // namespace chromatindb::sync
