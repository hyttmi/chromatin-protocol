#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mdbx.h++"

namespace chromatin::storage {

inline constexpr const char* TABLE_PROFILES   = "profiles";
inline constexpr const char* TABLE_NAMES      = "names";
inline constexpr const char* TABLE_INBOXES    = "inboxes";
inline constexpr const char* TABLE_REQUESTS   = "requests";
inline constexpr const char* TABLE_ALLOWLISTS = "allowlists";
inline constexpr const char* TABLE_REPL_LOG   = "repl_log";
inline constexpr const char* TABLE_NODES      = "nodes";
inline constexpr const char* TABLE_REPUTATION = "reputation";

class Storage {
public:
    explicit Storage(const std::filesystem::path& db_path);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    bool put(std::string_view table, std::span<const uint8_t> key, std::span<const uint8_t> value);
    std::optional<std::vector<uint8_t>> get(std::string_view table, std::span<const uint8_t> key) const;
    bool del(std::string_view table, std::span<const uint8_t> key);

    using Callback = std::function<bool(std::span<const uint8_t> key, std::span<const uint8_t> value)>;
    void foreach(std::string_view table, Callback cb) const;

private:
    mdbx::env_managed env_;
    std::unordered_map<std::string, mdbx::map_handle> maps_;

    mdbx::map_handle get_map(std::string_view table) const;
};

} // namespace chromatin::storage
