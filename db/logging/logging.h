#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace chromatindb::logging {

/// Initialize the logging system with console + optional file sink.
/// @param level Log level string: "trace", "debug", "info", "warn", "error", "critical"
/// @param log_file Path to rotating log file (empty = console only)
/// @param max_size_mb Max size per log file in MiB before rotation
/// @param max_files Max number of rotated log files to keep
/// @param log_format Log format: "text" or "json"
void init(const std::string& level = "info",
          const std::string& log_file = "",
          uint32_t max_size_mb = 10,
          uint32_t max_files = 3,
          const std::string& log_format = "text");

/// Change the log level at runtime (e.g. on SIGHUP config reload).
/// Updates the default logger, all registered named loggers, and the global level.
void set_level(const std::string& level);

/// Get or create a named logger.
/// Uses shared sinks from init() so all loggers write to both console and file.
std::shared_ptr<spdlog::logger> get_logger(const std::string& name);

} // namespace chromatindb::logging
