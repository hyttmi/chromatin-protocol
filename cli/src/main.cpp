#include "cli/src/commands.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ansicolor_sink.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char* VERSION = "0.1.0";

static fs::path default_identity_dir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        std::fprintf(stderr, "Error: HOME environment variable not set\n");
        std::exit(1);
    }
    return fs::path(home) / ".chromatindb";
}

/// Parse TTL string: plain seconds, or suffix with s/m/h/d.
/// Examples: "3600", "1h", "30m", "7d", "90s"
static uint32_t parse_ttl(const char* s) {
    char* end = nullptr;
    long val = std::strtol(s, &end, 10);
    if (val < 0) val = 0;
    if (end && *end) {
        switch (*end) {
            case 's': break;
            case 'm': val *= 60; break;
            case 'h': val *= 3600; break;
            case 'd': val *= 86400; break;
            default:
                std::fprintf(stderr, "Error: invalid TTL suffix '%c' (use s/m/h/d)\n", *end);
                std::exit(1);
        }
    }
    return static_cast<uint32_t>(val);
}

static void usage() {
    std::fprintf(stderr,
        "Usage: chromatindb-cli <command> [options]\n"
        "\n"
        "Commands:\n"
        "  keygen       Generate identity keypair\n"
        "  whoami       Print namespace\n"
        "  export-key   Export public keys\n"
        "  put          Upload encrypted file\n"
        "  get          Download and decrypt file\n"
        "  rm           Delete (tombstone)\n"
        "  reshare      Re-encrypt for new recipients\n"
        "  ls           List blobs in namespace\n"
        "  exists       Check if blob exists\n"
        "  info         Node information\n"
        "  stats        Namespace statistics\n"
        "  publish      Publish pubkey to node\n"
        "  contact      Manage contacts (add/rm/list)\n"
        "  delegate     Grant write access to another identity\n"
        "  revoke       Revoke write access\n"
        "  delegations  List active delegations\n"
        "  version      Print version\n"
        "\n"
        "Global options:\n"
        "  --identity <path>   Identity directory (default: ~/.chromatindb/)\n"
        "  --uds <path>        UDS socket path\n"
        "  -p, --port <port>   Port (default: 4200)\n"
        "  -q, --quiet         Minimal output\n"
    );
}

/// Parse "host:port" or just "host" (default port 4200).
static void parse_host_port(const std::string& arg,
                            std::string& host, uint16_t& port) {
    auto colon = arg.rfind(':');
    if (colon != std::string::npos) {
        host = arg.substr(0, colon);
        int p = std::atoi(arg.substr(colon + 1).c_str());
        if (p <= 0 || p > 65535) {
            std::fprintf(stderr, "Error: invalid port in '%s'\n", arg.c_str());
            std::exit(1);
        }
        port = static_cast<uint16_t>(p);
    } else {
        host = arg;
    }
}

