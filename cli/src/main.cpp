#include "cli/src/identity.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static const char* VERSION = "0.1.0";

static std::string to_hex(std::span<const uint8_t> data) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t b : data) {
        out.push_back(hex_chars[b >> 4]);
        out.push_back(hex_chars[b & 0x0f]);
    }
    return out;
}

static fs::path default_identity_dir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }
    return fs::path(home) / ".chromatindb";
}

static void usage() {
    std::fprintf(stderr,
        "Usage: chromatindb-cli [--identity <path>] <command>\n"
        "\n"
        "Commands:\n"
        "  keygen      Generate a new identity\n"
        "  whoami      Print namespace of current identity\n"
        "  export-key  Write public keys to stdout\n"
        "  version     Print version\n"
        "\n"
        "Options:\n"
        "  --identity <path>  Identity directory (default: ~/.chromatindb/)\n"
    );
}

static int cmd_keygen(const fs::path& identity_dir) {
    if (fs::exists(identity_dir / "identity.key")) {
        std::fprintf(stderr, "Identity already exists at %s\n", identity_dir.c_str());
        std::fprintf(stderr, "Remove it first if you want to regenerate.\n");
        return 1;
    }

    auto id = chromatindb::cli::Identity::generate();
    id.save_to(identity_dir);

    std::printf("%s\n", to_hex(id.namespace_id()).c_str());
    return 0;
}

static int cmd_whoami(const fs::path& identity_dir) {
    auto id = chromatindb::cli::Identity::load_from(identity_dir);
    std::printf("%s\n", to_hex(id.namespace_id()).c_str());
    return 0;
}

static int cmd_export_key(const fs::path& identity_dir) {
    auto id = chromatindb::cli::Identity::load_from(identity_dir);
    auto keys = id.export_public_keys();
    std::cout.write(reinterpret_cast<const char*>(keys.data()),
                    static_cast<std::streamsize>(keys.size()));
    std::cout.flush();
    return 0;
}

int main(int argc, char* argv[]) {
    fs::path identity_dir;
    int arg_idx = 1;

    // Parse global options
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (std::strcmp(argv[arg_idx], "--identity") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: --identity requires a path argument\n");
                return 1;
            }
            identity_dir = argv[arg_idx + 1];
            arg_idx += 2;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[arg_idx]);
            usage();
            return 1;
        }
    }

    if (identity_dir.empty()) {
        identity_dir = default_identity_dir();
    }

    if (arg_idx >= argc) {
        usage();
        return 1;
    }

    const std::string command = argv[arg_idx];

    try {
        if (command == "keygen") {
            return cmd_keygen(identity_dir);
        } else if (command == "whoami") {
            return cmd_whoami(identity_dir);
        } else if (command == "export-key") {
            return cmd_export_key(identity_dir);
        } else if (command == "version") {
            std::printf("chromatindb-cli %s\n", VERSION);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown command: %s\n", command.c_str());
            usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
