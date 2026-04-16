#include "db/storage/storage.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>

#include <mdbx.h++>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include "db/crypto/aead.h"
#include "db/crypto/hash.h"
#include "db/crypto/master_key.h"
#include "db/util/endian.h"
#include "db/wire/codec.h"

namespace chromatindb::storage {

namespace fs = std::filesystem;

// =============================================================================
// Key construction
// =============================================================================

static std::array<uint8_t, 64> make_blob_key(
    const uint8_t* ns, const uint8_t* hash) {
    std::array<uint8_t, 64> key;
    std::memcpy(key.data(), ns, 32);
    std::memcpy(key.data() + 32, hash, 32);
    return key;
}

static std::array<uint8_t, 40> make_seq_key(
    const uint8_t* ns, uint64_t seq_num) {
    std::array<uint8_t, 40> key;
    std::memcpy(key.data(), ns, 32);
    chromatindb::util::store_u64_be(key.data() + 32, seq_num);
    return key;
}

static std::array<uint8_t, 40> make_expiry_key(
    uint64_t expiry_ts, const uint8_t* hash) {
    std::array<uint8_t, 40> key;
    chromatindb::util::store_u64_be(key.data(), expiry_ts);
    std::memcpy(key.data() + 8, hash, 32);
    return key;
}

static std::array<uint8_t, 64> make_cursor_key(
    const uint8_t* peer_hash, const uint8_t* ns) {
    std::array<uint8_t, 64> key;
    std::memcpy(key.data(), peer_hash, 32);
    std::memcpy(key.data() + 32, ns, 32);
    return key;
}

// Cursor value: [seq_num_be:8][round_count_be:4][last_sync_ts_be:8] = 20 bytes
static constexpr size_t CURSOR_VALUE_SIZE = 20;


static std::array<uint8_t, CURSOR_VALUE_SIZE> encode_cursor_value(
    const SyncCursor& cursor) {
    std::array<uint8_t, CURSOR_VALUE_SIZE> buf;
    chromatindb::util::store_u64_be(buf.data(), cursor.seq_num);
    chromatindb::util::store_u32_be(buf.data() + 8, cursor.round_count);
    chromatindb::util::store_u64_be(buf.data() + 12, cursor.last_sync_timestamp);
    return buf;
}

static SyncCursor decode_cursor_value(const uint8_t* data) {
    SyncCursor cursor;
    cursor.seq_num = chromatindb::util::read_u64_be(data);
    cursor.round_count = chromatindb::util::read_u32_be(data + 8);
    cursor.last_sync_timestamp = chromatindb::util::read_u64_be(data + 12);
    return cursor;
}

template <size_t N>
static mdbx::slice to_slice(const std::array<uint8_t, N>& arr) {
    return mdbx::slice(arr.data(), arr.size());
}

// Sentinel slice for "not found" results from txn.get() 3-arg overload.
static const mdbx::slice not_found_sentinel;

// Quota value: [total_bytes_be:8][blob_count_be:8] = 16 bytes
static constexpr size_t QUOTA_VALUE_SIZE = 16;

static std::array<uint8_t, QUOTA_VALUE_SIZE> encode_quota_value(
    const NamespaceQuota& q) {
    std::array<uint8_t, QUOTA_VALUE_SIZE> buf;
    chromatindb::util::store_u64_be(buf.data(), q.total_bytes);
    chromatindb::util::store_u64_be(buf.data() + 8, q.blob_count);
    return buf;
}

static NamespaceQuota decode_quota_value(const uint8_t* data) {
    NamespaceQuota q;
    q.total_bytes = chromatindb::util::read_u64_be(data);
    q.blob_count = chromatindb::util::read_u64_be(data + 8);
    return q;
}

// =============================================================================
// Encryption at rest constants
// =============================================================================

static constexpr uint8_t ENCRYPTION_VERSION = 0x01;
static constexpr size_t ENVELOPE_OVERHEAD =
    1 + crypto::AEAD::NONCE_SIZE + crypto::AEAD::TAG_SIZE;  // 29 bytes

// =============================================================================

// =============================================================================
// Storage::Impl
// =============================================================================

struct Storage::Impl {
    mdbx::env_managed env;
    mdbx::map_handle blobs_map{0};
    mdbx::map_handle seq_map{0};
    mdbx::map_handle expiry_map{0};
    mdbx::map_handle delegation_map{0};
    mdbx::map_handle tombstone_map{0};
    mdbx::map_handle cursor_map{0};
    mdbx::map_handle quota_map{0};
    Clock clock;
    crypto::SecureBytes blob_key_;  // Derived from master key via HKDF
    std::string data_dir_;          // Stored for compact() reopen

    Impl(const std::string& data_dir, crypto::SecureBytes master_key, Clock clk)
        : clock(std::move(clk)), data_dir_(data_dir) {
        fs::create_directories(data_dir);

        open_env(data_dir);

        // Derive blob encryption key from master key
        blob_key_ = crypto::derive_blob_key(master_key);

        // Validate no unencrypted data exists in the database
        {
            auto rtxn = env.start_read();
            validate_no_unencrypted_data(rtxn);
        }

        spdlog::info("Storage opened at {} with encryption at rest", data_dir);
    }

    /// Open (or reopen) the mdbx environment and sub-databases.
    /// Used by both the constructor and compact().
    void open_env(const std::string& path) {
        // Create parameters with geometry
        mdbx::env_managed::create_parameters create_params;
        create_params.geometry.size_lower = 1 * mdbx::env::geometry::MiB;
        create_params.geometry.size_now = mdbx::env::geometry::default_value;
#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature) && __has_feature(address_sanitizer)
        // ASAN shadow memory consumes 1/8 of virtual address space.
        // Reduce upper limit to avoid MDBX_TOO_LARGE on mmap.
        create_params.geometry.size_upper = 1LL * mdbx::env::geometry::GiB;
#else
        create_params.geometry.size_upper = 64LL * mdbx::env::geometry::GiB;
#endif
        create_params.geometry.growth_step = 1 * mdbx::env::geometry::MiB;
        create_params.geometry.shrink_threshold = 4 * mdbx::env::geometry::MiB;
        create_params.use_subdirectory = false;

        // Operate parameters
        mdbx::env::operate_parameters operate_params;
        operate_params.max_maps = 9;  // 8 named sub-databases + 1 default
        operate_params.max_readers = 64;
        operate_params.mode = mdbx::env::mode::write_mapped_io;
        operate_params.durability = mdbx::env::durability::robust_synchronous;

        env = mdbx::env_managed(path, create_params, operate_params);

        // Create/open all 7 sub-databases in a single write transaction
        {
            auto txn = env.start_write();
            blobs_map = txn.create_map("blobs");
            seq_map = txn.create_map("sequence");
            expiry_map = txn.create_map("expiry");
            delegation_map = txn.create_map("delegation");
            tombstone_map = txn.create_map("tombstone");
            cursor_map = txn.create_map("cursor");
            quota_map = txn.create_map("quota");

            txn.commit();
        }
    }

    // =========================================================================
    // Encryption helpers
    // =========================================================================

    /// Encrypt encoded blob -> [version:1][nonce:12][ciphertext+tag:N+16]
    std::vector<uint8_t> encrypt_value(
        std::span<const uint8_t> plaintext,
        std::span<const uint8_t> ad) {

        std::array<uint8_t, crypto::AEAD::NONCE_SIZE> nonce;
        randombytes_buf(nonce.data(), nonce.size());

        auto ciphertext = crypto::AEAD::encrypt(
            plaintext, ad, nonce, blob_key_.span());

        std::vector<uint8_t> envelope;
        envelope.reserve(1 + nonce.size() + ciphertext.size());
        envelope.push_back(ENCRYPTION_VERSION);
        envelope.insert(envelope.end(), nonce.begin(), nonce.end());
        envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());
        return envelope;
    }

