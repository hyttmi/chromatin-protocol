#include "db/engine/engine.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include <asio/this_coro.hpp>

#include <chrono>
#include <cstring>

#include "db/crypto/hash.h"
#include "db/crypto/signing.h"
#include "db/crypto/thread_pool.h"
#include "db/engine/chunking.h"
#include "db/net/framing.h"
#include "db/storage/storage.h"
#include "db/util/hex.h"
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

namespace {

/// Convert 64-char hex string to 32-byte array.
/// Caller must ensure hex.size() == 64 and all chars are valid hex.
std::array<uint8_t, 32> hex_to_bytes32(const std::string& hex) {
    std::array<uint8_t, 32> result{};
    for (size_t i = 0; i < 32; ++i) {
        auto byte_str = hex.substr(i * 2, 2);
        result[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
    }
    return result;
}

} // anonymous namespace

BlobEngine::BlobEngine(storage::Storage& store,
                       asio::thread_pool& pool,
                       uint64_t max_storage_bytes,
                       uint64_t namespace_quota_bytes,
                       uint64_t namespace_quota_count)
    : storage_(store)
    , pool_(pool)
    , max_storage_bytes_(max_storage_bytes)
    , namespace_quota_bytes_(namespace_quota_bytes)
    , namespace_quota_count_(namespace_quota_count) {}

void BlobEngine::set_max_storage_bytes(uint64_t max_storage_bytes) {
    max_storage_bytes_ = max_storage_bytes;
}

void BlobEngine::set_quota_config(
    uint64_t quota_bytes, uint64_t quota_count,
    const std::map<std::string, std::pair<std::optional<uint64_t>,
        std::optional<uint64_t>>>& overrides) {
    namespace_quota_bytes_ = quota_bytes;
    namespace_quota_count_ = quota_count;
    namespace_quota_overrides_.clear();
    for (const auto& [hex_key, limits] : overrides) {
        if (hex_key.size() == 64) {
            namespace_quota_overrides_[hex_to_bytes32(hex_key)] = limits;
        }
    }
}

std::pair<uint64_t, uint64_t> BlobEngine::effective_quota(
    std::span<const uint8_t, 32> namespace_id) const {
    std::array<uint8_t, 32> ns_key;
    std::memcpy(ns_key.data(), namespace_id.data(), 32);
    auto it = namespace_quota_overrides_.find(ns_key);
    if (it != namespace_quota_overrides_.end()) {
        uint64_t byte_limit = it->second.first.has_value()
            ? it->second.first.value() : namespace_quota_bytes_;
        uint64_t count_limit = it->second.second.has_value()
            ? it->second.second.value() : namespace_quota_count_;
        return {byte_limit, count_limit};
    }
    return {namespace_quota_bytes_, namespace_quota_count_};
}

asio::awaitable<IngestResult> BlobEngine::ingest(
    const wire::BlobData& blob,
    std::shared_ptr<net::Connection> source) {
    // Step 0: Size check (cheapest possible -- one integer comparison)
    if (blob.data.size() > net::MAX_BLOB_DATA_SIZE) {
        spdlog::warn("Ingest rejected: blob data size {} exceeds max {}",
                     blob.data.size(), net::MAX_BLOB_DATA_SIZE);
        co_return IngestResult::rejection(IngestError::oversized_blob,
            "blob data size " + std::to_string(blob.data.size()) +
            " exceeds max " + std::to_string(net::MAX_BLOB_DATA_SIZE));
    }

    // Step 0b: Fast-reject optimization (not authoritative -- store_blob rechecks atomically, RES-03)
    // Tombstones exempt: small (36 bytes) and they free space by deleting target
    if (max_storage_bytes_ > 0 && !wire::is_tombstone(blob.data)) {
        if (storage_.used_bytes() >= max_storage_bytes_) {
            spdlog::warn("Ingest rejected: storage capacity exceeded ({} >= {} bytes)",
                         storage_.used_bytes(), max_storage_bytes_);
            co_return IngestResult::rejection(IngestError::storage_full,
                "storage capacity exceeded");
        }
    }

    // Step 0c: Timestamp validation (cheap integer compare, before any crypto)
    {
        constexpr uint64_t MAX_FUTURE_SECONDS = 3600;         // 1 hour
        constexpr uint64_t MAX_PAST_SECONDS = 30 * 24 * 3600; // 30 days
        auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (blob.timestamp > now + MAX_FUTURE_SECONDS) {
            spdlog::warn("Ingest rejected: timestamp too far in future (more than 1 hour ahead)");
            co_return IngestResult::rejection(IngestError::timestamp_rejected,
                "timestamp too far in future (more than 1 hour ahead)");
        }
        if (blob.timestamp < now - MAX_PAST_SECONDS) {
            spdlog::warn("Ingest rejected: timestamp too far in past (more than 30 days ago)");
            co_return IngestResult::rejection(IngestError::timestamp_rejected,
                "timestamp too far in past (more than 30 days ago)");
        }
    }

    // Step 0d: Already-expired blob rejection
    if (blob.ttl > 0) {
        auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (wire::saturating_expiry(blob.timestamp, blob.ttl) <= now) {
            spdlog::warn("Ingest rejected: blob already expired");
            co_return IngestResult::rejection(IngestError::timestamp_rejected,
                "blob already expired");
        }
    }

    // Step 1: Structural checks (cheapest first)
    if (blob.pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        spdlog::warn("Ingest rejected: wrong pubkey size {} (expected {})",
                     blob.pubkey.size(), crypto::Signer::PUBLIC_KEY_SIZE);
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "pubkey size " + std::to_string(blob.pubkey.size()) +
            " != " + std::to_string(crypto::Signer::PUBLIC_KEY_SIZE));
    }

