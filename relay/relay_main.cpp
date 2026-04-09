#include "relay/config/relay_config.h"
#include "relay/identity/relay_identity.h"

#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

void print_usage(const char* prog) {
    std::cerr << "chromatindb_relay v3.0.0\n\n"
              << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --config <path>   JSON config file (required)\n"
              << "  --help            Show this message\n"
              << "  --version         Show version\n";
}

std::string to_hex(std::span<const uint8_t> data) {
    std::string s;
    s.reserve(data.size() * 2);
    for (auto b : data) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
    }
    return s;
}

void init_logging(const std::string& level, const std::string& log_file) {
    std::vector<spdlog::sink_ptr> sinks;

    // Console sink (always present)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sinks.push_back(console_sink);

    // Rotating file sink (optional)
    if (!log_file.empty()) {
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file,
                10 * 1024 * 1024,  // 10 MiB
                3);                 // 3 rotated files
            sinks.push_back(file_sink);
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "warning: failed to open log file '" << log_file
                      << "': " << ex.what() << " (falling back to console only)\n";
        }
    }

    auto logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());

    // Parse level
    if (level == "trace") logger->set_level(spdlog::level::trace);
    else if (level == "debug") logger->set_level(spdlog::level::debug);
    else if (level == "warn") logger->set_level(spdlog::level::warn);
    else if (level == "error") logger->set_level(spdlog::level::err);
    else logger->set_level(spdlog::level::info);

    spdlog::set_default_logger(logger);
}

} // namespace

int main(int argc, char* argv[]) {
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "chromatindb_relay v3.0.0" << std::endl;
            return 0;
        }
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        std::cerr << "error: --config <path> is required\n\n";
        print_usage(argv[0]);
        return 1;
    }

    // Load config
    chromatindb::relay::config::RelayConfig cfg;
    try {
        cfg = chromatindb::relay::config::load_relay_config(config_path);
    } catch (const std::runtime_error& e) {
        std::cerr << "error: failed to load config: " << e.what() << "\n";
        return 1;
    }

    // Validate config
    try {
        chromatindb::relay::config::validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        std::cerr << "error: config validation failed: " << e.what() << "\n";
        return 1;
    }

    // Initialize logging
    init_logging(cfg.log_level, cfg.log_file);

    spdlog::info("chromatindb_relay starting");
    spdlog::info("  bind: {}:{}", cfg.bind_address, cfg.bind_port);
    spdlog::info("  uds: {}", cfg.uds_path);
    spdlog::info("  max_send_queue: {}", cfg.max_send_queue);

    // Load or generate identity
    try {
        auto identity = chromatindb::relay::identity::RelayIdentity::load_or_generate(
            cfg.identity_key_path);
        spdlog::info("  identity: {}", to_hex(identity.public_key_hash()));
    } catch (const std::runtime_error& e) {
        spdlog::error("failed to load identity: {}", e.what());
        return 1;
    }

    // Create io_context
    asio::io_context ioc;

    // Signal handling
    asio::signal_set term_signals(ioc, SIGTERM, SIGINT);
    term_signals.async_wait([&](const asio::error_code&, int sig) {
        spdlog::info("received signal {}, shutting down", sig);
        ioc.stop();
    });

    asio::signal_set hup_signal(ioc, SIGHUP);
    std::function<void(const asio::error_code&, int)> hup_handler;
    hup_handler = [&](const asio::error_code& ec, int) {
        if (ec) return;
        spdlog::info("received SIGHUP -- config reload not yet implemented");
        hup_signal.async_wait(hup_handler);
    };
    hup_signal.async_wait(hup_handler);

    spdlog::info("relay ready (no accept loop -- Phase 101)");

    ioc.run();

    spdlog::info("relay stopped");
    return 0;
}
