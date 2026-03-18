#pragma once

#include "db/engine/engine.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace chromatindb::sync {

/// Statistics from a sync round.
struct SyncStats {
    uint32_t blobs_sent = 0;
    uint32_t blobs_received = 0;
    uint32_t namespaces_synced = 0;
    uint32_t storage_full_count = 0;
    uint32_t quota_exceeded_count = 0;
};

/// Sync protocol logic: hash-list diff, expiry filtering, message encoding.
///
/// This class is intentionally NOT a coroutine class. It provides synchronous
/// helper methods for the sync algorithm. The async orchestration (sending
/// messages over connections) lives in PeerManager.
///
/// Thread safety: NOT thread-safe. Caller must synchronize access.
class SyncProtocol {
public:
    /// Construct with a BlobEngine, Storage, and injectable clock.
    /// Storage is used for index-only hash reads during sync.
    explicit SyncProtocol(engine::BlobEngine& engine,
                          storage::Storage& storage,
                          storage::Clock clock = storage::system_clock_seconds);

    /// Check if a blob has expired given the current time.
    /// Returns true if ttl > 0 AND timestamp + ttl <= now.
    static bool is_blob_expired(const wire::BlobData& blob, uint64_t now);

    /// Collect blob hashes for a namespace from the storage index.
    /// Reads hashes directly from seq_map without loading blob data.
    std::vector<std::array<uint8_t, 32>> collect_namespace_hashes(
        std::span<const uint8_t, 32> namespace_id);

    /// Compute the set difference: hashes in `theirs` not present in `ours`.
    static std::vector<std::array<uint8_t, 32>> diff_hashes(
        const std::vector<std::array<uint8_t, 32>>& ours,
        const std::vector<std::array<uint8_t, 32>>& theirs);

    /// Retrieve blobs by their content hashes from a specific namespace.
    std::vector<wire::BlobData> get_blobs_by_hashes(
        std::span<const uint8_t, 32> namespace_id,
        const std::vector<std::array<uint8_t, 32>>& hashes);

    /// Callback invoked after a blob is successfully ingested during sync.
    /// Parameters: namespace_id, blob_hash, seq_num, blob_data_size, is_tombstone.
    using OnBlobIngested = std::function<void(
        const std::array<uint8_t, 32>& namespace_id,
        const std::array<uint8_t, 32>& blob_hash,
        uint64_t seq_num,
        uint32_t blob_data_size,
        bool is_tombstone)>;

    /// Set the callback for successful sync blob ingests (pub/sub notifications).
    void set_on_blob_ingested(OnBlobIngested callback);

    /// Ingest received blobs. Validates and stores each non-expired blob.
    /// Returns stats: how many were accepted.
    SyncStats ingest_blobs(const std::vector<wire::BlobData>& blobs);

    // =========================================================================
    // Message encoding/decoding for sync protocol
    // =========================================================================

    /// Encode a namespace list.
    /// Wire format: [count:u32BE][ns1:32B][seq1:u64BE]...[nsN:32B][seqN:u64BE]
    static std::vector<uint8_t> encode_namespace_list(
        const std::vector<storage::NamespaceInfo>& namespaces);

    /// Decode a namespace list from wire bytes.
    static std::vector<storage::NamespaceInfo> decode_namespace_list(
        std::span<const uint8_t> payload);

    /// Encode a hash list for a specific namespace.
    /// Wire format: [ns:32B][count:u32BE][hash1:32B]...[hashN:32B]
    static std::vector<uint8_t> encode_hash_list(
        std::span<const uint8_t, 32> namespace_id,
        const std::vector<std::array<uint8_t, 32>>& hashes);

    /// Decode a hash list. Returns (namespace_id, hashes).
    static std::pair<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>>
        decode_hash_list(std::span<const uint8_t> payload);

    /// Encode blobs for transfer.
    /// Wire format: [count:u32BE][len1:u32BE][blob1_flatbuf]...[lenN:u32BE][blobN_flatbuf]
    static std::vector<uint8_t> encode_blob_transfer(
        const std::vector<wire::BlobData>& blobs);

    /// Encode a single blob for transfer (count=1, reuses existing wire format).
    /// Used for one-blob-at-a-time sync to keep memory bounded.
    static std::vector<uint8_t> encode_single_blob_transfer(
        const wire::BlobData& blob);

    /// Decode blobs from a transfer message.
    static std::vector<wire::BlobData> decode_blob_transfer(
        std::span<const uint8_t> payload);

private:
    engine::BlobEngine& engine_;
    storage::Storage& storage_;
    storage::Clock clock_;
    OnBlobIngested on_blob_ingested_;
};

} // namespace chromatindb::sync