    if (blob.signature.empty()) {
        spdlog::warn("Ingest rejected: empty signature");
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "empty signature");
    }

    // Step 2: Namespace ownership OR delegation check
    // Offload SHA3-256(pubkey) to thread pool (uniform model: all sha3_256 offloaded)
    auto derived_ns = co_await crypto::offload(pool_, [&blob]() {
        return crypto::sha3_256(blob.pubkey);
    });
    bool is_owner = (derived_ns == blob.namespace_id);
    bool is_delegate = false;

    if (!is_owner) {
        // Not the owner -- check if this pubkey has a valid delegation
        is_delegate = storage_.has_valid_delegation(blob.namespace_id, blob.pubkey);
        if (!is_delegate) {
            spdlog::warn("Ingest rejected: no ownership or delegation for namespace {:02x}{:02x}...",
                         blob.namespace_id[0], blob.namespace_id[1]);
            co_return IngestResult::rejection(IngestError::no_delegation,
                "pubkey has no ownership or valid delegation for this namespace");
        }

        // Delegates cannot create delegation blobs (only owners can)
        if (wire::is_delegation(blob.data)) {
            spdlog::warn("Ingest rejected: delegates cannot create delegation blobs");
            co_return IngestResult::rejection(IngestError::no_delegation,
                "delegates cannot create delegation blobs");
        }

        // Delegates cannot create tombstone blobs (deletion is owner-privileged)
        if (wire::is_tombstone(blob.data)) {
            spdlog::warn("Ingest rejected: delegates cannot create tombstone blobs");
            co_return IngestResult::rejection(IngestError::no_delegation,
                "delegates cannot create tombstone blobs");
        }
    }

    // Encode blob ONCE -- reused for quota check, dedup, tombstone check, and storage.
    auto encoded = wire::encode_blob(blob);

    // Step 2a: Fast-reject optimization (not authoritative -- store_blob rechecks atomically, RES-03)
    // Tombstones exempt: consistent with Step 0b capacity exemption
    if (!wire::is_tombstone(blob.data)) {
        auto [byte_limit, count_limit] = effective_quota(blob.namespace_id);
        if (byte_limit > 0 || count_limit > 0) {
            auto current = storage_.get_namespace_quota(blob.namespace_id);
            if (byte_limit > 0 && current.total_bytes + encoded.size() > byte_limit) {
                spdlog::warn("Ingest rejected: namespace byte quota exceeded ({} + {} > {})",
                             current.total_bytes, encoded.size(), byte_limit);
                co_return IngestResult::rejection(IngestError::quota_exceeded,
                    "namespace byte quota exceeded");
            }
            if (count_limit > 0 && current.blob_count + 1 > count_limit) {
                spdlog::warn("Ingest rejected: namespace count quota exceeded ({} + 1 > {})",
                             current.blob_count, count_limit);
                co_return IngestResult::rejection(IngestError::quota_exceeded,
                    "namespace count quota exceeded");
            }
        }
    }

    // Step 2.5: First dispatch -- blob_hash offloaded to thread pool
    auto content_hash = co_await crypto::offload(pool_, [&encoded]() {
        return wire::blob_hash(encoded);
    });

    // Dedup check on event loop: skip expensive ML-DSA-87 verification for already-stored blobs.
    // Duplicates pay only ONE thread pool round-trip (blob_hash), not two.
    if (storage_.has_blob(blob.namespace_id, content_hash)) {
        WriteAck ack;
        ack.blob_hash = content_hash;
        ack.seq_num = 0;  // Not looked up for dedup short-circuit (safe: see RESEARCH.md Pitfall 5)
        ack.status = IngestStatus::duplicate;
        ack.replication_count = 1;
        auto result = IngestResult::success(std::move(ack));
        result.source = std::move(source);
        co_return result;
    }

    // Step 3: Second dispatch -- build_signing_input + verify BUNDLED (most expensive)
    // Only for NEW blobs that passed dedup check.
    auto [signing_input, verify_ok] = co_await crypto::offload(pool_, [&blob]() {
        auto si = wire::build_signing_input(
            blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
        bool ok = crypto::Signer::verify(si, blob.signature, blob.pubkey);
        return std::pair{si, ok};
    });

    if (!verify_ok) {
        spdlog::warn("Ingest rejected: invalid signature (ns {:02x}{:02x}...)",
                     blob.namespace_id[0], blob.namespace_id[1]);
        co_return IngestResult::rejection(IngestError::invalid_signature,
            "ML-DSA-87 signature verification failed");
    }

    // Step 3.5: Tombstone handling for incoming blobs
    // Reuse already-computed content_hash and encoded -- no redundant encode/hash.
    if (wire::is_tombstone(blob.data)) {
        // Tombstone blob arriving via sync or direct ingest.
        // Delete the target blob if it exists, then store the tombstone.
        auto target_hash = wire::extract_tombstone_target(blob.data);
        storage_.delete_blob_data(blob.namespace_id, target_hash);
        spdlog::debug("Tombstone received: deleting target blob in ns {:02x}{:02x}...",
                       blob.namespace_id[0], blob.namespace_id[1]);
    } else {
        // Regular blob: check if a tombstone blocks it.
        if (storage_.has_tombstone_for(blob.namespace_id, content_hash)) {
            spdlog::debug("Ingest rejected: blob blocked by tombstone (ns {:02x}{:02x}...)",
                           blob.namespace_id[0], blob.namespace_id[1]);
            co_return IngestResult::rejection(IngestError::tombstoned,
                "blocked by tombstone");
        }
    }

    // Step 4: Store to storage layer -- pass pre-computed hash and encoded bytes
    // RES-03: Pass limits to store_blob for atomic check inside write transaction
    auto [byte_limit, count_limit] = effective_quota(blob.namespace_id);
    auto store_result = storage_.store_blob(blob, content_hash, encoded,
        wire::is_tombstone(blob.data) ? 0 : max_storage_bytes_,
        byte_limit, count_limit);

    switch (store_result.status) {
        case storage::StoreResult::Status::Stored: {
            WriteAck ack;
            ack.blob_hash = store_result.blob_hash;
            ack.seq_num = store_result.seq_num;
            ack.status = IngestStatus::stored;
            ack.replication_count = 1;
            auto result = IngestResult::success(std::move(ack));
            result.source = std::move(source);
            co_return result;
        }
        case storage::StoreResult::Status::Duplicate: {
            WriteAck ack;
            ack.blob_hash = store_result.blob_hash;
            ack.seq_num = store_result.seq_num;
            ack.status = IngestStatus::duplicate;
            ack.replication_count = 1;
            auto result = IngestResult::success(std::move(ack));
            result.source = std::move(source);
            co_return result;
        }
        case storage::StoreResult::Status::CapacityExceeded:
            co_return IngestResult::rejection(IngestError::storage_full,
                "storage capacity exceeded (atomic check)");
        case storage::StoreResult::Status::QuotaExceeded:
            co_return IngestResult::rejection(IngestError::quota_exceeded,
                "namespace quota exceeded (atomic check)");
        case storage::StoreResult::Status::Error:
            co_return IngestResult::rejection(IngestError::storage_error,
                "storage write failed");
    }

    // Unreachable, but satisfy compiler
    co_return IngestResult::rejection(IngestError::storage_error, "unknown status");
}

