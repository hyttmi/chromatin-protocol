#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "wire/codec.h"

namespace chromatin::storage {
class Storage;
}

namespace chromatin::engine {

/// Error codes for blob ingest rejection.
enum class IngestError {
    namespace_mismatch,   ///< SHA3-256(pubkey) != claimed namespace_id.
    invalid_signature,    ///< ML-DSA-87 signature verification failed.
    malformed_blob,       ///< Structural issue (wrong pubkey size, empty sig).
    storage_error         ///< Storage layer failed to write.
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
    explicit BlobEngine(storage::Storage& store);

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

    // Query methods in Plan 02

private:
    storage::Storage& storage_;
};

} // namespace chromatin::engine
