#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "db/crypto/secure_bytes.h"
#include "db/wire/codec.h"

namespace chromatindb::storage {

/// Result of a store_blob operation.
struct StoreResult {
    enum class Status {
        Stored,           ///< Blob successfully stored (new entry).
        Duplicate,        ///< Blob already exists (content-addressed dedup).
        CapacityExceeded, ///< Storage capacity limit exceeded (RES-03).
        QuotaExceeded,    ///< Namespace quota limit exceeded (RES-03).
        Error             ///< Storage error occurred.
    };

    Status status = Status::Error;
    uint64_t seq_num = 0;
    std::array<uint8_t, 32> blob_hash{};
};

/// Info about a namespace in storage.
struct NamespaceInfo {
    std::array<uint8_t, 32> namespace_id{};
    uint64_t latest_seq_num = 0;
};

/// Per-peer per-namespace sync cursor for resumption.
/// Stored in the cursor sub-database and survives restarts.
struct SyncCursor {
    uint64_t seq_num = 0;            ///< Last synced sequence number.
    uint32_t round_count = 0;        ///< Rounds since last full resync.
    uint64_t last_sync_timestamp = 0; ///< Unix timestamp of last sync.
};

/// Clock function type for injectable time. Returns Unix timestamp in seconds.
using Clock = std::function<uint64_t()>;

/// Default clock: system wall clock in seconds since epoch.
uint64_t system_clock_seconds();

/// Result of a compaction operation.
struct CompactResult {
    uint64_t before_bytes = 0;  ///< used_bytes() before compaction.
    uint64_t after_bytes = 0;   ///< used_bytes() after compaction (reopened env).
    uint64_t duration_ms = 0;   ///< Wall-clock time for the compaction.
    bool success = false;       ///< True if compaction completed successfully.
};

/// Per-namespace usage aggregate for quota tracking.
struct NamespaceQuota {
    uint64_t total_bytes = 0;  ///< Total encrypted envelope bytes stored.
    uint64_t blob_count = 0;   ///< Number of blobs stored.
};

/// Entry from the delegation_map for listing active delegations.
struct DelegationEntry {
    std::array<uint8_t, 32> delegate_pk_hash{};       ///< SHA3-256(delegate_pubkey)
    std::array<uint8_t, 32> delegation_blob_hash{};    ///< Content hash of the delegation blob
};

/// Lightweight blob reference for list/pagination queries.
/// Contains only hash and seq_num (no full blob data).
struct BlobRef {
    std::array<uint8_t, 32> blob_hash{};
    uint64_t seq_num = 0;
};

/// Persistent blob storage engine backed by libmdbx.
///
/// Manages seven sub-databases:
/// - blobs:      [namespace:32][hash:32] -> FlatBuffer-encoded blob
/// - sequence:   [namespace:32][seq_be:8] -> hash:32
/// - expiry:     [expiry_ts_be:8][hash:32] -> namespace:32
/// - delegation: [namespace:32][delegate_pk_hash:32] -> delegation_blob_hash:32
/// - tombstone:  [namespace:32][target_hash:32] -> (empty, existence check only)
/// - cursor:     [peer_hash:32][namespace:32] -> [seq_num_be:8][round_count_be:4][last_sync_ts_be:8]
/// - quota:      [namespace:32] -> [total_bytes_be:8][blob_count_be:8]
///
/// Thread safety: NOT thread-safe. Caller must synchronize access.
class Storage {
public:
    /// Open or create a storage engine at the given data directory.
    /// Creates the directory if it does not exist.
    /// @param data_dir Path to the data directory for libmdbx files.
    /// @param clock Injectable clock for testable expiry (defaults to system clock).
    /// @throws std::runtime_error if the database cannot be opened.
    explicit Storage(const std::string& data_dir, Clock clock = system_clock_seconds);

    ~Storage();

    /// Move construction and assignment.
    Storage(Storage&& other) noexcept;
    Storage& operator=(Storage&& other) noexcept;

    /// No copy.
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    /// Store a blob. Content-addressed dedup by SHA3-256 hash.
    /// Assigns a monotonically increasing seq_num per namespace.
    /// Creates expiry index entry for blobs with TTL > 0.
    /// @return Stored if new, Duplicate if already exists, Error on failure.
    StoreResult store_blob(const wire::BlobData& blob);

