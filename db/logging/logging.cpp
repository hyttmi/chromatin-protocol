#include "db/logging/logging.h"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>
#include <vector>

namespace chromatindb::logging {

static std::vector<spdlog::sink_ptr> shared_sinks;
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

void init(const std::string& level,
          const std::string& log_file,
          uint32_t max_size_mb,
          uint32_t max_files,
          const std::string& log_format) {
    global_level = parse_level(level);
    shared_sinks.clear();

    // Console sink (always present)
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    shared_sinks.push_back(console_sink);

    // File sink (optional)
    if (!log_file.empty()) {
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file,
                static_cast<std::size_t>(max_size_mb) * 1024 * 1024,
                max_files);
            shared_sinks.push_back(file_sink);
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "warning: failed to open log file '" << log_file
                      << "': " << ex.what() << " (falling back to console only)\n";
        }
    }

    // Set pattern on all sinks (same format for both)
    std::string pattern;
    if (log_format == "json") {
        pattern = R"({"ts":"%Y-%m-%dT%H:%M:%S.%e","level":"%l","logger":"%n","msg":"%v"})";
    } else {
        pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v";
    }
    for (auto& sink : shared_sinks) {
        sink->set_pattern(pattern);
    }

    // Create default logger from shared sinks
    auto default_logger = std::make_shared<spdlog::logger>("",
        shared_sinks.begin(), shared_sinks.end());
    default_logger->set_level(global_level);
    spdlog::set_default_logger(default_logger);
}

void set_level(const std::string& level) {
    auto new_level = parse_level(level);
    if (new_level == global_level) return;

    global_level = new_level;
    spdlog::default_logger()->set_level(global_level);
    spdlog::apply_all([](std::shared_ptr<spdlog::logger> logger) {
        logger->set_level(global_level);
    });
}

std::shared_ptr<spdlog::logger> get_logger(const std::string& name) {
    auto logger = spdlog::get(name);
    if (!logger) {
        logger = std::make_shared<spdlog::logger>(name,
            shared_sinks.begin(), shared_sinks.end());
        logger->set_level(global_level);
        spdlog::register_logger(logger);
    }
    return logger;
}

} // namespace chromatindb::logging