    /// Decrypt [version:1][nonce:12][ciphertext+tag:N+16] -> encoded blob
    std::vector<uint8_t> decrypt_value(
        std::span<const uint8_t> encrypted,
        std::span<const uint8_t> ad) {

        if (encrypted.size() < ENVELOPE_OVERHEAD) {
            throw std::runtime_error("Encrypted value too short");
        }
        if (encrypted[0] != ENCRYPTION_VERSION) {
            throw std::runtime_error(
                "Unknown encryption version or unencrypted data detected");
        }

        auto nonce = encrypted.subspan(1, crypto::AEAD::NONCE_SIZE);
        auto ciphertext = encrypted.subspan(1 + crypto::AEAD::NONCE_SIZE);

        auto plaintext = crypto::AEAD::decrypt(
            ciphertext, ad, nonce, blob_key_.span());
        if (!plaintext) {
            throw std::runtime_error("AEAD decryption failed (authentication error)");
        }
        return *plaintext;
    }

    /// Validate all existing blob values have the encryption version header.
    /// Refuses to start if unencrypted data is detected.
    void validate_no_unencrypted_data(mdbx::txn_managed& txn) {
        auto cursor = txn.open_cursor(blobs_map);
        auto first = cursor.to_first(false);
        if (!first.done) return;  // Empty database, nothing to validate

        do {
            auto val = cursor.current(false).value;
            if (val.length() == 0 ||
                static_cast<const uint8_t*>(val.data())[0] != ENCRYPTION_VERSION) {
                throw std::runtime_error(
                    "Database contains unencrypted data. "
                    "Delete data_dir and restart.");
            }
            auto next = cursor.to_next(false);
            if (!next.done) break;
        } while (true);
    }

    // =========================================================================
    // Seq helpers
    // =========================================================================

    uint64_t next_seq_num(mdbx::txn_managed& txn, const uint8_t* ns) {
        auto cursor = txn.open_cursor(seq_map);

        // Build key with max seq_num for this namespace
        auto upper_key = make_seq_key(ns, UINT64_MAX);
        auto result = cursor.lower_bound(to_slice(upper_key));

        if (result.done) {
            // Found a key >= upper_key. Could be in our namespace (with UINT64_MAX)
            // or in the next namespace. Check current entry first.
            auto key_data = result.key;
            if (key_data.length() == 40 &&
                std::memcmp(key_data.data(), ns, 32) == 0) {
                // This IS in our namespace (matched with UINT64_MAX seq)
                uint64_t current = chromatindb::util::read_u64_be(
                    static_cast<const uint8_t*>(key_data.data()) + 32);
                return current + 1;
            }
            // Found key is in a different namespace. Try previous entry.
            auto prev = cursor.to_previous(false);
            if (prev.done) {
                auto prev_key = prev.key;
                if (prev_key.length() == 40 &&
                    std::memcmp(prev_key.data(), ns, 32) == 0) {
                    uint64_t current = chromatindb::util::read_u64_be(
                        static_cast<const uint8_t*>(prev_key.data()) + 32);
                    return current + 1;
                }
            }
        } else {
            // lower_bound went past end of database.
            // Last entry might be in our namespace.
            auto last = cursor.to_last(false);
            if (last.done) {
                auto key_data = last.key;
                if (key_data.length() == 40 &&
                    std::memcmp(key_data.data(), ns, 32) == 0) {
                    uint64_t current = chromatindb::util::read_u64_be(
                        static_cast<const uint8_t*>(key_data.data()) + 32);
                    return current + 1;
                }
            }
        }

        return 1;  // First blob in this namespace
    }
};

// =============================================================================
// Public API
// =============================================================================

uint64_t system_clock_seconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
}

Storage::Storage(const std::string& data_dir, Clock clock)
    : impl_(std::make_unique<Impl>(
          data_dir,
          crypto::load_or_generate_master_key(data_dir),
          std::move(clock))) {
    rebuild_quota_aggregates();
}

Storage::~Storage() = default;
Storage::Storage(Storage&& other) noexcept = default;
Storage& Storage::operator=(Storage&& other) noexcept = default;

StoreResult Storage::store_blob(const wire::BlobData& blob) {
    auto encoded = wire::encode_blob(blob);
    auto hash = wire::blob_hash(encoded);
    return store_blob(blob, hash, encoded);
}

StoreResult Storage::store_blob(const wire::BlobData& blob,
                                const std::array<uint8_t, 32>& precomputed_hash,
                                std::span<const uint8_t> precomputed_encoded) {
    return store_blob(blob, precomputed_hash, precomputed_encoded, 0, 0, 0);
}

