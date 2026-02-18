#include "storage/storage.h"

#include <stdexcept>

namespace chromatin::storage {

Storage::Storage(const std::filesystem::path& db_path) {
    // Ensure parent directory exists
    std::filesystem::create_directories(db_path.parent_path());

    mdbx::env_managed::create_parameters create_params;
    create_params.geometry.size_lower = 1UL << 20;       // 1 MB
    create_params.geometry.size_upper = 1UL << 30;       // 1 GB
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
        TABLE_PROFILES, TABLE_NAMES, TABLE_INBOXES, TABLE_REQUESTS,
        TABLE_ALLOWLISTS, TABLE_REPL_LOG, TABLE_NODES, TABLE_REPUTATION,
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

} // namespace chromatin::storage
