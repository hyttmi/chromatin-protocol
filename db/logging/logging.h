#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace chromatindb::logging {

/// Initialize the logging system with a console sink.
/// Safe to call multiple times (idempotent).
/// @param level Log level string: "trace", "debug", "info", "warn", "error", "critical"
void init(const std::string& level = "info");

/// Get or create a named logger.
std::shared_ptr<spdlog::logger> get_logger(const std::string& name);

} // namespace chromatindb::logging