StoreResult Storage::store_blob(const wire::BlobData& blob,
                                const std::array<uint8_t, 32>& precomputed_hash,
                                std::span<const uint8_t> precomputed_encoded,
                                uint64_t max_storage_bytes,
                                uint64_t quota_byte_limit,
                                uint64_t quota_count_limit) {
    try {
        auto blob_key = make_blob_key(blob.namespace_id.data(), precomputed_hash.data());
        auto key_slice = to_slice(blob_key);

        auto txn = impl_->env.start_write();

        // Check dedup BEFORE encryption and capacity checks (hash is on plaintext)
        // Duplicates must bypass capacity/quota checks (Pitfall 4)
        auto existing = txn.get(impl_->blobs_map, key_slice, not_found_sentinel);
        if (existing.data() != nullptr) {
            // Blob already exists -- find existing seq_num by scanning
            uint64_t existing_seq = 0;
            auto cursor = txn.open_cursor(impl_->seq_map);

            // Scan from first entry for this namespace
            auto lower = make_seq_key(blob.namespace_id.data(), 1);
            auto seek = cursor.lower_bound(to_slice(lower));
            while (seek.done) {
                auto k = cursor.current(false).key;
                if (k.length() != 40 ||
                    std::memcmp(k.data(), blob.namespace_id.data(), 32) != 0) {
                    break;
                }
                auto v = cursor.current(false).value;
                if (v.length() == 36 &&
                    std::memcmp(v.data(), precomputed_hash.data(), 32) == 0) {
                    existing_seq = chromatindb::util::read_u64_be(
                        static_cast<const uint8_t*>(k.data()) + 32);
                    break;
                }
                seek = cursor.to_next(false);
            }

            txn.abort();
            StoreResult result;
            result.status = StoreResult::Status::Duplicate;
            result.seq_num = existing_seq;
            result.blob_hash = precomputed_hash;
            return result;
        }

        // RES-03 (D-10): Atomic capacity check inside write transaction
        // Tombstones exempt (small, free space by deleting target)
        if (max_storage_bytes > 0 && !wire::is_tombstone(blob.data)) {
            if (used_bytes() >= max_storage_bytes) {
                txn.abort();
                StoreResult result;
                result.status = StoreResult::Status::CapacityExceeded;
                result.blob_hash = precomputed_hash;
                return result;
            }
        }

        // RES-03 (D-11): Atomic quota check inside write transaction
        if (!wire::is_tombstone(blob.data) && (quota_byte_limit > 0 || quota_count_limit > 0)) {
            auto ns_slice = mdbx::slice(blob.namespace_id.data(), 32);
            auto existing_quota = txn.get(impl_->quota_map, ns_slice, not_found_sentinel);
            NamespaceQuota current{};
            if (existing_quota.data() != nullptr &&
                existing_quota.length() == QUOTA_VALUE_SIZE) {
                current = decode_quota_value(
                    static_cast<const uint8_t*>(existing_quota.data()));
            }
            if (quota_byte_limit > 0 && current.total_bytes + precomputed_encoded.size() > quota_byte_limit) {
                txn.abort();
                StoreResult result;
                result.status = StoreResult::Status::QuotaExceeded;
                result.blob_hash = precomputed_hash;
                return result;
            }
            if (quota_count_limit > 0 && current.blob_count + 1 > quota_count_limit) {
                txn.abort();
                StoreResult result;
                result.status = StoreResult::Status::QuotaExceeded;
                result.blob_hash = precomputed_hash;
                return result;
            }
        }

        // Encrypt the pre-computed encoded blob value (AD = 64-byte mdbx key)
        auto encrypted = impl_->encrypt_value(
            precomputed_encoded,
            std::span<const uint8_t>(blob_key.data(), blob_key.size()));

        // Store encrypted blob
        txn.upsert(impl_->blobs_map, key_slice,
                    mdbx::slice(encrypted.data(), encrypted.size()));

        // Assign seq_num
        uint64_t seq = impl_->next_seq_num(txn, blob.namespace_id.data());
        auto seq_key = make_seq_key(blob.namespace_id.data(), seq);

        // Extract 4-byte type prefix from blob data
        auto blob_type = wire::extract_blob_type(std::span<const uint8_t>(blob.data));

        // Store in seq_map: value is [hash:32][type:4] = 36 bytes
        std::array<uint8_t, 36> seq_value;
        std::memcpy(seq_value.data(), precomputed_hash.data(), 32);
        std::memcpy(seq_value.data() + 32, blob_type.data(), 4);
        txn.upsert(impl_->seq_map, to_slice(seq_key),
                    mdbx::slice(seq_value.data(), seq_value.size()));

        // Store expiry entry (skip for TTL=0)
        // Both timestamp and TTL are in seconds.
        if (blob.ttl > 0) {
            uint64_t expiry_time = wire::saturating_expiry(blob.timestamp, blob.ttl);
            auto exp_key = make_expiry_key(expiry_time, precomputed_hash.data());
            txn.upsert(impl_->expiry_map, to_slice(exp_key),
                        mdbx::slice(blob.namespace_id.data(),
                                    blob.namespace_id.size()));
        }

        // Populate delegation index if this is a delegation blob
        if (wire::is_delegation(blob.data)) {
            auto delegate_pubkey = wire::extract_delegate_pubkey(blob.data);
            auto delegate_pk_hash = crypto::sha3_256(delegate_pubkey);
            // Key: [namespace:32][delegate_pk_hash:32]
            auto deleg_key = make_blob_key(blob.namespace_id.data(), delegate_pk_hash.data());
            // Value: [delegation_blob_hash:32]
            txn.upsert(impl_->delegation_map, to_slice(deleg_key),
                        mdbx::slice(precomputed_hash.data(), precomputed_hash.size()));
        }

        // Populate tombstone index if this is a tombstone blob
        if (wire::is_tombstone(blob.data)) {
            auto target_hash = wire::extract_tombstone_target(
                std::span<const uint8_t>(blob.data));
            auto ts_key = make_blob_key(blob.namespace_id.data(), target_hash.data());
            txn.upsert(impl_->tombstone_map, to_slice(ts_key), mdbx::slice());
        }

        // Increment namespace quota aggregate (tombstones exempt)
        if (!wire::is_tombstone(blob.data)) {
            auto ns_slice = mdbx::slice(blob.namespace_id.data(), 32);
            auto existing_quota = txn.get(impl_->quota_map, ns_slice, not_found_sentinel);
            NamespaceQuota current{};
            if (existing_quota.data() != nullptr &&
                existing_quota.length() == QUOTA_VALUE_SIZE) {
                current = decode_quota_value(
                    static_cast<const uint8_t*>(existing_quota.data()));
            }
            current.total_bytes += encrypted.size();
            current.blob_count += 1;
            auto new_val = encode_quota_value(current);
            txn.upsert(impl_->quota_map, ns_slice,
                        mdbx::slice(new_val.data(), new_val.size()));
        }

        txn.commit();
        spdlog::debug("Stored blob in namespace (seq={})", seq);

        StoreResult result;
        result.status = StoreResult::Status::Stored;
        result.seq_num = seq;
        result.blob_hash = precomputed_hash;
        return result;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in store_blob: {}", e.what());
        StoreResult result;
        result.status = StoreResult::Status::Error;
        return result;
    }
}

