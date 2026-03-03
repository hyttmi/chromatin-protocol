#include "storage/storage.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include <mdbx.h++>
#include <spdlog/spdlog.h>

#include "crypto/hash.h"
#include "wire/codec.h"

namespace chromatin::storage {

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
// Storage::Impl
// =============================================================================

struct Storage::Impl {
    mdbx::env_managed env;
    mdbx::map_handle blobs_map{0};
    mdbx::map_handle seq_map{0};
    mdbx::map_handle expiry_map{0};
    Clock clock;

    Impl(const std::string& data_dir, Clock clk) : clock(std::move(clk)) {
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
        operate_params.max_maps = 4;  // 3 needed + 1 spare
        operate_params.max_readers = 64;
        operate_params.mode = mdbx::env::mode::write_mapped_io;
        operate_params.durability = mdbx::env::durability::robust_synchronous;

        env = mdbx::env_managed(data_dir, create_params, operate_params);

        // Create all 3 sub-databases in a single write transaction
        {
            auto txn = env.start_write();
            blobs_map = txn.create_map("blobs");
            seq_map = txn.create_map("sequence");
            expiry_map = txn.create_map("expiry");
            txn.commit();
        }

        spdlog::info("Storage opened at {} with 3 sub-databases", data_dir);
    }

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
    : impl_(std::make_unique<Impl>(data_dir, std::move(clock))) {}

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

        // Check dedup: use 3-arg get with sentinel for not-found
        auto existing = txn.get(impl_->blobs_map, key_slice, not_found_sentinel);
        if (existing.data() != not_found_sentinel.data() || existing.length() != 0 || existing.data() != nullptr) {
            // Need a better check. The sentinel is an empty slice (null, 0).
            // If get returns a non-null slice, the key was found.
            if (existing.data() != nullptr) {
                txn.abort();
                return StoreResult::Duplicate;
            }
        }

        // Store blob
        txn.upsert(impl_->blobs_map, key_slice,
                    mdbx::slice(encoded.data(), encoded.size()));

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

        txn.commit();
        spdlog::debug("Stored blob in namespace (seq={})", seq);
        return StoreResult::Stored;

    } catch (const std::exception& e) {
        spdlog::error("Storage error in store_blob: {}", e.what());
        return StoreResult::Error;
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

        auto decoded = wire::decode_blob(std::span<const uint8_t>(
            static_cast<const uint8_t*>(val.data()), val.length()));
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
                auto blob = wire::decode_blob(std::span<const uint8_t>(
                    static_cast<const uint8_t*>(blob_val.data()),
                    blob_val.length()));
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

            // Delete from blobs index (ignore if already deleted)
            auto blob_key = make_blob_key(ns_ptr, hash_ptr);
            try {
                txn.erase(impl_->blobs_map, to_slice(blob_key));
            } catch (const mdbx::exception&) {
                // Already deleted -- not an error
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

} // namespace chromatin::storage