asio::awaitable<IngestResult> BlobEngine::delete_blob(
    const wire::BlobData& delete_request,
    std::shared_ptr<net::Connection> source) {
    // The delete_request is a BlobData where:
    //   data = tombstone data (4-byte magic + 32-byte target hash = 36 bytes)
    //   ttl = 0 (permanent)
    //   signature = over canonical form SHA3-256(namespace || tombstone_data || 0 || timestamp)
    //
    // This design means the tombstone is directly storable and verifiable on any node.

    // Step 0c: Timestamp validation (cheap integer compare, before any crypto)
    {
        constexpr uint64_t MAX_FUTURE_SECONDS = 3600;         // 1 hour
        constexpr uint64_t MAX_PAST_SECONDS = 30 * 24 * 3600; // 30 days
        auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (delete_request.timestamp > now + MAX_FUTURE_SECONDS) {
            spdlog::warn("Delete rejected: timestamp too far in future (more than 1 hour ahead)");
            co_return IngestResult::rejection(IngestError::timestamp_rejected,
                "timestamp too far in future (more than 1 hour ahead)");
        }
        if (delete_request.timestamp < now - MAX_PAST_SECONDS) {
            spdlog::warn("Delete rejected: timestamp too far in past (more than 30 days ago)");
            co_return IngestResult::rejection(IngestError::timestamp_rejected,
                "timestamp too far in past (more than 30 days ago)");
        }
    }

    // Step 0d: Tombstone TTL validation (cheap integer compare)
    if (delete_request.ttl != 0) {
        spdlog::warn("Delete rejected: tombstone must have TTL=0 (permanent)");
        co_return IngestResult::rejection(IngestError::invalid_ttl,
            "tombstone must have TTL=0 (permanent)");
    }

    // Step 1: Structural checks
    if (delete_request.pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "pubkey size " + std::to_string(delete_request.pubkey.size()) +
            " != " + std::to_string(crypto::Signer::PUBLIC_KEY_SIZE));
    }

    if (delete_request.signature.empty()) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "empty signature");
    }

    // Validate data is tombstone format
    if (!wire::is_tombstone(delete_request.data)) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "delete request data must be tombstone format (4-byte magic + 32-byte hash)");
    }

    // Step 2: Namespace ownership check
    // Offload SHA3-256(pubkey) to thread pool (uniform model: all sha3_256 offloaded)
    auto derived_ns = co_await crypto::offload(pool_, [&delete_request]() {
        return crypto::sha3_256(delete_request.pubkey);
    });
    if (derived_ns != delete_request.namespace_id) {
        co_return IngestResult::rejection(IngestError::namespace_mismatch,
            "SHA3-256(pubkey) != namespace_id");
    }

    // Step 3: Single dispatch -- build_signing_input + verify bundled
    auto [signing_input, verify_ok] = co_await crypto::offload(pool_,
        [&delete_request]() {
            auto si = wire::build_signing_input(
                delete_request.namespace_id, delete_request.data,
                delete_request.ttl, delete_request.timestamp);
            bool ok = crypto::Signer::verify(si, delete_request.signature,
                                              delete_request.pubkey);
            return std::pair{si, ok};
        });

    if (!verify_ok) {
        co_return IngestResult::rejection(IngestError::invalid_signature,
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
            auto result = IngestResult::success(std::move(ack));
            result.source = std::move(source);
            co_return result;
        }
        case storage::StoreResult::Status::Duplicate: {
            // Idempotent: tombstone already exists
            WriteAck ack;
            ack.blob_hash = store_result.blob_hash;
            ack.seq_num = store_result.seq_num;
            ack.status = IngestStatus::duplicate;
            ack.replication_count = 1;
            auto result = IngestResult::success(std::move(ack));
            result.source = std::move(source);
            co_return result;
        }
        case storage::StoreResult::Status::Error:
            co_return IngestResult::rejection(IngestError::storage_error,
                "storage write failed for tombstone");
    }

    co_return IngestResult::rejection(IngestError::storage_error, "unknown status");
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

// =============================================================================
// Chunked storage API
// =============================================================================

asio::awaitable<IngestResult> BlobEngine::store_chunked(
    std::span<const uint8_t, 32> /*namespace_id*/,
    std::span<const uint8_t> /*pubkey*/,
    std::span<const uint8_t> /*data*/,
    uint32_t /*ttl*/,
    uint64_t /*timestamp*/,
    SignFn /*signature_fn*/) {
    // Stub: always reject (TDD RED)
    co_return IngestResult::rejection(IngestError::storage_error, "not implemented");
}

ChunkedReadResult BlobEngine::read_chunked(
    std::span<const uint8_t, 32> /*namespace_id*/,
    std::span<const uint8_t, 32> /*manifest_hash*/) {
    // Stub: always fail (TDD RED)
    return ChunkedReadResult{};
}

} // namespace chromatindb::engine