std::vector<StoreResult> Storage::store_blobs_atomic(
    const std::vector<PrecomputedBlob>& blobs,
    uint64_t max_storage_bytes,
    uint64_t quota_byte_limit,
    uint64_t quota_count_limit) {
    std::vector<StoreResult> results(blobs.size());
    if (blobs.empty()) return results;

    try {
        auto txn = impl_->env.start_write();

        // First pass: check dedup for each blob
        std::vector<bool> is_dup(blobs.size(), false);
        uint64_t new_blob_count = 0;

        for (size_t i = 0; i < blobs.size(); ++i) {
            const auto& pb = blobs[i];
            auto blob_key = make_blob_key(
                pb.blob.namespace_id.data(), pb.content_hash.data());
            auto existing = txn.get(impl_->blobs_map, to_slice(blob_key),
                                     not_found_sentinel);
            if (existing.data() != nullptr) {
                is_dup[i] = true;

                // Find existing seq_num
                uint64_t existing_seq = 0;
                auto cursor = txn.open_cursor(impl_->seq_map);
                auto lower = make_seq_key(pb.blob.namespace_id.data(), 1);
                auto seek = cursor.lower_bound(to_slice(lower));
                while (seek.done) {
                    auto k = cursor.current(false).key;
                    if (k.length() != 40 ||
                        std::memcmp(k.data(),
                                    pb.blob.namespace_id.data(), 32) != 0) {
                        break;
                    }
                    auto v = cursor.current(false).value;
                    if (v.length() == 36 &&
                        std::memcmp(v.data(),
                                    pb.content_hash.data(), 32) == 0) {
                        existing_seq = chromatindb::util::read_u64_be(
                            static_cast<const uint8_t*>(k.data()) + 32);
                        break;
                    }
                    seek = cursor.to_next(false);
                }

                results[i].status = StoreResult::Status::Duplicate;
                results[i].seq_num = existing_seq;
                results[i].blob_hash = pb.content_hash;
            } else {
                ++new_blob_count;
            }
        }

        // If all duplicates, commit (no-op) and return
        if (new_blob_count == 0) {
            txn.commit();
            return results;
        }

        // Capacity check: if max_storage_bytes > 0, check used_bytes
        // (tombstones exempt from capacity check)
        if (max_storage_bytes > 0) {
            bool has_non_tombstone = false;
            for (size_t i = 0; i < blobs.size(); ++i) {
                if (!is_dup[i] && !wire::is_tombstone(blobs[i].blob.data)) {
                    has_non_tombstone = true;
                    break;
                }
            }
            if (has_non_tombstone && used_bytes() >= max_storage_bytes) {
                txn.abort();
                for (auto& r : results) {
                    r.status = StoreResult::Status::CapacityExceeded;
                }
                return results;
            }
        }

        // Quota check: sum encrypted sizes of all non-dup non-tombstone blobs
        // per namespace, check against limits
        if (quota_byte_limit > 0 || quota_count_limit > 0) {
            // Accumulate per-namespace new byte and count totals
            std::map<std::array<uint8_t, 32>, std::pair<uint64_t, uint64_t>> ns_additions;
            for (size_t i = 0; i < blobs.size(); ++i) {
                if (is_dup[i]) continue;
                if (wire::is_tombstone(blobs[i].blob.data)) continue;
                auto& [bytes, count] = ns_additions[blobs[i].blob.namespace_id];
                bytes += blobs[i].encoded.size();
                count += 1;
            }

            for (const auto& [ns_id, additions] : ns_additions) {
                auto ns_slice = mdbx::slice(ns_id.data(), 32);
                auto existing_quota = txn.get(impl_->quota_map, ns_slice,
                                               not_found_sentinel);
                NamespaceQuota current{};
                if (existing_quota.data() != nullptr &&
                    existing_quota.length() == QUOTA_VALUE_SIZE) {
                    current = decode_quota_value(
                        static_cast<const uint8_t*>(existing_quota.data()));
                }
                if (quota_byte_limit > 0 &&
                    current.total_bytes + additions.first > quota_byte_limit) {
                    txn.abort();
                    for (auto& r : results) {
                        r.status = StoreResult::Status::QuotaExceeded;
                    }
                    return results;
                }
                if (quota_count_limit > 0 &&
                    current.blob_count + additions.second > quota_count_limit) {
                    txn.abort();
                    for (auto& r : results) {
                        r.status = StoreResult::Status::QuotaExceeded;
                    }
                    return results;
                }
            }
        }

        // Second pass: store each non-duplicate blob
        // Track per-namespace quota increments for batch update
        std::map<std::array<uint8_t, 32>, std::pair<uint64_t, uint64_t>> quota_increments;

        for (size_t i = 0; i < blobs.size(); ++i) {
            if (is_dup[i]) continue;

            const auto& pb = blobs[i];
            auto blob_key = make_blob_key(
                pb.blob.namespace_id.data(), pb.content_hash.data());

            // Encrypt the encoded blob (AD = 64-byte mdbx key)
            auto encrypted = impl_->encrypt_value(
                pb.encoded,
                std::span<const uint8_t>(blob_key.data(), blob_key.size()));

            // Store encrypted blob
            txn.upsert(impl_->blobs_map, to_slice(blob_key),
                        mdbx::slice(encrypted.data(), encrypted.size()));

            // Assign seq_num
            uint64_t seq = impl_->next_seq_num(txn, pb.blob.namespace_id.data());
            auto seq_key = make_seq_key(pb.blob.namespace_id.data(), seq);

            // Extract 4-byte type prefix from blob data
            auto blob_type = wire::extract_blob_type(std::span<const uint8_t>(pb.blob.data));

            // Store in seq_map: value is [hash:32][type:4] = 36 bytes
            std::array<uint8_t, 36> seq_value;
            std::memcpy(seq_value.data(), pb.content_hash.data(), 32);
            std::memcpy(seq_value.data() + 32, blob_type.data(), 4);
            txn.upsert(impl_->seq_map, to_slice(seq_key),
                        mdbx::slice(seq_value.data(), seq_value.size()));

            // Store expiry entry if TTL > 0
            if (pb.blob.ttl > 0) {
                uint64_t expiry_time = wire::saturating_expiry(
                    pb.blob.timestamp, pb.blob.ttl);
                auto exp_key = make_expiry_key(expiry_time,
                                                pb.content_hash.data());
                txn.upsert(impl_->expiry_map, to_slice(exp_key),
                            mdbx::slice(pb.blob.namespace_id.data(),
                                        pb.blob.namespace_id.size()));
            }

            // Populate delegation index if applicable
            if (wire::is_delegation(pb.blob.data)) {
                auto delegate_pubkey = wire::extract_delegate_pubkey(pb.blob.data);
                auto delegate_pk_hash = crypto::sha3_256(delegate_pubkey);
                auto deleg_key = make_blob_key(
                    pb.blob.namespace_id.data(), delegate_pk_hash.data());
                txn.upsert(impl_->delegation_map, to_slice(deleg_key),
                            mdbx::slice(pb.content_hash.data(),
                                        pb.content_hash.size()));
            }

            // Populate tombstone index if applicable
            if (wire::is_tombstone(pb.blob.data)) {
                auto target_hash = wire::extract_tombstone_target(
                    std::span<const uint8_t>(pb.blob.data));
                auto ts_key = make_blob_key(
                    pb.blob.namespace_id.data(), target_hash.data());
                txn.upsert(impl_->tombstone_map, to_slice(ts_key),
                            mdbx::slice());
            }

            // Accumulate quota increment (tombstones exempt)
            if (!wire::is_tombstone(pb.blob.data)) {
                auto& [bytes, count] = quota_increments[pb.blob.namespace_id];
                bytes += encrypted.size();
                count += 1;
            }

            results[i].status = StoreResult::Status::Stored;
            results[i].seq_num = seq;
            results[i].blob_hash = pb.content_hash;
        }

        // Update quota aggregates once per namespace
        for (const auto& [ns_id, increments] : quota_increments) {
            auto ns_slice = mdbx::slice(ns_id.data(), 32);
            auto existing_quota = txn.get(impl_->quota_map, ns_slice,
                                           not_found_sentinel);
            NamespaceQuota current{};
            if (existing_quota.data() != nullptr &&
                existing_quota.length() == QUOTA_VALUE_SIZE) {
                current = decode_quota_value(
                    static_cast<const uint8_t*>(existing_quota.data()));
            }
            current.total_bytes += increments.first;
            current.blob_count += increments.second;
            auto new_val = encode_quota_value(current);
            txn.upsert(impl_->quota_map, ns_slice,
                        mdbx::slice(new_val.data(), new_val.size()));
        }

        txn.commit();
        spdlog::debug("Stored {} blobs atomically ({} new, {} dups)",
                       blobs.size(), new_blob_count,
                       blobs.size() - new_blob_count);
        return results;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in store_blobs_atomic: {}", e.what());
        for (auto& r : results) {
            r.status = StoreResult::Status::Error;
        }
        return results;
    }
}

std::optional<wire::BlobData> Storage::get_blob(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> hash) {
    try {
        auto blob_key = make_blob_key(ns.data(), hash.data());
        auto txn = impl_->env.start_read();

        auto val = txn.get(impl_->blobs_map, to_slice(blob_key), not_found_sentinel);
        if (val.data() == nullptr) {
            return std::nullopt;
        }

        // Decrypt the value (AD = 64-byte mdbx key)
        auto decrypted = impl_->decrypt_value(
            std::span<const uint8_t>(
                static_cast<const uint8_t*>(val.data()), val.length()),
            std::span<const uint8_t>(blob_key.data(), blob_key.size()));

        auto decoded = wire::decode_blob(
            std::span<const uint8_t>(decrypted.data(), decrypted.size()));
        return decoded;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in get_blob: {}", e.what());
        return std::nullopt;
    }
}

bool Storage::has_blob(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> hash) {
    try {
        auto blob_key = make_blob_key(ns.data(), hash.data());
        auto txn = impl_->env.start_read();

        auto val = txn.get(impl_->blobs_map, to_slice(blob_key), not_found_sentinel);
        return val.data() != nullptr;

    } catch (const std::exception&) {
        return false;
    }
}

std::vector<wire::BlobData> Storage::get_blobs_by_seq(
    std::span<const uint8_t, 32> ns,
    uint64_t since_seq) {
    std::vector<wire::BlobData> results;

    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->seq_map);

        auto lower_key = make_seq_key(ns.data(), since_seq + 1);
        auto seek_result = cursor.lower_bound(to_slice(lower_key));

        if (!seek_result.done) {
            return results;
        }

        do {
            auto key_data = cursor.current(false).key;
            if (key_data.length() != 40) break;

            // Check namespace prefix
            if (std::memcmp(key_data.data(), ns.data(), 32) != 0) {
                break;
            }

            // Extract hash from 36-byte value (hash:32 + type:4)
            auto val_data = cursor.current(false).value;
            if (val_data.length() != 36) break;

            // Fetch full blob
            auto blob_key = make_blob_key(
                ns.data(),
                static_cast<const uint8_t*>(val_data.data()));

            auto blob_val = txn.get(impl_->blobs_map, to_slice(blob_key),
                                     not_found_sentinel);
            if (blob_val.data() != nullptr) {
                // Decrypt the value (AD = 64-byte mdbx key)
                auto decrypted = impl_->decrypt_value(
                    std::span<const uint8_t>(
                        static_cast<const uint8_t*>(blob_val.data()),
                        blob_val.length()),
                    std::span<const uint8_t>(blob_key.data(), blob_key.size()));
                auto blob = wire::decode_blob(
                    std::span<const uint8_t>(decrypted.data(), decrypted.size()));
                results.push_back(std::move(blob));
            }
            // else: blob deleted by expiry, skip (gap in seq)

            auto next = cursor.to_next(false);
            if (!next.done) break;

        } while (true);

    } catch (const std::exception& e) {
        spdlog::error("Storage error in get_blobs_by_seq: {}", e.what());
    }

    return results;
}

