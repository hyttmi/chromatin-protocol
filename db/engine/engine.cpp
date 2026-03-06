#include "db/engine/engine.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include "db/crypto/hash.h"
#include "db/crypto/signing.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

namespace chromatin::engine {

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

BlobEngine::BlobEngine(storage::Storage& store)
    : storage_(store) {}

IngestResult BlobEngine::ingest(const wire::BlobData& blob) {
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

    // Step 2: Namespace ownership check
    auto derived_ns = crypto::sha3_256(blob.pubkey);
    if (derived_ns != blob.namespace_id) {
        spdlog::warn("Ingest rejected: namespace mismatch (claimed {:02x}{:02x}...)",
                     blob.namespace_id[0], blob.namespace_id[1]);
        return IngestResult::rejection(IngestError::namespace_mismatch,
            "SHA3-256(pubkey) != namespace_id");
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

} // namespace chromatin::engine
