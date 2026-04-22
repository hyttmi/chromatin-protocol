#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/thread_pool.hpp>

#include "db/crypto/thread_pool.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

namespace chromatindb::net { class Connection; }

namespace chromatindb::engine {

/// Error codes for blob ingest rejection.
enum class IngestError {
    namespace_mismatch,   ///< SHA3-256(resolved_pubkey) != target_namespace.
    invalid_signature,    ///< ML-DSA-87 signature verification failed.
    malformed_blob,       ///< Structural issue (empty sig, malformed body).
    oversized_blob,       ///< Blob data exceeds MAX_BLOB_DATA_SIZE.
    storage_error,        ///< Storage layer failed to write.
    tombstoned,           ///< Blob rejected because a tombstone exists for it.
    no_delegation,        ///< Delegate write rejected: no valid delegation exists.
    storage_full,         ///< Storage capacity exceeded (max_storage_bytes).
    quota_exceeded,       ///< Namespace quota exceeded (byte or count limit).
    timestamp_rejected,   ///< Blob timestamp too far in future or past.
    invalid_ttl,          ///< Tombstone with non-zero TTL.
    pubk_first_violation, ///< D-03: first write to namespace was non-PUBK.
    pubk_mismatch,        ///< D-04: incoming PUBK signing pubkey differs from registered owner.
    bomb_ttl_nonzero,     ///< D-13(1): BOMB with ttl != 0 (BOMB must be permanent).
    bomb_malformed,       ///< D-13(2): BOMB header structural sanity failed (size mismatch).
    bomb_delegate_not_allowed ///< D-12: delegates cannot emit BOMB blobs.
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
    std::shared_ptr<net::Connection> source;  // For notification fan-out (nullptr = client write)

    /// Create a successful ingest result.
    static IngestResult success(WriteAck ack);

    /// Create a rejection result.
    static IngestResult rejection(IngestError err, std::string detail = "");
};

/// Blob validation and ingestion engine.
///
/// verify flow:
///   structural -> PUBK-first gate -> owner_pubkeys lookup (owner)
///              OR delegation_map lookup (delegate) -> sig verify.
/// Accepts blobs for ANY valid namespace, not just the local node's.
///
/// Thread safety: thread-confined to the io_context executor. BlobEngine
/// holds references to Storage and a thread_pool; the pool is used for
/// crypto offload via `co_await crypto::offload(pool, ...)`, after which
/// the coroutine MUST post back to the coroutine's executor before touching
/// Storage:
///
///   co_await asio::post(co_await asio::this_coro::executor,
///                       asio::use_awaitable);
///
/// See engine.cpp for the canonical example. Enforced for Storage by
/// STORAGE_THREAD_CHECK() at every public Storage method.
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
                        uint64_t namespace_quota_count = 0,
                        uint64_t max_ttl_seconds = 0);

    /// Update storage capacity limit (called on SIGHUP config reload).
    void set_max_storage_bytes(uint64_t max_storage_bytes);

    /// Update max TTL (called on SIGHUP config reload).
    void set_max_ttl_seconds(uint64_t max_ttl) { max_ttl_seconds_ = max_ttl; }

    /// Update quota configuration (called on SIGHUP config reload).
    void set_quota_config(uint64_t quota_bytes, uint64_t quota_count,
                          const std::map<std::string, std::pair<std::optional<uint64_t>,
                              std::optional<uint64_t>>>& overrides);

    /// Validate and ingest a blob.
    ///
    /// Validation pipeline (fail-fast, cheap to expensive):
    /// 1. Structural checks (signature non-empty)
    /// 1.5. PUBK-first gate (D-03): if target_namespace has no registered
    ///      owner_pubkey, the incoming blob MUST have the PUBK magic; else
    ///      reject with pubk_first_violation. Runs BEFORE any crypto offload.
    /// 2. Resolve signing pubkey: owner_pubkeys[blob.signer_hint] (owner) or
    ///    delegation_map[target_namespace || blob.signer_hint] (delegate).
    /// 3. Content hash (blob_hash) -- offloaded to pool, dedup check on event loop
    /// 4. Signature verification using resolved pubkey + target_namespace in
    ///    build_signing_input -- offloaded to pool.
    /// 4.5. If accepted blob is a PUBK, register embedded signing pubkey via
    ///      Storage::register_owner_pubkey (throws on D-04 mismatch).
    /// 5. Store to storage layer.
    ///
    /// Two-dispatch pattern: duplicates pay only one pool round-trip (blob_hash).
    ///
    /// @param target_namespace 32-byte target namespace from the transport envelope.
    /// @param blob Signed BlobData (post-122 schema: signer_hint, not inline pubkey).
    /// @return IngestResult with WriteAck on success or error on rejection.
    asio::awaitable<IngestResult> ingest(
        std::span<const uint8_t, 32> target_namespace,
        const wire::BlobData& blob,
        std::shared_ptr<net::Connection> source = nullptr);

    /// Delete a blob by creating a signed tombstone.
    ///
    /// The delete_request BlobData must contain:
    /// - signer_hint: SHA3-256 of signer's ML-DSA-87 public key
    /// - data: tombstone body (4-byte magic + 32-byte target hash = 36 bytes)
    /// - ttl: 0 (permanent)
    /// - timestamp: client's unix timestamp
    /// - signature: over canonical form
    ///   SHA3-256(target_namespace || tombstone_data || 0 || timestamp)
    ///
    /// Validation: owner_pubkeys lookup + delegation fallback + signature
    /// (same as ingest, minus PUBK-first gate — tombstones are never PUBKs, so
    /// a delete to a namespace without a registered owner falls through to
    /// no_delegation).
    /// On success: deletes target blob if present, stores tombstone, returns ack.
    ///
    /// @param target_namespace 32-byte target namespace from the transport envelope.
    /// @param delete_request Signed tombstone blob.
    asio::awaitable<IngestResult> delete_blob(
        std::span<const uint8_t, 32> target_namespace,
        const wire::BlobData& delete_request,
        std::shared_ptr<net::Connection> source = nullptr);

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

    /// Resolve effective quota limits for a namespace (override > global).
    /// Returns {byte_limit, count_limit} where 0 = unlimited.
    std::pair<uint64_t, uint64_t> effective_quota(
        std::span<const uint8_t, 32> namespace_id) const;

private:
    storage::Storage& storage_;
    asio::thread_pool& pool_;
    uint64_t max_storage_bytes_ = 0;
    uint64_t namespace_quota_bytes_ = 0;
    uint64_t namespace_quota_count_ = 0;
    uint64_t max_ttl_seconds_ = 0;
    // Per-namespace overrides: key is raw 32-byte namespace hash
    // Value: {optional max_bytes, optional max_count}
    std::map<std::array<uint8_t, 32>, std::pair<std::optional<uint64_t>,
             std::optional<uint64_t>>> namespace_quota_overrides_;
};

} // namespace chromatindb::engine