std::vector<BlobRef> Storage::get_blob_refs_since(
    std::span<const uint8_t, 32> ns,
    uint64_t since_seq,
    uint32_t max_count) {
    std::vector<BlobRef> results;

    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->seq_map);

        auto lower_key = make_seq_key(ns.data(), since_seq + 1);
        auto seek_result = cursor.lower_bound(to_slice(lower_key));

        if (!seek_result.done) {
            return results;
        }

        static constexpr std::array<uint8_t, 32> zero_hash{};

        do {
            auto key_data = cursor.current(false).key;
            if (key_data.length() != 40) break;

            // Check namespace prefix
            if (std::memcmp(key_data.data(), ns.data(), 32) != 0) {
                break;
            }

            // Read 36-byte value (hash:32 + type:4)
            auto val_data = cursor.current(false).value;
            if (val_data.length() != 36) break;

            // Skip zero-hash sentinels (deleted blobs)
            if (std::memcmp(val_data.data(), zero_hash.data(), 32) != 0) {
                BlobRef ref;
                std::memcpy(ref.blob_hash.data(), val_data.data(), 32);
                ref.seq_num = chromatindb::util::read_u64_be(
                    static_cast<const uint8_t*>(key_data.data()) + 32);
                std::memcpy(ref.blob_type.data(),
                            static_cast<const uint8_t*>(val_data.data()) + 32, 4);
                results.push_back(ref);

                if (max_count > 0 && results.size() >= max_count) {
                    break;
                }
            }

            auto next = cursor.to_next(false);
            if (!next.done) break;

        } while (true);

    } catch (const std::exception& e) {
        spdlog::error("Storage error in get_blob_refs_since: {}", e.what());
    }

    return results;
}

std::vector<std::array<uint8_t, 32>> Storage::get_hashes_by_namespace(
    std::span<const uint8_t, 32> ns) {
    std::vector<std::array<uint8_t, 32>> hashes;

    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->seq_map);

        // Seek to first entry for this namespace (seq_num = 1)
        auto lower_key = make_seq_key(ns.data(), 1);
        auto seek_result = cursor.lower_bound(to_slice(lower_key));
        if (!seek_result.done) return hashes;

        do {
            auto key_data = cursor.current(false).key;
            if (key_data.length() != 40) break;

            // Check namespace prefix
            if (std::memcmp(key_data.data(), ns.data(), 32) != 0) break;

            // Read hash from 36-byte value; skip zero-hash sentinels left
            // by delete_blob_data to preserve seq_num monotonicity
            auto val_data = cursor.current(false).value;
            if (val_data.length() == 36) {
                static constexpr std::array<uint8_t, 32> zero_hash{};
                if (std::memcmp(val_data.data(), zero_hash.data(), 32) != 0) {
                    std::array<uint8_t, 32> hash;
                    std::memcpy(hash.data(), val_data.data(), 32);
                    hashes.push_back(hash);
                }
            }

            auto next = cursor.to_next(false);
            if (!next.done) break;
        } while (true);

    } catch (const std::exception& e) {
        spdlog::error("Storage error in get_hashes_by_namespace: {}", e.what());
    }

    return hashes;
}

std::vector<NamespaceInfo> Storage::list_namespaces() {
    std::vector<NamespaceInfo> result;

    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->seq_map);

        auto first = cursor.to_first(false);
        if (!first.done) {
            return result;  // Empty database
        }

        while (true) {
            auto key_data = cursor.current(false).key;
            if (key_data.length() != 40) break;

            // Extract namespace from key (first 32 bytes)
            NamespaceInfo info;
            std::memcpy(info.namespace_id.data(), key_data.data(), 32);

            // Find the latest seq_num for this namespace:
            // Seek to [namespace][0xFF..FF] -- the upper bound
            auto upper_key = make_seq_key(info.namespace_id.data(), UINT64_MAX);
            auto upper_result = cursor.lower_bound(to_slice(upper_key));

            if (upper_result.done) {
                // Check if this key is still in our namespace
                auto k = cursor.current(false).key;
                if (k.length() == 40 &&
                    std::memcmp(k.data(), info.namespace_id.data(), 32) == 0) {
                    info.latest_seq_num = chromatindb::util::read_u64_be(
                        static_cast<const uint8_t*>(k.data()) + 32);
                } else {
                    // It's in the next namespace; go back one
                    auto prev = cursor.to_previous(false);
                    if (prev.done) {
                        auto pk = prev.key;
                        if (pk.length() == 40 &&
                            std::memcmp(pk.data(), info.namespace_id.data(), 32) == 0) {
                            info.latest_seq_num = chromatindb::util::read_u64_be(
                                static_cast<const uint8_t*>(pk.data()) + 32);
                        }
                    }
                }
            } else {
                // Past end of database -- last entry must be ours
                auto last = cursor.to_last(false);
                if (last.done) {
                    auto lk = last.key;
                    if (lk.length() == 40 &&
                        std::memcmp(lk.data(), info.namespace_id.data(), 32) == 0) {
                        info.latest_seq_num = chromatindb::util::read_u64_be(
                            static_cast<const uint8_t*>(lk.data()) + 32);
                    }
                }
            }

            result.push_back(info);

            // Jump to next namespace: increment last byte of namespace_id
            // Find next namespace by seeking to [namespace+1][0x00..00]
            std::array<uint8_t, 32> next_ns = info.namespace_id;
            bool carry = true;
            for (int i = 31; i >= 0 && carry; --i) {
                if (next_ns[i] < 0xFF) {
                    next_ns[i]++;
                    carry = false;
                } else {
                    next_ns[i] = 0;
                }
            }
            if (carry) {
                break;  // All 0xFF namespace -- no more possible
            }

            auto next_key = make_seq_key(next_ns.data(), 0);
            auto next_result = cursor.lower_bound(to_slice(next_key));
            if (!next_result.done) {
                break;  // No more entries
            }
        }

    } catch (const std::exception& e) {
        spdlog::error("Storage error in list_namespaces: {}", e.what());
    }

    return result;
}

