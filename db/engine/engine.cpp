#include "db/engine/engine.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include "db/crypto/hash.h"
#include "db/crypto/signing.h"
#include "db/net/framing.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

namespace chromatindb::engine {

// =============================================================================
// IngestResult factory methods
// =============================================================================

IngestResult IngestResult::success(WriteAck ack) {
    IngestResult r;
    r.accepted = true;
    r.ack = std::move(ack);
    return r;
}

IngestResult IngestResult::rejection(IngestError err, std::string detail) {
    IngestResult r;
    r.accepted = false;
    r.error = err;
    r.error_detail = std::move(detail);
    return r;
}

// =============================================================================
// BlobEngine
// =============================================================================

BlobEngine::BlobEngine(storage::Storage& store, uint64_t max_storage_bytes)
    : storage_(store), max_storage_bytes_(max_storage_bytes) {}

IngestResult BlobEngine::ingest(const wire::BlobData& blob) {
    // Step 0: Size check (cheapest possible -- one integer comparison)
    if (blob.data.size() > net::MAX_BLOB_DATA_SIZE) {
        spdlog::warn("Ingest rejected: blob data size {} exceeds max {}",
                     blob.data.size(), net::MAX_BLOB_DATA_SIZE);
        return IngestResult::rejection(IngestError::oversized_blob,
            "blob data size " + std::to_string(blob.data.size()) +
            " exceeds max " + std::to_string(net::MAX_BLOB_DATA_SIZE));
    }

    // Step 0b: Capacity check (query + comparison, cheaper than crypto)
    // Tombstones exempt: small (36 bytes) and they free space by deleting target
    if (max_storage_bytes_ > 0 && !wire::is_tombstone(blob.data)) {
        if (storage_.used_bytes() >= max_storage_bytes_) {
            spdlog::warn("Ingest rejected: storage capacity exceeded ({} >= {} bytes)",
                         storage_.used_bytes(), max_storage_bytes_);
            return IngestResult::rejection(IngestError::storage_full,
                "storage capacity exceeded");
        }
    }

    // Step 1: Structural checks (cheapest first)
    if (blob.pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        spdlog::warn("Ingest rejected: wrong pubkey size {} (expected {})",
                     blob.pubkey.size(), crypto::Signer::PUBLIC_KEY_SIZE);
        return IngestResult::rejection(IngestError::malformed_blob,
            "pubkey size " + std::to_string(blob.pubkey.size()) +
            " != " + std::to_string(crypto::Signer::PUBLIC_KEY_SIZE));
    }

    if (blob.signature.empty()) {
        spdlog::warn("Ingest rejected: empty signature");
        return IngestResult::rejection(IngestError::malformed_blob,
            "empty signature");
    }

    // Step 2: Namespace ownership OR delegation check
    auto derived_ns = crypto::sha3_256(blob.pubkey);
    bool is_owner = (derived_ns == blob.namespace_id);
    bool is_delegate = false;

    if (!is_owner) {
        // Not the owner -- check if this pubkey has a valid delegation
        is_delegate = storage_.has_valid_delegation(blob.namespace_id, blob.pubkey);
        if (!is_delegate) {
            spdlog::warn("Ingest rejected: no ownership or delegation for namespace {:02x}{:02x}...",
                         blob.namespace_id[0], blob.namespace_id[1]);
            return IngestResult::rejection(IngestError::no_delegation,
                "pubkey has no ownership or valid delegation for this namespace");
        }

        // Delegates cannot create delegation blobs (only owners can)
        if (wire::is_delegation(blob.data)) {
            spdlog::warn("Ingest rejected: delegates cannot create delegation blobs");
            return IngestResult::rejection(IngestError::no_delegation,
                "delegates cannot create delegation blobs");
        }

        // Delegates cannot create tombstone blobs (deletion is owner-privileged)
        if (wire::is_tombstone(blob.data)) {
            spdlog::warn("Ingest rejected: delegates cannot create tombstone blobs");
            return IngestResult::rejection(IngestError::no_delegation,
                "delegates cannot create tombstone blobs");
        }
    }

    // Step 3: Signature verification (most expensive)
    auto signing_input = wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);

    if (!crypto::Signer::verify(signing_input, blob.signature, blob.pubkey)) {
        spdlog::warn("Ingest rejected: invalid signature (ns {:02x}{:02x}...)",
                     blob.namespace_id[0], blob.namespace_id[1]);
        return IngestResult::rejection(IngestError::invalid_signature,
            "ML-DSA-87 signature verification failed");
    }

    // Step 3.5: Tombstone handling for incoming blobs
    if (wire::is_tombstone(blob.data)) {
        // Tombstone blob arriving via sync or direct ingest.
        // Delete the target blob if it exists, then store the tombstone.
        auto target_hash = wire::extract_tombstone_target(blob.data);
        storage_.delete_blob_data(blob.namespace_id, target_hash);
        spdlog::debug("Tombstone received: deleting target blob in ns {:02x}{:02x}...",
                       blob.namespace_id[0], blob.namespace_id[1]);
    } else {
        // Regular blob: check if a tombstone blocks it.
        auto encoded = wire::encode_blob(blob);
        auto content_hash = wire::blob_hash(encoded);
        if (storage_.has_tombstone_for(blob.namespace_id, content_hash)) {
            spdlog::debug("Ingest rejected: blob blocked by tombstone (ns {:02x}{:02x}...)",
                           blob.namespace_id[0], blob.namespace_id[1]);
            return IngestResult::rejection(IngestError::tombstoned,
                "blocked by tombstone");
        }
    }

