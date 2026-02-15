/*
 * Helix - Logging
 */

#include "helix/log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>

namespace helix::log {

void init(const std::string &level)
{
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::from_str(level));
}

std::shared_ptr<spdlog::logger> get(const std::string &component)
{
    auto logger = spdlog::get(component);
    if (!logger) {
        logger = spdlog::stdout_color_mt(component);
    }
    return logger;
}

} // namespace helix::log
