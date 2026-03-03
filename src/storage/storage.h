#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "wire/codec.h"

namespace chromatin::storage {

/// Result of a store_blob operation.
enum class StoreResult {
    Stored,     ///< Blob successfully stored (new entry).
    Duplicate,  ///< Blob already exists (content-addressed dedup).
    Error       ///< Storage error occurred.
};

/// Clock function type for injectable time. Returns Unix timestamp in seconds.
using Clock = std::function<uint64_t()>;

/// Default clock: system wall clock in seconds since epoch.
uint64_t system_clock_seconds();

/// Persistent blob storage engine backed by libmdbx.
///
/// Manages three sub-databases:
/// - blobs:    [namespace:32][hash:32] -> FlatBuffer-encoded blob
/// - sequence: [namespace:32][seq_be:8] -> hash:32
/// - expiry:   [expiry_ts_be:8][hash:32] -> namespace:32
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

    /// Run the TTL expiry scanner.
    /// Deletes all blobs with expiry_timestamp <= now from blobs and expiry indexes.
    /// Sequence index entries are NOT deleted (gaps are expected).
    /// @return Number of blobs purged.
    size_t run_expiry_scan();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chromatin::storage
