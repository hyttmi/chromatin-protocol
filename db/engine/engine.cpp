#include "db/engine/engine.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include <asio/this_coro.hpp>

#include <chrono>
#include <cstring>

#include "db/crypto/hash.h"
#include "db/crypto/signing.h"
#include "db/crypto/thread_pool.h"
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
                       uint64_t namespace_quota_count,
                       uint64_t max_ttl_seconds)
    : storage_(store)
    , pool_(pool)
    , max_storage_bytes_(max_storage_bytes)
    , namespace_quota_bytes_(namespace_quota_bytes)
    , namespace_quota_count_(namespace_quota_count)
    , max_ttl_seconds_(max_ttl_seconds) {}

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
    std::span<const uint8_t, 32> target_namespace,
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

    // Step 0e: Max TTL enforcement (tombstones + BOMBs exempt — must be permanent)
    // D-13(1): BOMB is permanent like a single tombstone, exempt from max_ttl.
    if (max_ttl_seconds_ > 0 && !wire::is_tombstone(blob.data) && !wire::is_bomb(blob.data)) {
        if (blob.ttl == 0) {
            co_return IngestResult::rejection(IngestError::invalid_ttl,
                "permanent blobs not allowed (max_ttl_seconds=" +
                std::to_string(max_ttl_seconds_) + ")");
        }
        if (blob.ttl > max_ttl_seconds_) {
            co_return IngestResult::rejection(IngestError::invalid_ttl,
                "TTL " + std::to_string(blob.ttl) +
                " exceeds max " + std::to_string(max_ttl_seconds_));
        }
    }

    // Step 1: Structural checks (no inline pubkey, only signature)
    if (blob.signature.empty()) {
        spdlog::warn("Ingest rejected: empty signature");
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "empty signature");
    }

    // Step 1.5 (D-03): PUBK-first invariant.
    // Runs BEFORE any crypto::offload — the adversarial-flood defense (Pitfall #6):
    // no registered owner for target_namespace AND inbound blob is not a PUBK => reject
    // without burning ML-DSA-87 verify CPU. This single site is the node-level gate;
    // sync inherits it because SyncProtocol::ingest_blobs delegates to engine.ingest
    // (no parallel check per feedback_no_duplicate_code.md).
    if (!storage_.has_owner_pubkey(target_namespace)) {
        if (!wire::is_pubkey_blob(blob.data)) {
            spdlog::warn("Ingest rejected: PUBK-first violation (ns {:02x}{:02x}... has no registered owner)",
                         target_namespace[0], target_namespace[1]);
            co_return IngestResult::rejection(IngestError::pubk_first_violation,
                "first write to namespace must be PUBK");
        }
        // else: a PUBK blob is allowed to register a new namespace -- fall through.
    }

    // Step 1.7 (D-13): BOMB structural validation.
    // Runs BEFORE crypto::offload — Pitfall #6 adversarial-flood defense
    // (mirrors Step 1.5 PUBK-first discipline). Cheap integer checks
    // only; uses helpers from Plan 01 — no inline payload re-parsing.
    //
    // Gate uses has_bomb_magic (magic + min-size, NOT structure) so malformed
    // BOMBs (magic present but count/size inconsistent) enter this block and
    // get rejected with bomb_malformed. If we gated on is_bomb (strict), a
    // malformed BOMB would fail is_bomb, skip the gate, and get accepted as
    // an opaque signed blob — silent corruption of the "node rejects malformed
    // BOMB at ingest" contract.
    if (wire::has_bomb_magic(blob.data)) {
        if (blob.ttl != 0) {
            spdlog::warn("Ingest rejected: BOMB with ttl={} (MUST be 0; ns {:02x}{:02x}...)",
                         blob.ttl, target_namespace[0], target_namespace[1]);
            co_return IngestResult::rejection(IngestError::bomb_ttl_nonzero,
                "BOMB must have ttl=0 (permanent)");
        }
        if (!wire::validate_bomb_structure(blob.data)) {
            spdlog::warn("Ingest rejected: BOMB structural check failed (size={}, ns {:02x}{:02x}...)",
                         blob.data.size(), target_namespace[0], target_namespace[1]);
            co_return IngestResult::rejection(IngestError::bomb_malformed,
                "BOMB structural check failed (size mismatch or too short)");
        }
        // count == 0 is structurally valid (Plan 01 A2): side-effect loop runs
        // zero iterations; no DoS amplification. Full PQ signature per empty BOMB
        // makes this economically unattractive to attackers.
    }

    // Step 2 (D-09): resolve signing pubkey via owner_pubkeys DBI
    // (owner write) or delegation_map (delegate write). For PUBK blobs on
    // fresh namespaces the owner_pubkeys lookup misses — we fall through to
    // the PUBK body as the source of truth (verify against the embedded
    // signing pubkey). Registration into owner_pubkeys happens only AFTER
    // successful signature verification, at Step 4.5.
    std::optional<std::array<uint8_t, 2592>> resolved_pubkey;
    bool is_owner = false;
    bool is_delegate = false;

    auto owner_pk_opt = storage_.get_owner_pubkey(blob.signer_hint);
    if (owner_pk_opt.has_value()) {
        // Owner write: cross-check SHA3(pubkey) == target_namespace as integrity guard
        // (T-122-07: owner_pubkeys DBI corruption pointing to wrong namespace).
        auto hint_check = co_await crypto::offload(pool_, [&owner_pk_opt]() {
            return crypto::sha3_256(
                std::span<const uint8_t>(owner_pk_opt->data(), owner_pk_opt->size()));
        });
        // CONC-04: post back to ioc_ before Storage access.
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        if (std::memcmp(hint_check.data(), target_namespace.data(), 32) == 0) {
            is_owner = true;
            resolved_pubkey = owner_pk_opt;
        }
        // else: signer_hint resolves to a pubkey that doesn't own target_namespace.
        // Fall through to delegation check.
    }

    if (!is_owner && !resolved_pubkey.has_value() && wire::is_pubkey_blob(blob.data)) {
        // Fresh-namespace PUBK registration path: owner_pubkeys lookup missed
        // (has_owner_pubkey(target_namespace) was false at Step 1.5 so the
        // blob.signer_hint lookup was guaranteed to miss too for the fresh
        // namespace case). Verify the PUBK's embedded signing pubkey against
        // target_namespace using the body-supplied pk, then register at Step 4.5.
        auto embedded_sk = wire::extract_pubk_signing_pk(blob.data);
        auto hint_check = co_await crypto::offload(pool_, [embedded_sk]() {
            return crypto::sha3_256(std::span<const uint8_t>(embedded_sk.data(), 2592));
        });
        // CONC-04: post back to ioc_ before Storage access.
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        if (std::memcmp(hint_check.data(), target_namespace.data(), 32) != 0) {
            spdlog::warn("Ingest rejected: PUBK embedded signing pk does not hash to target_namespace (ns {:02x}{:02x}...)",
                         target_namespace[0], target_namespace[1]);
            co_return IngestResult::rejection(IngestError::namespace_mismatch,
                "SHA3(PUBK signing pk) != target_namespace");
        }
        std::array<uint8_t, 2592> resolved_bytes{};
        std::memcpy(resolved_bytes.data(), embedded_sk.data(), 2592);
        resolved_pubkey = resolved_bytes;
        is_owner = true;
    }

    if (!is_owner) {
        // Delegation path: composite-key lookup on delegation_map via signer_hint
        // (Storage::get_delegate_pubkey_by_hint).
        auto delegate_pk_opt = storage_.get_delegate_pubkey_by_hint(target_namespace, blob.signer_hint);
        if (delegate_pk_opt.has_value()) {
            is_delegate = true;
            resolved_pubkey = delegate_pk_opt;
        }
    }

    if (!is_owner && !is_delegate) {
        spdlog::warn("Ingest rejected: unknown signer_hint for namespace {:02x}{:02x}... (no owner_pubkey, no delegation)",
                     target_namespace[0], target_namespace[1]);
        co_return IngestResult::rejection(IngestError::no_delegation,
            "signer_hint not found in owner_pubkeys or delegation_map");
    }

    if (is_delegate) {
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

        // D-12: Delegates cannot emit BOMB blobs (batched-tombstone
        // is owner-privileged, same rationale as single tombstones above).
        if (wire::is_bomb(blob.data)) {
            spdlog::warn("Ingest rejected: delegates cannot create BOMB blobs (ns {:02x}{:02x}...)",
                         target_namespace[0], target_namespace[1]);
            co_return IngestResult::rejection(IngestError::bomb_delegate_not_allowed,
                "delegates cannot create BOMB blobs");
        }
    }

    // Encode blob ONCE -- reused for quota check, dedup, tombstone check, and storage.
    auto encoded = wire::encode_blob(blob);

    // Step 2a: Fast-reject optimization (not authoritative -- store_blob rechecks atomically, RES-03)
    // Tombstones exempt: consistent with Step 0b capacity exemption
    if (!wire::is_tombstone(blob.data)) {
        auto [byte_limit, count_limit] = effective_quota(target_namespace);
        if (byte_limit > 0 || count_limit > 0) {
            auto current = storage_.get_namespace_quota(target_namespace);
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
    // CONC-04: post back to ioc_ before Storage access.
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);

    // Dedup check on event loop: skip expensive ML-DSA-87 verification for already-stored blobs.
    // Duplicates pay only ONE thread pool round-trip (blob_hash), not two.
    if (storage_.has_blob(target_namespace, content_hash)) {
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
    // Only for NEW blobs that passed dedup check. Uses target_namespace (from
    // transport envelope) as the first sponge input — cross-namespace replay
    // defense (D-13): delegate signing for N_A submitted as N_B produces a
    // different signing_input digest, so verify fails.
    auto [signing_input, verify_ok] = co_await crypto::offload(pool_,
        [&blob, target_namespace, &resolved_pubkey]() {
            auto si = wire::build_signing_input(
                target_namespace, blob.data, blob.ttl, blob.timestamp);
            bool ok = crypto::Signer::verify(si, blob.signature,
                std::span<const uint8_t>(resolved_pubkey->data(), 2592));
            return std::pair{si, ok};
        });
    // CONC-04: post back to ioc_ before Storage access.
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);

    if (!verify_ok) {
        spdlog::warn("Ingest rejected: invalid signature (ns {:02x}{:02x}...)",
                     target_namespace[0], target_namespace[1]);
        co_return IngestResult::rejection(IngestError::invalid_signature,
            "ML-DSA-87 signature verification failed");
    }

    // Step 3.5: Tombstone / BOMB handling for incoming blobs
    // Reuse already-computed content_hash and encoded -- no redundant encode/hash.
    if (wire::is_tombstone(blob.data)) {
        // Tombstone blob arriving via sync or direct ingest.
        // Delete the target blob if it exists, then store the tombstone.
        auto target_hash = wire::extract_tombstone_target(blob.data);
        storage_.delete_blob_data(target_namespace, target_hash);
        spdlog::debug("Tombstone received: deleting target blob in ns {:02x}{:02x}...",
                       target_namespace[0], target_namespace[1]);
    } else if (wire::is_bomb(blob.data)) {
        // D-05 / D-14: BOMB (batched-tombstone) side-effect.
        // Iterate the declared target_hashes and delete each. Runs on the
        // io_context thread (post-back-to-ioc at :340 already happened). Each
        // delete is a single MDBX txn; batch-txn optimization is YAGNI until
        // measured. D-14: no per-target existence check — BOMB can legitimately
        // pre-mark not-yet-received blobs. Regular-blob block-by-tombstone is
        // skipped (BOMB IS a batched tombstone; no has_tombstone_for call).
        auto targets = wire::extract_bomb_targets(blob.data);
        for (const auto& target_hash : targets) {
            storage_.delete_blob_data(target_namespace, target_hash);
        }
        spdlog::debug("BOMB received: deleted {} target blob(s) in ns {:02x}{:02x}...",
                       targets.size(), target_namespace[0], target_namespace[1]);
    } else {
        // Regular blob: check if a tombstone blocks it.
        if (storage_.has_tombstone_for(target_namespace, content_hash)) {
            spdlog::debug("Ingest rejected: blob blocked by tombstone (ns {:02x}{:02x}...)",
                           target_namespace[0], target_namespace[1]);
            co_return IngestResult::rejection(IngestError::tombstoned,
                "blocked by tombstone");
        }
    }

    // Step 4.5 (D-04 / D-05): if accepted blob is a PUBK, register the
    // embedded signing pubkey in owner_pubkeys. register_owner_pubkey THROWS on
    // D-04 mismatch — we catch and surface as IngestError::pubk_mismatch.
    // Idempotent-match (same bytes) returns silently and is the expected
    // steady-state for re-ingest of an already-registered PUBK.
    if (wire::is_pubkey_blob(blob.data)) {
        auto embedded_sk = wire::extract_pubk_signing_pk(blob.data);
        try {
            storage_.register_owner_pubkey(blob.signer_hint, embedded_sk);
        } catch (const std::exception& e) {
            spdlog::warn("Ingest rejected: PUBK_MISMATCH (ns {:02x}{:02x}...): {}",
                         target_namespace[0], target_namespace[1], e.what());
            co_return IngestResult::rejection(IngestError::pubk_mismatch,
                "PUBK signing pubkey differs from registered owner");
        }
    }

    // Step 4: Store to storage layer -- pass pre-computed hash and encoded bytes
    // RES-03: Pass limits to store_blob for atomic check inside write transaction
    auto [byte_limit, count_limit] = effective_quota(target_namespace);
    auto store_result = storage_.store_blob(target_namespace, blob, content_hash, encoded,
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
    std::span<const uint8_t, 32> target_namespace,
    const wire::BlobData& delete_request,
    std::shared_ptr<net::Connection> source) {
    // The delete_request is a BlobData where:
    //   signer_hint = SHA3-256(signing pubkey) (no inline pubkey post-122)
    //   data        = tombstone data (4-byte magic + 32-byte target hash = 36 bytes)
    //   ttl         = 0 (permanent)
    //   signature   = over canonical form
    //                 SHA3-256(target_namespace || tombstone_data || 0 || timestamp)
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

    // Step 1: Structural checks (no inline pubkey)
    if (delete_request.signature.empty()) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "empty signature");
    }

    // Validate data is tombstone format
    if (!wire::is_tombstone(delete_request.data)) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "delete request data must be tombstone format (4-byte magic + 32-byte hash)");
    }

    // Step 2 (D-09): resolve signing pubkey via owner_pubkeys DBI
    // (owner tombstone) or delegation_map (delegate tombstone -- currently
    // rejected in the engine, but we still need to resolve the signer to
    // verify the signature before the owner-only check fires). No PUBK-first
    // gate here: a delete to a namespace without a registered owner falls
    // through to no_delegation rejection (tombstones are never PUBKs).
    std::optional<std::array<uint8_t, 2592>> resolved_pubkey;
    bool is_owner = false;

    auto owner_pk_opt = storage_.get_owner_pubkey(delete_request.signer_hint);
    if (owner_pk_opt.has_value()) {
        auto hint_check = co_await crypto::offload(pool_, [&owner_pk_opt]() {
            return crypto::sha3_256(
                std::span<const uint8_t>(owner_pk_opt->data(), owner_pk_opt->size()));
        });
        // CONC-04: post back to ioc_ before Storage access.
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        if (std::memcmp(hint_check.data(), target_namespace.data(), 32) == 0) {
            is_owner = true;
            resolved_pubkey = owner_pk_opt;
        }
    }

    if (!is_owner) {
        // Delegates cannot create tombstone blobs (deletion is owner-privileged).
        // Mirror the ingest-path rule: even if a delegation entry resolves
        // signer_hint, we reject the delete as not-owner-authorized.
        auto delegate_pk_opt =
            storage_.get_delegate_pubkey_by_hint(target_namespace, delete_request.signer_hint);
        if (delegate_pk_opt.has_value()) {
            spdlog::warn("Delete rejected: delegates cannot create tombstone blobs (ns {:02x}{:02x}...)",
                         target_namespace[0], target_namespace[1]);
            co_return IngestResult::rejection(IngestError::no_delegation,
                "delegates cannot create tombstone blobs");
        }
        spdlog::warn("Delete rejected: unknown signer_hint for namespace {:02x}{:02x}... (no owner_pubkey, no delegation)",
                     target_namespace[0], target_namespace[1]);
        co_return IngestResult::rejection(IngestError::no_delegation,
            "signer_hint not found in owner_pubkeys or delegation_map");
    }

    // Step 3: Single dispatch -- build_signing_input + verify bundled (using
    // target_namespace from transport envelope + resolved owner pubkey).
    auto [signing_input, verify_ok] = co_await crypto::offload(pool_,
        [&delete_request, target_namespace, &resolved_pubkey]() {
            auto si = wire::build_signing_input(
                target_namespace, delete_request.data,
                delete_request.ttl, delete_request.timestamp);
            bool ok = crypto::Signer::verify(si, delete_request.signature,
                std::span<const uint8_t>(resolved_pubkey->data(), 2592));
            return std::pair{si, ok};
        });
    // CONC-04: post back to ioc_ before Storage access.
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);

    if (!verify_ok) {
        co_return IngestResult::rejection(IngestError::invalid_signature,
            "ML-DSA-87 signature verification failed");
    }

    // Step 4: Extract target hash and delete target blob
    auto target_hash = wire::extract_tombstone_target(delete_request.data);
    storage_.delete_blob_data(target_namespace, target_hash);

    // Step 5: Store the tombstone blob (the delete_request IS the tombstone)
    auto store_result = storage_.store_blob(target_namespace, delete_request);

    switch (store_result.status) {
        case storage::StoreResult::Status::Stored: {
            spdlog::info("Blob deleted via tombstone in ns {:02x}{:02x}...",
                          target_namespace[0], target_namespace[1]);
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
        case storage::StoreResult::Status::CapacityExceeded:
            co_return IngestResult::rejection(IngestError::storage_full,
                "storage capacity exceeded (atomic check)");
        case storage::StoreResult::Status::QuotaExceeded:
            co_return IngestResult::rejection(IngestError::quota_exceeded,
                "namespace quota exceeded (atomic check)");
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

} // namespace chromatindb::engine