    /// Store a blob with pre-computed content hash and encoded bytes.
    /// Skips the internal encode_blob() + blob_hash() calls.
    /// Used by engine.ingest() which has already computed these values.
    StoreResult store_blob(const wire::BlobData& blob,
                           const std::array<uint8_t, 32>& precomputed_hash,
                           std::span<const uint8_t> precomputed_encoded);

    /// Store a blob with pre-computed hash/encoded AND capacity/quota limits.
    /// Checks limits atomically inside the write transaction (RES-03, D-10/D-11).
    /// Pass 0 for any limit to skip that check.
    StoreResult store_blob(const wire::BlobData& blob,
                           const std::array<uint8_t, 32>& precomputed_hash,
                           std::span<const uint8_t> precomputed_encoded,
                           uint64_t max_storage_bytes,
                           uint64_t quota_byte_limit,
                           uint64_t quota_count_limit);

    /// Retrieve a blob by namespace + content hash.
    /// @return The blob data if found, nullopt otherwise.
    std::optional<wire::BlobData> get_blob(
        std::span<const uint8_t, 32> ns,
        std::span<const uint8_t, 32> hash);

    /// Check if a blob exists by namespace + content hash.
    bool has_blob(
        std::span<const uint8_t, 32> ns,
        std::span<const uint8_t, 32> hash);

    /// Retrieve blobs in a namespace with seq_num > since_seq.
    /// Returns blobs in ascending seq_num order.
    /// Skips seq entries pointing to deleted blobs (gaps from expiry).
    std::vector<wire::BlobData> get_blobs_by_seq(
        std::span<const uint8_t, 32> ns,
        uint64_t since_seq);

    /// Lightweight query: return {blob_hash, seq_num} pairs for pagination.
    /// Returns pairs with seq_num > since_seq in ascending order, up to max_count.
    /// More efficient than get_blobs_by_seq (reads seq_map only, not blobs_map).
    /// Skips zero-hash sentinels (deleted blobs with seq gaps).
    std::vector<BlobRef> get_blob_refs_since(
        std::span<const uint8_t, 32> ns,
        uint64_t since_seq,
        uint32_t max_count);

    /// Retrieve all blob hashes for a namespace from the seq_map index.
    /// Reads only 32-byte hash values from seq_map without touching blobs_map.
    /// This is the memory-efficient path for sync hash collection.
    /// Hashes are returned in seq_num order (ascending).
    /// Note: may include hashes for blobs deleted by expiry (seq gaps).
    std::vector<std::array<uint8_t, 32>> get_hashes_by_namespace(
        std::span<const uint8_t, 32> ns);

    /// List all namespaces in storage with their latest seq_num.
    /// Scans the sequence sub-database using cursor jumps.
    /// @return Vector of NamespaceInfo with namespace_id and latest_seq_num.
    std::vector<NamespaceInfo> list_namespaces();

    /// Delete a single blob by namespace + content hash.
    /// Removes from blobs_map and expiry_map (if entry exists).
    /// Replaces seq_map entry with zero-hash sentinel to preserve seq_num
    /// monotonicity (required for cursor-based sync change detection).
    /// @return true if found and deleted, false if not found.
    bool delete_blob_data(
        std::span<const uint8_t, 32> ns,
        std::span<const uint8_t, 32> blob_hash);

    /// Check if a tombstone exists for a given target blob hash in a namespace.
    /// Performs O(1) indexed lookup in the tombstone_map sub-database.
    /// @return true if a tombstone blob exists targeting this hash in this namespace.
    bool has_tombstone_for(
        std::span<const uint8_t, 32> ns,
        std::span<const uint8_t, 32> target_blob_hash);

    /// Check if a valid delegation exists for a given namespace+delegate_pubkey pair.
    /// Performs O(1) indexed lookup in the delegation_map sub-database.
    /// @return true if a delegation blob exists for this delegate in this namespace.
    bool has_valid_delegation(
        std::span<const uint8_t, 32> namespace_id,
        std::span<const uint8_t> delegate_pubkey);

    /// Count total tombstone entries across all namespaces.
    /// O(1) via MDBX map statistics (no cursor scan).
    /// @return Number of tombstone entries in tombstone_map.
    uint64_t count_tombstones() const;

    /// Count delegation entries for a specific namespace.
    /// Uses cursor prefix scan on delegation_map with [namespace:32] prefix.
    /// @return Number of delegations for the given namespace.
    uint64_t count_delegations(std::span<const uint8_t, 32> namespace_id) const;

