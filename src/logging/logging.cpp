#include "logging/logging.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <mutex>

namespace chromatin::logging {

static std::once_flag init_flag;
static spdlog::level::level_enum global_level = spdlog::level::info;

static spdlog::level::level_enum parse_level(const std::string& level) {
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "info") return spdlog::level::info;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error" || level == "err") return spdlog::level::err;
    if (level == "critical") return spdlog::level::critical;
    return spdlog::level::info;
}

void init(const std::string& level) {
    global_level = parse_level(level);

    std::call_once(init_flag, [&]() {
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
        spdlog::set_level(global_level);
    });

    // Allow level updates even after first init
    spdlog::set_level(global_level);
}

std::shared_ptr<spdlog::logger> get_logger(const std::string& name) {
    auto logger = spdlog::get(name);
    if (!logger) {
        logger = spdlog::stderr_color_mt(name);
        logger->set_level(global_level);
    }
    return logger;
}

} // namespace chromatin::logging
