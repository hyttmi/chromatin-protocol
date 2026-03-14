#include "db/storage/storage.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include <mdbx.h++>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include "db/crypto/aead.h"
#include "db/crypto/hash.h"
#include "db/crypto/master_key.h"
#include "db/wire/codec.h"

namespace chromatindb::storage {

namespace fs = std::filesystem;

// =============================================================================
// Byte helpers
// =============================================================================

static void encode_be_u64(uint64_t val, uint8_t* out) {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = static_cast<uint8_t>(val >> (i * 8));
    }
}

static uint64_t decode_be_u64(const uint8_t* data) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | data[i];
    }
    return val;
}

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
    encode_be_u64(seq_num, key.data() + 32);
    return key;
}

static std::array<uint8_t, 40> make_expiry_key(
    uint64_t expiry_ts, const uint8_t* hash) {
    std::array<uint8_t, 40> key;
    encode_be_u64(expiry_ts, key.data());
    std::memcpy(key.data() + 8, hash, 32);
    return key;
}

template <size_t N>
static mdbx::slice to_slice(const std::array<uint8_t, N>& arr) {
    return mdbx::slice(arr.data(), arr.size());
}

// Sentinel slice for "not found" results from txn.get() 3-arg overload.
static const mdbx::slice not_found_sentinel;

// =============================================================================
// Encryption at rest constants
// =============================================================================

