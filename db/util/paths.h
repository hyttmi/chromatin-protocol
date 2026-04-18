#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace chromatindb::util {

/// Resolve the canonical config path for peer-management subcommands.
/// Lookup order:
///   1. $CHROMATINDB_CONFIG environment variable (if set and non-empty)
///   2. /etc/chromatindb/node.json (system install)
///   3. ./data/config.json (dev default)
inline std::string resolve_config_path() {
    if (const char* env = std::getenv("CHROMATINDB_CONFIG"); env && *env) {
        return env;
    }
    const std::string system_path = "/etc/chromatindb/node.json";
    std::error_code ec;
    if (std::filesystem::exists(system_path, ec)) {
        return system_path;
    }
    return "./data/config.json";
}

} // namespace chromatindb::util