    // Step 4: Store to storage layer
    auto store_result = storage_.store_blob(blob);

    switch (store_result.status) {
        case storage::StoreResult::Status::Stored: {
            WriteAck ack;
            ack.blob_hash = store_result.blob_hash;
            ack.seq_num = store_result.seq_num;
            ack.status = IngestStatus::stored;
            ack.replication_count = 1;
            return IngestResult::success(std::move(ack));
        }
        case storage::StoreResult::Status::Duplicate: {
            WriteAck ack;
            ack.blob_hash = store_result.blob_hash;
            ack.seq_num = store_result.seq_num;
            ack.status = IngestStatus::duplicate;
            ack.replication_count = 1;
            return IngestResult::success(std::move(ack));
        }
        case storage::StoreResult::Status::Error:
            return IngestResult::rejection(IngestError::storage_error,
                "storage write failed");
    }

    // Unreachable, but satisfy compiler
    return IngestResult::rejection(IngestError::storage_error, "unknown status");
}

IngestResult BlobEngine::delete_blob(const wire::BlobData& delete_request) {
    // The delete_request is a BlobData where:
    //   data = tombstone data (4-byte magic + 32-byte target hash = 36 bytes)
    //   ttl = 0 (permanent)
    //   signature = over canonical form SHA3-256(namespace || tombstone_data || 0 || timestamp)
    //
    // This design means the tombstone is directly storable and verifiable on any node.

    // Step 1: Structural checks
    if (delete_request.pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        return IngestResult::rejection(IngestError::malformed_blob,
            "pubkey size " + std::to_string(delete_request.pubkey.size()) +
            " != " + std::to_string(crypto::Signer::PUBLIC_KEY_SIZE));
    }

    if (delete_request.signature.empty()) {
        return IngestResult::rejection(IngestError::malformed_blob,
            "empty signature");
    }

    // Validate data is tombstone format
    if (!wire::is_tombstone(delete_request.data)) {
        return IngestResult::rejection(IngestError::malformed_blob,
            "delete request data must be tombstone format (4-byte magic + 32-byte hash)");
    }

    // Step 2: Namespace ownership check
    auto derived_ns = crypto::sha3_256(delete_request.pubkey);
    if (derived_ns != delete_request.namespace_id) {
        return IngestResult::rejection(IngestError::namespace_mismatch,
            "SHA3-256(pubkey) != namespace_id");
    }

    // Step 3: Signature verification
    auto signing_input = wire::build_signing_input(
        delete_request.namespace_id, delete_request.data,
        delete_request.ttl, delete_request.timestamp);

    if (!crypto::Signer::verify(signing_input, delete_request.signature,
                                 delete_request.pubkey)) {
        return IngestResult::rejection(IngestError::invalid_signature,
            "ML-DSA-87 signature verification failed");
    }

    // Step 4: Extract target hash and delete target blob
    auto target_hash = wire::extract_tombstone_target(delete_request.data);
    storage_.delete_blob_data(delete_request.namespace_id, target_hash);

    // Step 5: Store the tombstone blob (the delete_request IS the tombstone)
    auto store_result = storage_.store_blob(delete_request);

    switch (store_result.status) {
        case storage::StoreResult::Status::Stored: {
            spdlog::info("Blob deleted via tombstone in ns {:02x}{:02x}...",
                          delete_request.namespace_id[0], delete_request.namespace_id[1]);
            WriteAck ack;
            ack.blob_hash = store_result.blob_hash;
            ack.seq_num = store_result.seq_num;
            ack.status = IngestStatus::stored;
            ack.replication_count = 1;
            return IngestResult::success(std::move(ack));
        }
        case storage::StoreResult::Status::Duplicate: {
            // Idempotent: tombstone already exists
            WriteAck ack;
            ack.blob_hash = store_result.blob_hash;
            ack.seq_num = store_result.seq_num;
            ack.status = IngestStatus::duplicate;
            ack.replication_count = 1;
            return IngestResult::success(std::move(ack));
        }
        case storage::StoreResult::Status::Error:
            return IngestResult::rejection(IngestError::storage_error,
                "storage write failed for tombstone");
    }

    return IngestResult::rejection(IngestError::storage_error, "unknown status");
}

// =============================================================================
// Query methods
// =============================================================================

std::vector<wire::BlobData> BlobEngine::get_blobs_since(
    std::span<const uint8_t, 32> namespace_id,
    uint64_t since_seq,
    uint32_t max_count)
{
    auto results = storage_.get_blobs_by_seq(namespace_id, since_seq);

    if (max_count > 0 && results.size() > max_count) {
        results.resize(max_count);
    }

    return results;
}

std::optional<wire::BlobData> BlobEngine::get_blob(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t, 32> blob_hash)
{
    return storage_.get_blob(namespace_id, blob_hash);
}

std::vector<storage::NamespaceInfo> BlobEngine::list_namespaces() {
    return storage_.list_namespaces();
}

} // namespace chromatindb::engine
