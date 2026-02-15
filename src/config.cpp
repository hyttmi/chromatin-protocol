/*
 * Helix - Configuration
 */

#include "helix/config.h"

#include <cstring>
#include <iostream>

namespace helix {

static void print_usage(const char *prog)
{
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --listen <addr>       Listen address (default: 0.0.0.0)\n"
        << "  --port <port>         Listen port (default: 9090)\n"
        << "  --tls-cert <path>     TLS certificate file\n"
        << "  --tls-key <path>      TLS private key file\n"
        << "  --dht-port <port>     DHT port (default: 4222)\n"
        << "  --bootstrap <host>    DHT bootstrap node (repeatable)\n"
        << "  --data-dir <path>     Data directory (default: ./helix-data)\n"
        << "  --log-level <level>   Log level: trace,debug,info,warn,error (default: info)\n"
        << "  --help                Show this help\n";
}

bool Config::parse(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char *name) {
            return std::strcmp(argv[i], name) == 0;
        };

        auto next = [&]() -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << argv[i] << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg("--help") || arg("-h")) {
            print_usage(argv[0]);
            return false;
        } else if (arg("--listen")) {
            auto v = next(); if (!v) return false;
            listen_addr = v;
        } else if (arg("--port")) {
            auto v = next(); if (!v) return false;
            listen_port = static_cast<uint16_t>(std::stoi(v));
        } else if (arg("--tls-cert")) {
            auto v = next(); if (!v) return false;
            tls_cert = v;
        } else if (arg("--tls-key")) {
            auto v = next(); if (!v) return false;
            tls_key = v;
        } else if (arg("--dht-port")) {
            auto v = next(); if (!v) return false;
            dht_port = static_cast<uint16_t>(std::stoi(v));
        } else if (arg("--bootstrap")) {
            auto v = next(); if (!v) return false;
            bootstrap_nodes.emplace_back(v);
        } else if (arg("--data-dir")) {
            auto v = next(); if (!v) return false;
            data_dir = v;
        } else if (arg("--log-level")) {
            auto v = next(); if (!v) return false;
            log_level = v;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return false;
        }
    }

    return true;
}

} // namespace helix