    /// List all delegation entries for a specific namespace.
    /// Uses cursor prefix scan on delegation_map (same pattern as count_delegations).
    /// @return Vector of DelegationEntry with delegate_pk_hash and delegation_blob_hash.
    std::vector<DelegationEntry> list_delegations(std::span<const uint8_t, 32> namespace_id) const;

    /// Run the TTL expiry scanner.
    /// Deletes all blobs with expiry_timestamp <= now from blobs and expiry indexes.
    /// Sequence index entries are NOT deleted (gaps are expected).
    /// @return Number of blobs purged.
    size_t run_expiry_scan();

    /// Query the earliest expiry timestamp in the expiry_map.
    /// O(1) via MDBX cursor seek to first key.
    /// @return The earliest expiry timestamp (wall-clock seconds), or nullopt if no expiring blobs.
    std::optional<uint64_t> get_earliest_expiry() const;

    /// Return the current mmap geometry size in bytes.
    /// Uses mdbx env info mi_geo.current (O(1), authoritative).
    /// Note: This is the mmap file geometry, NOT the actual data volume.
    /// Freed pages are reused internally by libmdbx's B-tree garbage collector.
    /// The file only physically shrinks when freed space exceeds shrink_threshold
    /// (4 MiB). This is correct mmap database behavior, not a bug.
    uint64_t used_bytes() const;

    /// Return the actual data size (mi_last_pgno * pagesize), which reflects
    /// B-tree occupancy rather than mmap file geometry (used_bytes()).
    /// More accurate for storage reporting since it excludes pre-allocated
    /// but unused mmap pages.
    uint64_t used_data_bytes() const;

    /// Perform a live compaction: copy the database with mdbx compactify,
    /// close the current env, swap the compacted copy over the original,
    /// and reopen the env. Reclaims disk space from deleted/fragmented data.
    /// @return CompactResult with before/after bytes, duration, and success flag.
    CompactResult compact();

    /// Create a live compacted backup at the given file path.
    /// Does NOT block concurrent reads/writes.
    /// @param dest_path Destination file path (e.g., "/backups/chromatindb.dat").
    /// @return true if backup succeeded, false on error.
    bool backup(const std::string& dest_path);

    /// Perform a read-only integrity scan of all sub-databases at startup.
    /// Logs entry counts and any cross-reference inconsistencies as warnings.
    /// Does NOT refuse to start on inconsistencies -- informational only.
    void integrity_scan();

    // =========================================================================
    // Sync cursor API
    // =========================================================================

    /// Retrieve a sync cursor for a given peer+namespace pair.
    /// @return The cursor if found, nullopt otherwise.
    std::optional<SyncCursor> get_sync_cursor(
        std::span<const uint8_t, 32> peer_hash,
        std::span<const uint8_t, 32> namespace_id);

    /// Store or update a sync cursor for a given peer+namespace pair.
    void set_sync_cursor(
        std::span<const uint8_t, 32> peer_hash,
        std::span<const uint8_t, 32> namespace_id,
        const SyncCursor& cursor);

    /// Delete a sync cursor for a given peer+namespace pair.
    void delete_sync_cursor(
        std::span<const uint8_t, 32> peer_hash,
        std::span<const uint8_t, 32> namespace_id);

    /// Delete all cursors for a given peer (across all namespaces).
    /// @return Number of cursors deleted.
    size_t delete_peer_cursors(std::span<const uint8_t, 32> peer_hash);

    /// Reset round_count to 0 for all cursors (used on SIGHUP).
    /// Preserves seq_num and last_sync_timestamp.
    /// @return Number of cursors reset.
    size_t reset_all_round_counters();

    /// List unique peer hashes that have stored cursors.
    std::vector<std::array<uint8_t, 32>> list_cursor_peers();

    /// Delete cursors for peers NOT in the known set.
    /// Used at startup to clean up cursors for peers that have been removed.
    /// @return Number of cursors deleted.
    size_t cleanup_stale_cursors(
        const std::vector<std::array<uint8_t, 32>>& known_peer_hashes);

    // =========================================================================
    // Namespace quota API
    // =========================================================================

    /// Get the current quota aggregate for a namespace.
    /// O(1) read from the quota sub-database.
    /// @return NamespaceQuota with total_bytes and blob_count (both 0 if unknown).
    NamespaceQuota get_namespace_quota(std::span<const uint8_t, 32> ns);

    /// Rebuild quota aggregates from actual stored blobs.
    /// Clears the quota sub-database and recomputes from blobs_map.
    /// Called on startup to ensure accuracy.
    void rebuild_quota_aggregates();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chromatindb::storage
