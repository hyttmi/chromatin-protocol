#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <asio/thread_pool.hpp>

#include "db/storage/storage.h"
#include "db/wire/codec.h"

namespace chromatindb::engine {

/// Error codes for blob ingest rejection.
enum class IngestError {
    namespace_mismatch,   ///< SHA3-256(pubkey) != claimed namespace_id.
    invalid_signature,    ///< ML-DSA-87 signature verification failed.
    malformed_blob,       ///< Structural issue (wrong pubkey size, empty sig).
    oversized_blob,       ///< Blob data exceeds MAX_BLOB_DATA_SIZE.
    storage_error,        ///< Storage layer failed to write.
    tombstoned,           ///< Blob rejected because a tombstone exists for it.
    no_delegation,        ///< Delegate write rejected: no valid delegation exists.
    storage_full,         ///< Storage capacity exceeded (max_storage_bytes).
    quota_exceeded        ///< Namespace quota exceeded (byte or count limit).
};

/// Status of a successful ingest.
enum class IngestStatus { stored, duplicate };

/// Write acknowledgment returned on successful ingest.
struct WriteAck {
    std::array<uint8_t, 32> blob_hash{};
    uint64_t seq_num = 0;
    IngestStatus status = IngestStatus::stored;
    uint32_t replication_count = 1;  // Stubbed until ACKW-02
};

/// Result of an ingest operation: either accepted (with WriteAck) or rejected (with error).
struct IngestResult {
    bool accepted = false;
    std::optional<WriteAck> ack;
    std::optional<IngestError> error;
    std::string error_detail;

    /// Create a successful ingest result.
    static IngestResult success(WriteAck ack);

    /// Create a rejection result.
    static IngestResult rejection(IngestError err, std::string detail = "");
};

/// Blob validation and ingestion engine.
///
/// Validates blobs in fail-fast order (structural -> namespace -> signature)
/// before storing. Accepts blobs for ANY valid namespace, not just the local node's.
///
/// Thread safety: NOT thread-safe. Caller must synchronize access.
class BlobEngine {
public:
    /// Construct a BlobEngine backed by the given storage.
    /// @param store Reference to storage (must outlive this engine).
    /// @param pool Thread pool for crypto offload (must outlive this engine).
    /// @param max_storage_bytes Capacity limit in bytes (0 = unlimited).
    /// @param namespace_quota_bytes Global namespace byte limit (0 = unlimited).
    /// @param namespace_quota_count Global namespace blob count limit (0 = unlimited).
    explicit BlobEngine(storage::Storage& store,
                        asio::thread_pool& pool,
                        uint64_t max_storage_bytes = 0,
                        uint64_t namespace_quota_bytes = 0,
                        uint64_t namespace_quota_count = 0);

    /// Update quota configuration (called on SIGHUP config reload).
    void set_quota_config(uint64_t quota_bytes, uint64_t quota_count,
                          const std::map<std::string, std::pair<std::optional<uint64_t>,
                              std::optional<uint64_t>>>& overrides);

    /// Validate and ingest a blob.
    ///
    /// Validation pipeline (fail-fast, cheap to expensive):
    /// 1. Structural checks (pubkey size, signature non-empty)
    /// 2. Namespace ownership (SHA3-256(pubkey) == namespace_id)
    /// 3. Signature verification (ML-DSA-87)
    /// 4. Store to storage layer
    ///
    /// @return IngestResult with WriteAck on success or error on rejection.
    IngestResult ingest(const wire::BlobData& blob);

    /// Delete a blob by creating a signed tombstone.
    ///
    /// The delete_request BlobData must contain:
    /// - namespace_id: target namespace
    /// - pubkey: owner's ML-DSA-87 public key
    /// - data: 32-byte target blob hash (raw bytes, not tombstone-encoded)
    /// - ttl: 0
    /// - timestamp: client's unix timestamp
    /// - signature: over canonical form SHA3-256(namespace || data || 0 || timestamp)
    ///
    /// Validation: same pipeline as ingest (structural -> namespace -> signature).
    /// On success: deletes target blob if present, stores tombstone, returns ack.
    IngestResult delete_blob(const wire::BlobData& delete_request);

    /// Query blobs in a namespace since a given seq_num.
    /// Returns blobs with seq_num > since_seq in ascending order.
    /// @param namespace_id The 32-byte namespace to query.
    /// @param since_seq Return blobs with seq_num strictly greater than this.
    /// @param max_count Maximum number of blobs to return (0 = no limit).
    std::vector<wire::BlobData> get_blobs_since(
        std::span<const uint8_t, 32> namespace_id,
        uint64_t since_seq,
        uint32_t max_count = 0);

    /// Query a single blob by namespace + content hash.
    /// @return The blob data if found, nullopt otherwise.
    std::optional<wire::BlobData> get_blob(
        std::span<const uint8_t, 32> namespace_id,
        std::span<const uint8_t, 32> blob_hash);

    /// List all namespaces with at least one stored blob.
    /// @return Namespace IDs with their latest seq_num.
    std::vector<storage::NamespaceInfo> list_namespaces();

private:
    /// Resolve effective quota limits for a namespace (override > global).
    /// Returns {byte_limit, count_limit} where 0 = unlimited.
    std::pair<uint64_t, uint64_t> effective_quota(
        std::span<const uint8_t, 32> namespace_id) const;

    storage::Storage& storage_;
    asio::thread_pool& pool_;
    uint64_t max_storage_bytes_ = 0;
    uint64_t namespace_quota_bytes_ = 0;
    uint64_t namespace_quota_count_ = 0;
    // Per-namespace overrides: key is raw 32-byte namespace hash
    // Value: {optional max_bytes, optional max_count}
    std::map<std::array<uint8_t, 32>, std::pair<std::optional<uint64_t>,
             std::optional<uint64_t>>> namespace_quota_overrides_;
};

} // namespace chromatindb::engine