static constexpr uint8_t ENCRYPTION_VERSION = 0x01;
static constexpr size_t ENVELOPE_OVERHEAD =
    1 + crypto::AEAD::NONCE_SIZE + crypto::AEAD::TAG_SIZE;  // 29 bytes

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
    Clock clock;
    crypto::SecureBytes blob_key_;  // Derived from master key via HKDF

    Impl(const std::string& data_dir, crypto::SecureBytes master_key, Clock clk)
        : clock(std::move(clk)) {
        fs::create_directories(data_dir);

        // Create parameters with geometry
        mdbx::env_managed::create_parameters create_params;
        create_params.geometry.size_lower = 1 * mdbx::env::geometry::MiB;
        create_params.geometry.size_now = mdbx::env::geometry::default_value;
        create_params.geometry.size_upper = 64LL * mdbx::env::geometry::GiB;
        create_params.geometry.growth_step = 1 * mdbx::env::geometry::MiB;
        create_params.geometry.shrink_threshold = 4 * mdbx::env::geometry::MiB;
        create_params.use_subdirectory = false;

        // Operate parameters
        mdbx::env::operate_parameters operate_params;
        operate_params.max_maps = 6;  // 5 sub-databases + 1 spare
        operate_params.max_readers = 64;
        operate_params.mode = mdbx::env::mode::write_mapped_io;
        operate_params.durability = mdbx::env::durability::robust_synchronous;

        env = mdbx::env_managed(data_dir, create_params, operate_params);

        // Create all 5 sub-databases in a single write transaction
        {
            auto txn = env.start_write();
            blobs_map = txn.create_map("blobs");
            seq_map = txn.create_map("sequence");
            expiry_map = txn.create_map("expiry");
            delegation_map = txn.create_map("delegation");
            tombstone_map = txn.create_map("tombstone");
            txn.commit();
        }

        // Derive blob encryption key from master key
        blob_key_ = crypto::derive_blob_key(master_key);

        // Validate no unencrypted data exists in the database
        {
            auto rtxn = env.start_read();
            validate_no_unencrypted_data(rtxn);
        }

        spdlog::info("Storage opened at {} with encryption at rest", data_dir);
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
                uint64_t current = decode_be_u64(
                    static_cast<const uint8_t*>(key_data.data()) + 32);
                return current + 1;
            }
            // Found key is in a different namespace. Try previous entry.
            auto prev = cursor.to_previous(false);
            if (prev.done) {
                auto prev_key = prev.key;
                if (prev_key.length() == 40 &&
                    std::memcmp(prev_key.data(), ns, 32) == 0) {
                    uint64_t current = decode_be_u64(
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
                    uint64_t current = decode_be_u64(
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
          std::move(clock))) {}

Storage::~Storage() = default;
Storage::Storage(Storage&& other) noexcept = default;
Storage& Storage::operator=(Storage&& other) noexcept = default;

StoreResult Storage::store_blob(const wire::BlobData& blob) {
    try {
        auto encoded = wire::encode_blob(blob);
        auto hash = wire::blob_hash(encoded);
        auto blob_key = make_blob_key(blob.namespace_id.data(), hash.data());
        auto key_slice = to_slice(blob_key);

        auto txn = impl_->env.start_write();

        // Check dedup BEFORE encryption (hash is on plaintext)
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
                if (v.length() == 32 &&
                    std::memcmp(v.data(), hash.data(), 32) == 0) {
                    existing_seq = decode_be_u64(
                        static_cast<const uint8_t*>(k.data()) + 32);
                    break;
                }
                seek = cursor.to_next(false);
            }

            txn.abort();
            StoreResult result;
            result.status = StoreResult::Status::Duplicate;
            result.seq_num = existing_seq;
            result.blob_hash = hash;
            return result;
        }

        // Encrypt the encoded blob value (AD = 64-byte mdbx key)
        auto encrypted = impl_->encrypt_value(
            std::span<const uint8_t>(encoded.data(), encoded.size()),
            std::span<const uint8_t>(blob_key.data(), blob_key.size()));

        // Store encrypted blob
        txn.upsert(impl_->blobs_map, key_slice,
                    mdbx::slice(encrypted.data(), encrypted.size()));

        // Assign seq_num
        uint64_t seq = impl_->next_seq_num(txn, blob.namespace_id.data());
        auto seq_key = make_seq_key(blob.namespace_id.data(), seq);
        txn.upsert(impl_->seq_map, to_slice(seq_key),
                    mdbx::slice(hash.data(), hash.size()));

        // Store expiry entry (skip for TTL=0)
        if (blob.ttl > 0) {
            uint64_t expiry_time = static_cast<uint64_t>(blob.timestamp) +
                                   static_cast<uint64_t>(blob.ttl);
            auto exp_key = make_expiry_key(expiry_time, hash.data());
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
                        mdbx::slice(hash.data(), hash.size()));
        }

        // Populate tombstone index if this is a tombstone blob
        if (wire::is_tombstone(blob.data)) {
            auto target_hash = wire::extract_tombstone_target(
                std::span<const uint8_t>(blob.data));
            auto ts_key = make_blob_key(blob.namespace_id.data(), target_hash.data());
            txn.upsert(impl_->tombstone_map, to_slice(ts_key), mdbx::slice());
        }

        txn.commit();
        spdlog::debug("Stored blob in namespace (seq={})", seq);

        StoreResult result;
        result.status = StoreResult::Status::Stored;
        result.seq_num = seq;
        result.blob_hash = hash;
        return result;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in store_blob: {}", e.what());
        StoreResult result;
        result.status = StoreResult::Status::Error;
        return result;
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

            // Extract hash from value
            auto val_data = cursor.current(false).value;
            if (val_data.length() != 32) break;

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

            // Read hash from value (32 bytes)
            auto val_data = cursor.current(false).value;
            if (val_data.length() == 32) {
                std::array<uint8_t, 32> hash;
                std::memcpy(hash.data(), val_data.data(), 32);
                hashes.push_back(hash);
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
                    info.latest_seq_num = decode_be_u64(
                        static_cast<const uint8_t*>(k.data()) + 32);
                } else {
                    // It's in the next namespace; go back one
                    auto prev = cursor.to_previous(false);
                    if (prev.done) {
                        auto pk = prev.key;
                        if (pk.length() == 40 &&
                            std::memcmp(pk.data(), info.namespace_id.data(), 32) == 0) {
                            info.latest_seq_num = decode_be_u64(
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
                        info.latest_seq_num = decode_be_u64(
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

        // Decrypt and decode to get TTL + timestamp for expiry cleanup
        auto decrypted = impl_->decrypt_value(
            std::span<const uint8_t>(
                static_cast<const uint8_t*>(existing.data()), existing.length()),
            std::span<const uint8_t>(blob_key.data(), blob_key.size()));
        auto blob = wire::decode_blob(
            std::span<const uint8_t>(decrypted.data(), decrypted.size()));

        // Delete from blobs_map
        txn.erase(impl_->blobs_map, key_slice);

        // Delete from seq_map (scan for matching hash)
        {
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
                if (v.length() == 32 &&
                    std::memcmp(v.data(), blob_hash.data(), 32) == 0) {
                    cursor.erase();
                    break;
                }
                seek = cursor.to_next(false);
            }
        }

        // Delete from expiry_map if TTL > 0
        if (blob.ttl > 0) {
            uint64_t expiry_time = static_cast<uint64_t>(blob.timestamp) +
                                   static_cast<uint64_t>(blob.ttl);
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

            uint64_t expiry_ts = decode_be_u64(
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
                // Decrypt the value (AD = 64-byte mdbx key)
                auto decrypted = impl_->decrypt_value(
                    std::span<const uint8_t>(
                        static_cast<const uint8_t*>(raw.data()), raw.length()),
                    std::span<const uint8_t>(blob_key.data(), blob_key.size()));
                auto decoded = wire::decode_blob(
                    std::span<const uint8_t>(decrypted.data(), decrypted.size()));
                if (wire::is_tombstone(decoded.data)) {
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

uint64_t Storage::used_bytes() const {
    auto info = impl_->env.get_info();
    return info.mi_geo.current;
}

} // namespace chromatindb::storage