bool Storage::delete_blob_data(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> blob_hash) {
    try {
        auto txn = impl_->env.start_write();
        auto blob_key = make_blob_key(ns.data(), blob_hash.data());
        auto key_slice = to_slice(blob_key);

        // Check if blob exists
        auto existing = txn.get(impl_->blobs_map, key_slice, not_found_sentinel);
        if (existing.data() == nullptr) {
            txn.abort();
            return false;
        }

        // Capture encrypted size for quota decrement (before erase)
        auto encrypted_size = existing.length();

        // Decrypt and decode to get TTL + timestamp for expiry cleanup
        auto decrypted = impl_->decrypt_value(
            std::span<const uint8_t>(
                static_cast<const uint8_t*>(existing.data()), existing.length()),
            std::span<const uint8_t>(blob_key.data(), blob_key.size()));
        auto blob = wire::decode_blob(
            std::span<const uint8_t>(decrypted.data(), decrypted.size()));

        // Delete from blobs_map
        txn.erase(impl_->blobs_map, key_slice);

        // Decrement namespace quota aggregate
        {
            auto ns_slice = mdbx::slice(ns.data(), 32);
            auto existing_quota = txn.get(impl_->quota_map, ns_slice, not_found_sentinel);
            if (existing_quota.data() != nullptr &&
                existing_quota.length() == QUOTA_VALUE_SIZE) {
                auto current = decode_quota_value(
                    static_cast<const uint8_t*>(existing_quota.data()));
                current.total_bytes = (current.total_bytes > encrypted_size)
                    ? current.total_bytes - encrypted_size : 0;
                current.blob_count = (current.blob_count > 0)
                    ? current.blob_count - 1 : 0;
                if (current.total_bytes == 0 && current.blob_count == 0) {
                    txn.erase(impl_->quota_map, ns_slice);
                } else {
                    auto new_val = encode_quota_value(current);
                    txn.upsert(impl_->quota_map, ns_slice,
                                mdbx::slice(new_val.data(), new_val.size()));
                }
            }
        }

        // Tombstone seq_map entry: replace hash with zero sentinel to preserve
        // seq_num monotonicity.  Erasing the entry would let next_seq_num()
        // recycle the same number, which breaks cursor-based change detection
        // (cursor compares latest_seq_num to detect new data).
        {
            static constexpr std::array<uint8_t, 32> zero_hash{};
            auto cursor = txn.open_cursor(impl_->seq_map);
            auto lower = make_seq_key(ns.data(), 1);
            auto seek = cursor.lower_bound(to_slice(lower));
            while (seek.done) {
                auto k = cursor.current(false).key;
                if (k.length() != 40 ||
                    std::memcmp(k.data(), ns.data(), 32) != 0) {
                    break;
                }
                auto v = cursor.current(false).value;
                if (v.length() == 36 &&
                    std::memcmp(v.data(), blob_hash.data(), 32) == 0) {
                    // Replace with zero sentinel (36 bytes to preserve value size)
                    std::array<uint8_t, 36> zero_sentinel{};
                    txn.upsert(impl_->seq_map,
                               mdbx::slice(k.data(), k.length()),
                               mdbx::slice(zero_sentinel.data(), zero_sentinel.size()));
                    break;
                }
                seek = cursor.to_next(false);
            }
        }

        // Delete from expiry_map if TTL > 0
        // Both timestamp and TTL are in seconds.
        if (blob.ttl > 0) {
            uint64_t expiry_time = wire::saturating_expiry(blob.timestamp, blob.ttl);
            auto exp_key = make_expiry_key(expiry_time, blob_hash.data());
            try {
                txn.erase(impl_->expiry_map, to_slice(exp_key));
            } catch (const mdbx::exception&) {
                // Already deleted -- not an error
            }
        }

        // Clean delegation index if this was a delegation blob
        if (wire::is_delegation(blob.data)) {
            auto delegate_pubkey = wire::extract_delegate_pubkey(blob.data);
            auto delegate_pk_hash = crypto::sha3_256(delegate_pubkey);
            auto deleg_key = make_blob_key(ns.data(), delegate_pk_hash.data());
            try {
                txn.erase(impl_->delegation_map, to_slice(deleg_key));
            } catch (const mdbx::exception&) {
                // Already deleted -- not an error
            }
        }

        // Clean tombstone index if this was a tombstone blob
        if (wire::is_tombstone(blob.data)) {
            auto target_hash = wire::extract_tombstone_target(
                std::span<const uint8_t>(blob.data));
            auto ts_key = make_blob_key(ns.data(), target_hash.data());
            try {
                txn.erase(impl_->tombstone_map, to_slice(ts_key));
            } catch (const mdbx::exception&) {
                // Already deleted -- not an error
            }
        }

        txn.commit();
        spdlog::debug("Deleted blob from storage (ns {:02x}{:02x}...)",
                       ns[0], ns[1]);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in delete_blob_data: {}", e.what());
        return false;
    }
}

bool Storage::has_valid_delegation(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t> delegate_pubkey) {
    try {
        auto delegate_pk_hash = crypto::sha3_256(delegate_pubkey);
        auto deleg_key = make_blob_key(namespace_id.data(), delegate_pk_hash.data());
        auto txn = impl_->env.start_read();

        auto val = txn.get(impl_->delegation_map, to_slice(deleg_key), not_found_sentinel);
        return val.data() != nullptr;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in has_valid_delegation: {}", e.what());
        return false;
    }
}

bool Storage::has_tombstone_for(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target_blob_hash) {
    try {
        auto ts_key = make_blob_key(ns.data(), target_blob_hash.data());
        auto txn = impl_->env.start_read();
        auto val = txn.get(impl_->tombstone_map, to_slice(ts_key), not_found_sentinel);
        return val.data() != nullptr;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in has_tombstone_for: {}", e.what());
        return false;
    }
}

uint64_t Storage::count_tombstones() const {
    try {
        auto txn = impl_->env.start_read();
        return txn.get_map_stat(impl_->tombstone_map).ms_entries;
    } catch (const std::exception& e) {
        spdlog::error("Storage error in count_tombstones: {}", e.what());
        return 0;
    }
}

uint64_t Storage::count_delegations(std::span<const uint8_t, 32> namespace_id) const {
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->delegation_map);

        // Seek to lower bound: namespace prefix with zero hash suffix
        std::array<uint8_t, 64> lower_key{};
        std::memcpy(lower_key.data(), namespace_id.data(), 32);
        // remaining 32 bytes are zero -- sorts before any real delegate_pk_hash

        uint64_t count = 0;
        auto result = cursor.lower_bound(to_slice(lower_key));
        while (result.done) {
            auto key = cursor.current(false).key;
            // Check that the first 32 bytes still match our namespace
            if (key.length() < 32 ||
                std::memcmp(key.data(), namespace_id.data(), 32) != 0) {
                break;  // Past our namespace prefix
            }
            ++count;
            result = cursor.to_next(false);
        }
        return count;
    } catch (const mdbx::exception& e) {
        if (e.error().code() == MDBX_NOTFOUND) return 0;
        spdlog::error("Storage error in count_delegations: {}", e.what());
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Storage error in count_delegations: {}", e.what());
        return 0;
    }
}

std::vector<DelegationEntry> Storage::list_delegations(std::span<const uint8_t, 32> namespace_id) const {
    std::vector<DelegationEntry> entries;
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->delegation_map);

        // Seek to lower bound: namespace prefix with zero hash suffix
        std::array<uint8_t, 64> lower_key{};
        std::memcpy(lower_key.data(), namespace_id.data(), 32);

        auto result = cursor.lower_bound(to_slice(lower_key));
        while (result.done) {
            auto kv = cursor.current(true);
            auto key = kv.key;
            // Check namespace prefix match (key = [namespace:32][delegate_pk_hash:32])
            if (key.length() < 64 ||
                std::memcmp(key.data(), namespace_id.data(), 32) != 0) {
                break;
            }
            DelegationEntry entry;
            std::memcpy(entry.delegate_pk_hash.data(),
                        static_cast<const uint8_t*>(key.data()) + 32, 32);
            if (kv.value.length() >= 32) {
                std::memcpy(entry.delegation_blob_hash.data(),
                            kv.value.data(), 32);
            }
            entries.push_back(entry);
            result = cursor.to_next(false);
        }
    } catch (const mdbx::exception& e) {
        if (e.error().code() != MDBX_NOTFOUND)
            spdlog::error("Storage error in list_delegations: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::error("Storage error in list_delegations: {}", e.what());
    }
    return entries;
}

