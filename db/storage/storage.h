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
        Stored,     ///< Blob successfully stored (new entry).
        Duplicate,  ///< Blob already exists (content-addressed dedup).
        Error       ///< Storage error occurred.
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

/// Clock function type for injectable time. Returns Unix timestamp in seconds.
using Clock = std::function<uint64_t()>;

/// Default clock: system wall clock in seconds since epoch.
uint64_t system_clock_seconds();

/// Persistent blob storage engine backed by libmdbx.
///
/// Manages five sub-databases:
/// - blobs:      [namespace:32][hash:32] -> FlatBuffer-encoded blob
/// - sequence:   [namespace:32][seq_be:8] -> hash:32
/// - expiry:     [expiry_ts_be:8][hash:32] -> namespace:32
/// - delegation: [namespace:32][delegate_pk_hash:32] -> delegation_blob_hash:32
/// - tombstone:  [namespace:32][target_hash:32] -> (empty, existence check only)
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
    /// Does NOT touch seq_map (gaps are expected per existing pattern).
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

    /// Run the TTL expiry scanner.
    /// Deletes all blobs with expiry_timestamp <= now from blobs and expiry indexes.
    /// Sequence index entries are NOT deleted (gaps are expected).
    /// @return Number of blobs purged.
    size_t run_expiry_scan();

    /// Return the current database file size in bytes.
    /// Uses mdbx env info (O(1), authoritative).
    uint64_t used_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chromatindb::storage
