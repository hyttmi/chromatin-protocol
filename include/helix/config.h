/*
 * Helix - Configuration
 *
 * Command-line and file-based configuration for a helix node.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define HELIX_VERSION "0.1.0"
#define HELIX_MAX_BLOB_SIZE (50 * 1024 * 1024)  /* 50 MB */
#define HELIX_MAX_PREKEY_SIZE (16 * 1024)        /* 16 KB */

namespace helix {

struct Config {
    /* Network */
    std::string listen_addr = "0.0.0.0";
    uint16_t listen_port = 9090;

    /* TLS */
    std::string tls_cert;
    std::string tls_key;

    /* DHT */
    uint16_t dht_port = 4222;
    std::vector<std::string> bootstrap_nodes;

    /* Storage */
    std::string data_dir = "./helix-data";

    /* Logging */
    std::string log_level = "info";

    bool parse(int argc, char *argv[]);
};

} // namespace helix