size_t Storage::run_expiry_scan() {
    size_t purged = 0;

    try {
        uint64_t now = impl_->clock();
        auto txn = impl_->env.start_write();
        auto cursor = txn.open_cursor(impl_->expiry_map);

        auto first = cursor.to_first(false);
        if (!first.done) {
            txn.commit();
            return 0;
        }

        bool more = true;
        while (more) {
            auto current = cursor.current(false);
            auto key_data = current.key;
            if (key_data.length() != 40) break;

            uint64_t expiry_ts = chromatindb::util::read_u64_be(
                static_cast<const uint8_t*>(key_data.data()));

            if (expiry_ts > now) break;

            const uint8_t* hash_ptr =
                static_cast<const uint8_t*>(key_data.data()) + 8;
            auto ns_data = current.value;
            const uint8_t* ns_ptr =
                static_cast<const uint8_t*>(ns_data.data());

            // Read blob before deleting to check if it's a tombstone
            auto blob_key = make_blob_key(ns_ptr, hash_ptr);
            auto raw = txn.get(impl_->blobs_map, to_slice(blob_key),
                               not_found_sentinel);
            if (raw.data() != nullptr) {
                auto encrypted_size = raw.length();
                // Decrypt the value (AD = 64-byte mdbx key)
                auto decrypted = impl_->decrypt_value(
                    std::span<const uint8_t>(
                        static_cast<const uint8_t*>(raw.data()), raw.length()),
                    std::span<const uint8_t>(blob_key.data(), blob_key.size()));
                auto decoded = wire::decode_blob(
                    std::span<const uint8_t>(decrypted.data(), decrypted.size()));
                bool is_tombstone = wire::is_tombstone(decoded.data);
                if (is_tombstone) {
                    auto target_hash = wire::extract_tombstone_target(
                        std::span<const uint8_t>(decoded.data));
                    auto ts_key = make_blob_key(ns_ptr, target_hash.data());
                    try {
                        txn.erase(impl_->tombstone_map, to_slice(ts_key));
                    } catch (const mdbx::exception&) {
                        // Already cleaned -- not an error
                    }
                }
                // Delete from blobs index
                try {
                    txn.erase(impl_->blobs_map, to_slice(blob_key));
                } catch (const mdbx::exception&) {
                    // Already deleted -- not an error
                }
                // Decrement namespace quota aggregate (tombstones were never counted)
                if (!is_tombstone) {
                    auto ns_qslice = mdbx::slice(ns_ptr, 32);
                    auto existing_quota = txn.get(impl_->quota_map, ns_qslice,
                                                  not_found_sentinel);
                    if (existing_quota.data() != nullptr &&
                        existing_quota.length() == QUOTA_VALUE_SIZE) {
                        auto current = decode_quota_value(
                            static_cast<const uint8_t*>(existing_quota.data()));
                        current.total_bytes = (current.total_bytes > encrypted_size)
                            ? current.total_bytes - encrypted_size : 0;
                        current.blob_count = (current.blob_count > 0)
                            ? current.blob_count - 1 : 0;
                        if (current.total_bytes == 0 && current.blob_count == 0) {
                            txn.erase(impl_->quota_map, ns_qslice);
                        } else {
                            auto new_val = encode_quota_value(current);
                            txn.upsert(impl_->quota_map, ns_qslice,
                                        mdbx::slice(new_val.data(), new_val.size()));
                        }
                    }
                }
            }

            // Advance cursor BEFORE erasing (erase invalidates position)
            auto next = cursor.to_next(false);
            more = next.done;

            // Now go back and erase the entry we just read
            // Actually cursor.erase() erases current entry. But after to_next,
            // the cursor is on the NEXT entry. We need to erase the PREVIOUS one.
            // Better approach: use txn.erase with the key directly.
            txn.erase(impl_->expiry_map, key_data);

            purged++;
        }

        txn.commit();

        if (purged > 0) {
            spdlog::info("Expiry scan: purged {} blobs", purged);
        }

    } catch (const std::exception& e) {
        spdlog::error("Storage error in run_expiry_scan: {}", e.what());
    }

    return purged;
}

std::optional<uint64_t> Storage::get_earliest_expiry() const {
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->expiry_map);
        auto first = cursor.to_first(false);
        if (!first.done) return std::nullopt;

        auto key_data = cursor.current(false).key;
        if (key_data.length() < 8) return std::nullopt;

        return chromatindb::util::read_u64_be(
            static_cast<const uint8_t*>(key_data.data()));
    } catch (const std::exception& e) {
        spdlog::error("get_earliest_expiry: {}", e.what());
        return std::nullopt;
    }
}

uint64_t Storage::used_bytes() const {
    // Returns mmap file geometry (mi_geo.current), NOT actual data volume.
    // Freed pages are reused internally by libmdbx's B-tree garbage collector.
    // The file only physically shrinks when freed space exceeds shrink_threshold
    // (4 MiB). This is correct mmap database behavior, not a bug.
    auto info = impl_->env.get_info();
    return info.mi_geo.current;
}

uint64_t Storage::used_data_bytes() const {
    auto info = impl_->env.get_info();
    // mi_last_pgno is the last used page number (0-based).
    // Multiply by page size to get actual B-tree data occupancy.
    return (info.mi_last_pgno + 1) * info.mi_dxb_pagesize;
}

void Storage::integrity_scan() {
    try {
        // Collect stats in a scoped read transaction, then release it
        // before calling list_namespaces() (which opens its own read txn).
        uint64_t blobs_entries, seq_entries, expiry_entries, tombstone_entries;
        uint64_t cursor_entries, delegation_entries, quota_entries;
        {
            auto txn = impl_->env.start_read();
            blobs_entries = txn.get_map_stat(impl_->blobs_map).ms_entries;
            seq_entries = txn.get_map_stat(impl_->seq_map).ms_entries;
            expiry_entries = txn.get_map_stat(impl_->expiry_map).ms_entries;
            tombstone_entries = txn.get_map_stat(impl_->tombstone_map).ms_entries;
            cursor_entries = txn.get_map_stat(impl_->cursor_map).ms_entries;
            delegation_entries = txn.get_map_stat(impl_->delegation_map).ms_entries;
            quota_entries = txn.get_map_stat(impl_->quota_map).ms_entries;
        }

        spdlog::info("integrity scan: blobs={} seq={} expiry={} tombstone={} "
                     "cursor={} delegation={} quota={}",
                     blobs_entries, seq_entries, expiry_entries,
                     tombstone_entries, cursor_entries, delegation_entries,
                     quota_entries);

        // Cross-reference checks
        if (seq_entries > 0 && blobs_entries == 0) {
            spdlog::warn("integrity scan: seq_map has {} entries but blobs_map "
                         "is empty (possible data loss)", seq_entries);
        }

        // Check quota_map vs namespace count (list_namespaces opens its own txn)
        auto namespaces = list_namespaces();
        if (quota_entries != namespaces.size() && !namespaces.empty()) {
            spdlog::warn("integrity scan: quota_map has {} entries but {} "
                         "namespaces found (possible stale quota state)",
                         quota_entries, namespaces.size());
        }
    } catch (const std::exception& e) {
        spdlog::error("integrity scan failed: {}", e.what());
    }
}

// =============================================================================
// Sync cursor API
// =============================================================================

std::optional<SyncCursor> Storage::get_sync_cursor(
    std::span<const uint8_t, 32> peer_hash,
    std::span<const uint8_t, 32> namespace_id) {
    try {
        auto key = make_cursor_key(peer_hash.data(), namespace_id.data());
        auto txn = impl_->env.start_read();
        auto val = txn.get(impl_->cursor_map, to_slice(key), not_found_sentinel);
        if (val.data() == nullptr) return std::nullopt;
        if (val.length() != CURSOR_VALUE_SIZE) return std::nullopt;
        return decode_cursor_value(static_cast<const uint8_t*>(val.data()));
    } catch (const std::exception& e) {
        spdlog::error("Storage error in get_sync_cursor: {}", e.what());
        return std::nullopt;
    }
}

void Storage::set_sync_cursor(
    std::span<const uint8_t, 32> peer_hash,
    std::span<const uint8_t, 32> namespace_id,
    const SyncCursor& cursor) {
    try {
        auto key = make_cursor_key(peer_hash.data(), namespace_id.data());
        auto val = encode_cursor_value(cursor);
        auto txn = impl_->env.start_write();
        txn.upsert(impl_->cursor_map, to_slice(key),
                    mdbx::slice(val.data(), val.size()));
        txn.commit();
    } catch (const std::exception& e) {
        spdlog::error("Storage error in set_sync_cursor: {}", e.what());
    }
}

void Storage::delete_sync_cursor(
    std::span<const uint8_t, 32> peer_hash,
    std::span<const uint8_t, 32> namespace_id) {
    try {
        auto key = make_cursor_key(peer_hash.data(), namespace_id.data());
        auto txn = impl_->env.start_write();
        try {
            txn.erase(impl_->cursor_map, to_slice(key));
        } catch (const mdbx::exception&) {
            // Not found -- not an error
        }
        txn.commit();
    } catch (const std::exception& e) {
        spdlog::error("Storage error in delete_sync_cursor: {}", e.what());
    }
}

