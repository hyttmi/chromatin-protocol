#include "storage/storage.h"

#include <cstring>
#include <stdexcept>

namespace chromatin::storage {

Storage::Storage(const std::filesystem::path& db_path, uint64_t max_size) {
    // Ensure parent directory exists
    std::filesystem::create_directories(db_path.parent_path());

    mdbx::env_managed::create_parameters create_params;
    create_params.geometry.size_lower = 1UL << 20;       // 1 MB
    create_params.geometry.size_upper = max_size;
    create_params.geometry.growth_step = 1UL << 20;      // 1 MB
    create_params.geometry.shrink_threshold = 2UL << 20;  // 2 MB
    create_params.geometry.pagesize = 4096;

    mdbx::env::operate_parameters operate_params;
    operate_params.max_maps = 16;
    operate_params.max_readers = 64;

    env_ = mdbx::env_managed(db_path.string(), create_params, operate_params);

    // Pre-create all table maps in a write transaction
    auto txn = env_.start_write();

    const char* tables[] = {
        TABLE_PROFILES, TABLE_NAMES, TABLE_REQUESTS,
        TABLE_ALLOWLISTS, TABLE_REPL_LOG, TABLE_NODES, TABLE_REPUTATION,
        TABLE_INBOX_INDEX, TABLE_MESSAGE_BLOBS,
        TABLE_GROUP_META, TABLE_GROUP_INDEX, TABLE_GROUP_BLOBS,
    };

    for (const auto* table : tables) {
        maps_[table] = txn.create_map(table);
    }

    txn.commit();
}

Storage::~Storage() {
    // env_managed closes itself in its destructor
}

mdbx::map_handle Storage::get_map(std::string_view table) const {
    auto it = maps_.find(std::string(table));
    if (it == maps_.end()) {
        throw std::invalid_argument("Unknown table: " + std::string(table));
    }
    return it->second;
}

bool Storage::put(std::string_view table, std::span<const uint8_t> key, std::span<const uint8_t> value) {
    auto map = get_map(table);
    auto txn = env_.start_write();
    txn.upsert(map,
               mdbx::slice(key.data(), key.size()),
               mdbx::slice(value.data(), value.size()));
    txn.commit();
    return true;
}

std::optional<std::vector<uint8_t>> Storage::get(std::string_view table, std::span<const uint8_t> key) const {
    auto map = get_map(table);
    auto txn = env_.start_read();

    // Use the overload with value_at_absence to avoid exception on missing key
    mdbx::slice sentinel = mdbx::slice();
    mdbx::slice result = txn.get(map, mdbx::slice(key.data(), key.size()), sentinel);

    if (result.data() == nullptr && result.size() == 0) {
        return std::nullopt;
    }

    auto* begin = static_cast<const uint8_t*>(result.data());
    return std::vector<uint8_t>(begin, begin + result.size());
}

bool Storage::batch_put(const std::vector<BatchOp>& ops) {
    auto txn = env_.start_write();
    for (const auto& op : ops) {
        auto map = get_map(op.table);
        txn.upsert(map,
                    mdbx::slice(op.key.data(), op.key.size()),
                    mdbx::slice(op.value.data(), op.value.size()));
    }
    txn.commit();
    return true;
}

bool Storage::del(std::string_view table, std::span<const uint8_t> key) {
    auto map = get_map(table);
    auto txn = env_.start_write();
    bool erased = txn.erase(map, mdbx::slice(key.data(), key.size()));
    txn.commit();
    return erased;
}

void Storage::foreach(std::string_view table, Callback cb) const {
    auto map = get_map(table);
    auto txn = env_.start_read();
    auto cursor = txn.open_cursor(map);

    auto result = cursor.to_first(false);
    while (result) {
        auto k = result.key;
        auto v = result.value;
        std::span<const uint8_t> key_span(static_cast<const uint8_t*>(k.data()), k.size());
        std::span<const uint8_t> val_span(static_cast<const uint8_t*>(v.data()), v.size());
        if (!cb(key_span, val_span)) {
            break;
        }
        result = cursor.to_next(false);
    }
}

void Storage::scan(std::string_view table, std::span<const uint8_t> prefix, Callback cb) const {
    auto map = get_map(table);
    auto txn = env_.start_read();
    auto cursor = txn.open_cursor(map);

    auto result = cursor.lower_bound(mdbx::slice(prefix.data(), prefix.size()), false);
    while (result) {
        auto k = result.key;
        // Stop if key is shorter than prefix or doesn't start with prefix
        if (k.size() < prefix.size() ||
            std::memcmp(k.data(), prefix.data(), prefix.size()) != 0) {
            break;
        }
        auto v = result.value;
        std::span<const uint8_t> key_span(static_cast<const uint8_t*>(k.data()), k.size());
        std::span<const uint8_t> val_span(static_cast<const uint8_t*>(v.data()), v.size());
        if (!cb(key_span, val_span)) {
            break;
        }
        result = cursor.to_next(false);
    }
}

void Storage::reverse_scan_one(std::string_view table, std::span<const uint8_t> prefix,
                               std::span<const uint8_t> upper_bound, Callback cb) const {
    auto map = get_map(table);
    auto txn = env_.start_read();
    auto cursor = txn.open_cursor(map);

    // Seek to upper_bound (or past it)
    auto result = cursor.lower_bound(mdbx::slice(upper_bound.data(), upper_bound.size()), false);

    // If lower_bound found a key >= upper_bound, move back one position
    if (result) {
        result = cursor.to_previous(false);
    } else {
        // No key >= upper_bound, try the very last key in the table
        result = cursor.to_last(false);
    }

    if (!result) return;

    auto k = result.key;
    // Verify the found key starts with our prefix
    if (k.size() >= prefix.size() &&
        std::memcmp(k.data(), prefix.data(), prefix.size()) == 0) {
        auto v = result.value;
        std::span<const uint8_t> key_span(static_cast<const uint8_t*>(k.data()), k.size());
        std::span<const uint8_t> val_span(static_cast<const uint8_t*>(v.data()), v.size());
        cb(key_span, val_span);
    }
}

} // namespace chromatin::storage