int main(int argc, char* argv[]) {
    // All log output to stderr so stdout is clean for data/hashes
    auto stderr_sink = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("cli", stderr_sink));

    namespace cmd = chromatindb::cli::cmd;

    std::string identity_dir_str;
    cmd::ConnectOpts opts;
    bool force = false;
    int arg_idx = 1;

    // Parse global options (before command)
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        const char* arg = argv[arg_idx];

        if (std::strcmp(arg, "--identity") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: --identity requires a path\n");
                return 1;
            }
            identity_dir_str = argv[++arg_idx];
            ++arg_idx;
        } else if (std::strcmp(arg, "--uds") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: --uds requires a path\n");
                return 1;
            }
            opts.uds_path = argv[++arg_idx];
            ++arg_idx;
        } else if (std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--port") == 0) {
            if (arg_idx + 1 >= argc) {
                std::fprintf(stderr, "Error: %s requires a port number\n", arg);
                return 1;
            }
            int p = std::atoi(argv[++arg_idx]);
            if (p <= 0 || p > 65535) {
                std::fprintf(stderr, "Error: invalid port\n");
                return 1;
            }
            opts.port = static_cast<uint16_t>(p);
            ++arg_idx;
        } else if (std::strcmp(arg, "-q") == 0 || std::strcmp(arg, "--quiet") == 0) {
            opts.quiet = true;
            ++arg_idx;
        } else if (std::strcmp(arg, "--force") == 0) {
            force = true;
            ++arg_idx;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", arg);
            usage();
            return 1;
        }
    }

    if (identity_dir_str.empty()) {
        identity_dir_str = default_identity_dir().string();
    }

    if (arg_idx >= argc) {
        usage();
        return 1;
    }

    const std::string command = argv[arg_idx++];

    try {
        // =====================================================================
        // version (no identity needed)
        // =====================================================================
        if (command == "version") {
            std::printf("chromatindb-cli %s\n", VERSION);
            return 0;
        }

        // =====================================================================
        // keygen
        // =====================================================================
        if (command == "keygen") {
            while (arg_idx < argc) {
                if (std::strcmp(argv[arg_idx], "--force") == 0) {
                    force = true;
                } else {
                    std::fprintf(stderr, "Unknown keygen option: %s\n", argv[arg_idx]);
                    return 1;
                }
                ++arg_idx;
            }
            return cmd::keygen(identity_dir_str, force);
        }

        // =====================================================================
        // whoami
        // =====================================================================
        if (command == "whoami") {
            return cmd::whoami(identity_dir_str);
        }

        // =====================================================================
        // export-key
        // =====================================================================
        if (command == "export-key") {
            return cmd::export_key(identity_dir_str);
        }

        // =====================================================================
        // put <file>... [host[:port]]
        //   --share <pubkey_file> (repeatable)
        //   --ttl <seconds|Ns|Nm|Nh|Nd>
        //   --stdin
        // =====================================================================
        if (command == "put") {
            std::vector<std::string> file_paths;
            std::vector<std::string> share_files;
            uint32_t ttl = 0;
            bool from_stdin = false;

            // Collect all args, defer host[:port] detection
            std::vector<std::string> positionals;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--share") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --share requires a file path\n");
                        return 1;
                    }
                    share_files.push_back(argv[++arg_idx]);
                    ++arg_idx;
                } else if (std::strcmp(a, "--ttl") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --ttl requires a value\n");
                        return 1;
                    }
                    ttl = parse_ttl(argv[++arg_idx]);
                    ++arg_idx;
                } else if (std::strcmp(a, "--stdin") == 0) {
                    from_stdin = true;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    positionals.push_back(a);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown put option: %s\n", a);
                    return 1;
                }
            }

            // Last positional that looks like host[:port] (contains : or no . in path)
            // is the target. Everything else is a file path.
            for (size_t i = 0; i < positionals.size(); ++i) {
                // If it's an existing file, it's a file path
                if (fs::exists(positionals[i])) {
                    file_paths.push_back(positionals[i]);
                } else if (i == positionals.size() - 1) {
                    // Last non-file positional = host[:port]
                    parse_host_port(positionals[i].c_str(), opts.host, opts.port);
                } else {
                    std::fprintf(stderr, "Error: file not found: %s\n", positionals[i].c_str());
                    return 1;
                }
            }

            if (!from_stdin && file_paths.empty()) {
                std::fprintf(stderr, "Error: put requires file path(s) or --stdin\n");
                return 1;
            }

            return cmd::put(identity_dir_str, file_paths, share_files,
                           ttl, from_stdin, opts);
        }

        // =====================================================================
        // get <hash>... [host[:port]]
        //   --stdout
        //   --namespace <hex>
        //   -o, --output-dir <dir>
        // =====================================================================
        if (command == "get") {
            std::string namespace_hex;
            std::string output_dir;
            bool to_stdout = false;

            std::vector<std::string> positionals;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--stdout") == 0) {
                    to_stdout = true;
                    ++arg_idx;
                } else if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (std::strcmp(a, "--output-dir") == 0 || std::strcmp(a, "-o") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --output-dir requires a path\n");
                        return 1;
                    }
                    output_dir = argv[++arg_idx];
                    ++arg_idx;
                } else if (a[0] != '-') {
                    positionals.push_back(a);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown get option: %s\n", a);
                    return 1;
                }
            }

            // Hashes are 64 hex chars. Last positional that isn't a hash = host[:port]
            std::vector<std::string> hash_hexes;
            for (size_t i = 0; i < positionals.size(); ++i) {
                if (positionals[i].size() == 64) {
                    hash_hexes.push_back(positionals[i]);
                } else if (i == positionals.size() - 1) {
                    parse_host_port(positionals[i].c_str(), opts.host, opts.port);
                } else {
                    std::fprintf(stderr, "Error: invalid hash: %s\n", positionals[i].c_str());
                    return 1;
                }
            }

            if (hash_hexes.empty()) {
                std::fprintf(stderr, "Error: get requires at least one blob hash\n");
                return 1;
            }

            return cmd::get(identity_dir_str, hash_hexes, namespace_hex,
                           to_stdout, output_dir, opts);
        }

        // =====================================================================
        // rm <hash> [host[:port]]
        //   --namespace <hex>
        // =====================================================================
        if (command == "rm") {
            std::string hash_hex;
            std::string namespace_hex;

            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (a[0] != '-' && hash_hex.empty()) {
                    hash_hex = a;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown rm option: %s\n", a);
                    return 1;
                }
            }

            if (hash_hex.empty()) {
                std::fprintf(stderr, "Error: rm requires a blob hash\n");
                return 1;
            }

            return cmd::rm(identity_dir_str, hash_hex, namespace_hex, opts);
        }

        // =====================================================================
        // reshare <hash> [host[:port]]
        //   --share <pubkey_file> (repeatable)
        //   --ttl <seconds>
        //   --namespace <hex>
        // =====================================================================
        if (command == "reshare") {
            std::string hash_hex;
            std::string namespace_hex;
            std::vector<std::string> share_files;
            uint32_t ttl = 0;

            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--share") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --share requires a file path\n");
                        return 1;
                    }
                    share_files.push_back(argv[++arg_idx]);
                    ++arg_idx;
                } else if (std::strcmp(a, "--ttl") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --ttl requires a value\n");
                        return 1;
                    }
                    ttl = parse_ttl(argv[++arg_idx]);
                    ++arg_idx;
                } else if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (a[0] != '-' && hash_hex.empty()) {
                    hash_hex = a;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown reshare option: %s\n", a);
                    return 1;
                }
            }

            if (hash_hex.empty()) {
                std::fprintf(stderr, "Error: reshare requires a blob hash\n");
                return 1;
            }

            return cmd::reshare(identity_dir_str, hash_hex, namespace_hex,
                               share_files, ttl, opts);
        }

        // =====================================================================
        // ls [host[:port]]
        //   --namespace <hex>
        // =====================================================================
        if (command == "ls") {
            std::string namespace_hex;

            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown ls option: %s\n", a);
                    return 1;
                }
            }

            return cmd::ls(identity_dir_str, namespace_hex, opts);
        }

        // =====================================================================
        // exists <hash> [host[:port]]
        //   --namespace <hex>
        // =====================================================================
        if (command == "exists") {
            std::string hash_hex;
            std::string namespace_hex;

            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (a[0] != '-' && hash_hex.empty()) {
                    hash_hex = a;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown exists option: %s\n", a);
                    return 1;
                }
            }

            if (hash_hex.empty()) {
                std::fprintf(stderr, "Error: exists requires a blob hash\n");
                return 1;
            }

            return cmd::exists(identity_dir_str, hash_hex, namespace_hex, opts);
        }

        // =====================================================================
        // info [host[:port]]
        // =====================================================================
        if (command == "info") {
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown info option: %s\n", a);
                    return 1;
                }
            }
            return cmd::info(identity_dir_str, opts);
        }

        // =====================================================================
        // stats [host[:port]]
        // =====================================================================
        if (command == "stats") {
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown stats option: %s\n", a);
                    return 1;
                }
            }
            return cmd::stats(identity_dir_str, opts);
        }

        // =====================================================================
        // delegate <pubkey_file> [host[:port]]
        // =====================================================================
        if (command == "delegate") {
            std::string pubkey_file;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (a[0] != '-' && pubkey_file.empty()) {
                    pubkey_file = a;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown delegate option: %s\n", a);
                    return 1;
                }
            }
            if (pubkey_file.empty()) {
                std::fprintf(stderr, "Error: delegate requires a pubkey file\n");
                return 1;
            }
            return cmd::delegate(identity_dir_str, pubkey_file, opts);
        }

        // =====================================================================
        // revoke <pubkey_file> [host[:port]]
        // =====================================================================
        if (command == "revoke") {
            std::string pubkey_file;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (a[0] != '-' && pubkey_file.empty()) {
                    pubkey_file = a;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown revoke option: %s\n", a);
                    return 1;
                }
            }
            if (pubkey_file.empty()) {
                std::fprintf(stderr, "Error: revoke requires a pubkey file\n");
                return 1;
            }
            return cmd::revoke(identity_dir_str, pubkey_file, opts);
        }

        // =====================================================================
        // delegations [host[:port]]
        //   --namespace <hex>
        // =====================================================================
        if (command == "delegations") {
            std::string namespace_hex;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown delegations option: %s\n", a);
                    return 1;
                }
            }
            return cmd::delegations(identity_dir_str, namespace_hex, opts);
        }

        // =====================================================================
        // publish [host[:port]]
        // =====================================================================
        if (command == "publish") {
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (a[0] != '-') {
                    parse_host_port(a, opts.host, opts.port);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown publish option: %s\n", a);
                    return 1;
                }
            }
            return cmd::publish(identity_dir_str, opts);
        }

        // =====================================================================
        // contact add <name> <namespace_hex> [host[:port]]
        // contact rm <name>
        // contact list
        // =====================================================================
        if (command == "contact") {
            if (arg_idx >= argc) {
                std::fprintf(stderr, "Usage: contact <add|rm|list> ...\n");
                return 1;
            }
            std::string subcmd = argv[arg_idx++];

            if (subcmd == "add") {
                if (arg_idx + 1 >= argc) {
                    std::fprintf(stderr, "Usage: contact add <name> <namespace_hex> [host[:port]]\n");
                    return 1;
                }
                std::string name = argv[arg_idx++];
                std::string ns_hex = argv[arg_idx++];
                while (arg_idx < argc) {
                    parse_host_port(argv[arg_idx++], opts.host, opts.port);
                }
                return cmd::contact_add(identity_dir_str, name, ns_hex, opts);
            }

            if (subcmd == "rm") {
                if (arg_idx >= argc) {
                    std::fprintf(stderr, "Usage: contact rm <name>\n");
                    return 1;
                }
                return cmd::contact_rm(identity_dir_str, argv[arg_idx]);
            }

            if (subcmd == "list") {
                return cmd::contact_list(identity_dir_str);
            }

            std::fprintf(stderr, "Unknown contact subcommand: %s\n", subcmd.c_str());
            return 1;
        }

        // =====================================================================
        // Unknown
        // =====================================================================
        std::fprintf(stderr, "Unknown command: %s\n", command.c_str());
        usage();
        return 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