size_t Storage::delete_peer_cursors(std::span<const uint8_t, 32> peer_hash) {
    size_t deleted = 0;
    try {
        // Collect keys to delete first, then erase them
        std::vector<std::array<uint8_t, 64>> keys_to_delete;
        {
            auto rtxn = impl_->env.start_read();
            auto cursor = rtxn.open_cursor(impl_->cursor_map);
            auto lower = make_cursor_key(peer_hash.data(),
                std::array<uint8_t, 32>{}.data());
            auto seek = cursor.lower_bound(to_slice(lower));
            while (seek.done) {
                auto k = cursor.current(false).key;
                if (k.length() != 64 ||
                    std::memcmp(k.data(), peer_hash.data(), 32) != 0) {
                    break;
                }
                std::array<uint8_t, 64> key;
                std::memcpy(key.data(), k.data(), 64);
                keys_to_delete.push_back(key);
                seek = cursor.to_next(false);
            }
        }

        if (!keys_to_delete.empty()) {
            auto txn = impl_->env.start_write();
            for (const auto& key : keys_to_delete) {
                txn.erase(impl_->cursor_map, to_slice(key));
                ++deleted;
            }
            txn.commit();
        }
    } catch (const std::exception& e) {
        spdlog::error("Storage error in delete_peer_cursors: {}", e.what());
    }
    return deleted;
}

size_t Storage::reset_all_round_counters() {
    size_t count = 0;
    try {
        auto txn = impl_->env.start_write();
        auto cursor = txn.open_cursor(impl_->cursor_map);

        auto result = cursor.to_first(false);
        while (result.done) {
            auto val = cursor.current(false).value;
            if (val.length() == CURSOR_VALUE_SIZE) {
                // Decode, zero round_count, re-encode
                auto cur = decode_cursor_value(
                    static_cast<const uint8_t*>(val.data()));
                cur.round_count = 0;
                auto new_val = encode_cursor_value(cur);
                // Update the value in-place via cursor
                cursor.upsert(cursor.current(false).key,
                               mdbx::slice(new_val.data(), new_val.size()));
                ++count;
            }
            result = cursor.to_next(false);
        }

        txn.commit();
    } catch (const std::exception& e) {
        spdlog::error("Storage error in reset_all_round_counters: {}", e.what());
    }
    return count;
}

std::vector<std::array<uint8_t, 32>> Storage::list_cursor_peers() {
    std::vector<std::array<uint8_t, 32>> peers;
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->cursor_map);

        auto result = cursor.to_first(false);
        std::array<uint8_t, 32> last_peer{};
        bool have_last = false;

        while (result.done) {
            auto k = cursor.current(false).key;
            if (k.length() != 64) break;

            std::array<uint8_t, 32> peer;
            std::memcpy(peer.data(), k.data(), 32);

            if (!have_last || peer != last_peer) {
                peers.push_back(peer);
                last_peer = peer;
                have_last = true;
            }

            result = cursor.to_next(false);
        }
    } catch (const std::exception& e) {
        spdlog::error("Storage error in list_cursor_peers: {}", e.what());
    }
    return peers;
}

size_t Storage::cleanup_stale_cursors(
    const std::vector<std::array<uint8_t, 32>>& known_peer_hashes) {
    // Build a set for O(1) lookups
    std::set<std::array<uint8_t, 32>> known_set(
        known_peer_hashes.begin(), known_peer_hashes.end());

    auto peers = list_cursor_peers();
    size_t total_deleted = 0;
    for (const auto& peer : peers) {
        if (known_set.find(peer) == known_set.end()) {
            total_deleted += delete_peer_cursors(peer);
        }
    }
    return total_deleted;
}

// =============================================================================
// Namespace quota API
// =============================================================================

NamespaceQuota Storage::get_namespace_quota(
    std::span<const uint8_t, 32> ns) {
    try {
        auto txn = impl_->env.start_read();
        auto ns_slice = mdbx::slice(ns.data(), 32);
        auto val = txn.get(impl_->quota_map, ns_slice, not_found_sentinel);
        if (val.data() == nullptr || val.length() != QUOTA_VALUE_SIZE) {
            return {};
        }
        return decode_quota_value(static_cast<const uint8_t*>(val.data()));
    } catch (const std::exception& e) {
        spdlog::error("Storage error in get_namespace_quota: {}", e.what());
        return {};
    }
}

void Storage::rebuild_quota_aggregates() {
    try {
        auto txn = impl_->env.start_write();

        // Clear existing quota entries
        // RES-04 (D-12): Fixed iterator -- erase current, restart from first
        {
            auto cursor = txn.open_cursor(impl_->quota_map);
            auto result = cursor.to_first(false);
            while (result.done) {
                cursor.erase();                    // erase current entry
                result = cursor.to_first(false);   // restart from first remaining
            }
        }

        // Scan blobs_map: key = [namespace:32][hash:32], value = encrypted blob
        std::map<std::array<uint8_t, 32>, NamespaceQuota> aggregates;
        {
            auto cursor = txn.open_cursor(impl_->blobs_map);
            auto result = cursor.to_first(false);
            while (result.done) {
                auto current = cursor.current(false);
                auto key = current.key;
                auto val = current.value;

                if (key.length() == 64 && val.length() > 0) {
                    std::array<uint8_t, 32> ns_id{};
                    std::memcpy(ns_id.data(), key.data(), 32);
                    aggregates[ns_id].total_bytes += val.length();
                    aggregates[ns_id].blob_count += 1;
                }

                result = cursor.to_next(false);
            }
        }

        // Write aggregates to quota_map
        size_t total_count = 0;
        for (const auto& [ns_id, quota] : aggregates) {
            auto ns_slice = mdbx::slice(ns_id.data(), 32);
            auto val = encode_quota_value(quota);
            txn.upsert(impl_->quota_map, ns_slice,
                        mdbx::slice(val.data(), val.size()));
            total_count += quota.blob_count;
        }

        txn.commit();
        spdlog::info("Quota rebuild: {} namespaces, {} total blobs",
                     aggregates.size(), total_count);
    } catch (const std::exception& e) {
        spdlog::error("Storage error in rebuild_quota_aggregates: {}", e.what());
    }
}

// =============================================================================
// Compaction
// =============================================================================

CompactResult Storage::compact() {
    CompactResult result;
    auto start = std::chrono::steady_clock::now();

    try {
        result.before_bytes = used_bytes();

        // data_dir_ is a directory containing mdbx.dat and mdbx.lck.
        // env.copy() with compactify=true creates a single-file copy at the destination.
        // Strategy: copy to temp file, close env, swap mdbx.dat, reopen.
        auto db_file = fs::path(impl_->data_dir_) / "mdbx.dat";
        auto temp_file = fs::path(impl_->data_dir_) / "mdbx.compact";

        // Clean up any prior interrupted compaction
        std::error_code ec;
        fs::remove(temp_file, ec);

        // Live compacted copy -- does NOT block concurrent reads/writes
        impl_->env.copy(temp_file.string(), true);

        // Close current env (releases file locks)
        impl_->env.close();

        // Swap: rename compacted copy over the original mdbx.dat
        fs::rename(temp_file, db_file);
        // Remove stale lockfile so mdbx creates a fresh one on reopen
        fs::remove(fs::path(impl_->data_dir_) / "mdbx.lck", ec);

        // Reopen env from the swapped-in compacted file
        impl_->open_env(impl_->data_dir_);

        result.after_bytes = used_bytes();
        result.success = true;

    } catch (const std::exception& e) {
        spdlog::error("compaction failed: {}", e.what());

        // Attempt to reopen the original (might still be intact if copy failed)
        try {
            impl_->open_env(impl_->data_dir_);
        } catch (const std::exception& reopen_err) {
            spdlog::error("CRITICAL: failed to reopen storage after compaction failure: {}",
                          reopen_err.what());
        }

        // Clean up temp file
        std::error_code ec;
        fs::remove(fs::path(impl_->data_dir_) / "mdbx.compact", ec);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    result.duration_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    return result;
}

// =============================================================================
// Backup
// =============================================================================

bool Storage::backup(const std::string& dest_path) {
    try {
        // Live compacted copy to destination — does NOT block concurrent reads/writes
        impl_->env.copy(dest_path, true);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("backup failed: {}", e.what());
        return false;
    }
}

} // namespace chromatindb::storage
