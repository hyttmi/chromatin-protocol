/*
 * Helix - Logging
 *
 * Structured logging via spdlog. Every component gets a named logger.
 * Log levels configurable at runtime.
 *
 * Privacy: never log full fingerprints, private keys, session tokens,
 * or raw blob data. Truncate fingerprints to 16 chars.
 */

#pragma once

#include <spdlog/spdlog.h>
#include <memory>
#include <string>

namespace helix::log {

void init(const std::string &level);
std::shared_ptr<spdlog::logger> get(const std::string &component);

} // namespace helix::log
