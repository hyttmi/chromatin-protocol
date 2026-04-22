#include "cli/src/commands.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ansicolor_sink.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char* VERSION = "1.0.0";

static fs::path default_identity_dir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        std::fprintf(stderr, "Error: HOME environment variable not set\n");
        std::exit(1);
    }
    return fs::path(home) / ".cdb";
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

static void usage(FILE* out = stderr) {
    std::fprintf(out,
        "Usage: cdb <command> [options]\n"
        "\n"
        "Commands:\n"
        "  keygen       Generate identity keypair\n"
        "  whoami       Print namespace\n"
        "  export-key   Export public keys\n"
        "  put          Upload encrypted file\n"
        "  get          Download and decrypt file\n"
        "  rm           Delete (tombstone)\n"
        "  reshare      Re-encrypt for new recipients\n"
        "  ls           List blobs (--raw for all, --type TYPE to filter)\n"
        "  exists       Check if blob exists\n"
        "  info         Node information\n"
        "  stats        Namespace statistics\n"
        "  publish      Publish pubkey to node\n"
        "  contact      Manage contacts (add/rm/list/import/export)\n"
        "  group        Manage contact groups (create/add/rm/list)\n"
        "  delegate     Grant write access to another identity\n"
        "  revoke       Revoke write access\n"
        "  delegations  List active delegations\n"
        "  version      Print version\n"
        "\n"
        "Global options:\n"
        "  --identity <path>   Identity directory (default: ~/.cdb/)\n"
        "  --node <name>       Use named node from config.json\n"
        "  --host <addr>       Target host (overrides config)\n"
        "  -p, --port <port>   Port (default: 4200)\n"
        "  --uds <path>        UDS socket path (overrides host)\n"
        "  -v, --verbose       Show info-level log output\n"
        "  -q, --quiet         Minimal output\n"
        "  -h, --help          Show this help\n"
        "\n"
        "Run `cdb <command> --help` for per-command options.\n"
    );
}

/// Check if argv[idx] is --help or -h.
static bool is_help_flag(int argc, char* argv[], int idx) {
    if (idx >= argc) return false;
    return std::strcmp(argv[idx], "--help") == 0 || std::strcmp(argv[idx], "-h") == 0;
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
    spdlog::set_level(spdlog::level::warn);

    namespace cmd = chromatindb::cli::cmd;

    std::string identity_dir_str;
    std::string node_name;
    cmd::ConnectOpts opts;
    bool cli_host_set = false;
    bool cli_port_set = false;

    // Pre-pass: extract global flags from anywhere in argv, then rebuild argv
    // with only the command + per-command args. This lets global flags appear
    // in any order -- e.g. both `cdb --node home put file` and
    // `cdb put --node home file` work identically.
    std::vector<char*> remaining;
    remaining.reserve(argc);
    remaining.push_back(argv[0]);  // Preserve program name at [0]
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        auto need_value = [&](const char* flag_name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: %s requires a value\n", flag_name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (std::strcmp(arg, "--identity") == 0) {
            identity_dir_str = need_value("--identity");
        } else if (std::strcmp(arg, "--node") == 0) {
            node_name = need_value("--node");
        } else if (std::strcmp(arg, "--uds") == 0) {
            opts.uds_path = need_value("--uds");
            opts.uds_path_explicit = true;
        } else if (std::strcmp(arg, "--host") == 0) {
            parse_host_port(need_value("--host"), opts.host, opts.port);
            cli_host_set = true;
        } else if (std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--port") == 0) {
            const char* v = need_value(arg);
            int p = std::atoi(v);
            if (p <= 0 || p > 65535) {
                std::fprintf(stderr, "Error: invalid port '%s'\n", v);
                return 1;
            }
            opts.port = static_cast<uint16_t>(p);
            cli_port_set = true;
        } else if (std::strcmp(arg, "-v") == 0 || std::strcmp(arg, "--verbose") == 0) {
            spdlog::set_level(spdlog::level::info);
        } else if (std::strcmp(arg, "-q") == 0 || std::strcmp(arg, "--quiet") == 0) {
            opts.quiet = true;
        } else {
            // Not a global flag -- keep for per-command parsing.
            // (--force is deliberately NOT global: each command interprets it
            // differently — keygen = overwrite identity, rm = tombstone
            // missing blob, get = overwrite local file — so each subcommand
            // parses it locally.)
            remaining.push_back(argv[i]);
        }
    }

    // Per-command parsing operates on the filtered argv.
    argc = static_cast<int>(remaining.size());
    argv = remaining.data();
    int arg_idx = 1;

    if (identity_dir_str.empty()) {
        identity_dir_str = default_identity_dir().string();
    }

    // Load config.json defaults (CLI flags override)
    {
        auto config_path = fs::path(identity_dir_str) / "config.json";
        if (fs::exists(config_path)) {
            try {
                std::ifstream cf(config_path);
                auto cfg = nlohmann::json::parse(cf, nullptr, false);
                if (!cfg.is_discarded() && cfg.is_object()) {
                    // Resolve named node: --node flag > default_node > legacy host/port
                    std::string resolve = node_name;
                    if (resolve.empty() && !cli_host_set &&
                        cfg.contains("default_node") && cfg["default_node"].is_string()) {
                        resolve = cfg["default_node"].get<std::string>();
                    }
                    if (!resolve.empty() && cfg.contains("nodes") && cfg["nodes"].is_object()) {
                        auto& nodes = cfg["nodes"];
                        if (nodes.contains(resolve) && nodes[resolve].is_string()) {
                            parse_host_port(nodes[resolve].get<std::string>(),
                                            opts.host, opts.port);
                            cli_host_set = true;
                        } else {
                            std::fprintf(stderr, "Error: unknown node '%s'\n", resolve.c_str());
                            return 1;
                        }
                    }
                    // Legacy flat host/port fallback
                    if (!cli_host_set && cfg.contains("host") && cfg["host"].is_string()) {
                        opts.host = cfg["host"].get<std::string>();
                    }
                    if (!cli_port_set && cfg.contains("port") && cfg["port"].is_number_unsigned()) {
                        opts.port = static_cast<uint16_t>(cfg["port"].get<unsigned>());
                    }
                }
            } catch (const std::exception& e) {
                // IN-03 fix: narrow the catch and surface the error. Project-memory
                // rule: "Don't suppress errors with || true". The UX of falling
                // back to defaults on a bad config is preserved; the SILENCING
                // was the violation.
                std::fprintf(stderr, "Warning: ignoring malformed %s: %s\n",
                             config_path.c_str(), e.what());
            }
        } else if (!node_name.empty()) {
            std::fprintf(stderr, "Error: --node requires config.json at %s\n",
                         (fs::path(identity_dir_str) / "config.json").c_str());
            return 1;
        }
    }

    // Resolve the final transport. Explicit --uds always wins: the user said
    // "talk to this socket", so honour that even if config.json also named a
    // default node. Otherwise, any remote targeting (--node, --host, -p)
    // disables the UDS probe so std::filesystem::exists() can't bail on a
    // socket the user can't stat (e.g. root-owned /run/chromatindb/node.sock).
    if (opts.uds_path_explicit) {
        // Keep opts.uds_path; Connection::connect prefers UDS when set.
    } else if (!node_name.empty() || cli_host_set || cli_port_set) {
        opts.uds_path.clear();
    }

    if (arg_idx >= argc) {
        usage();
        return 1;
    }

    const std::string command = argv[arg_idx++];

    if (command == "--help" || command == "-h" || command == "help") {
        usage(stdout);
        return 0;
    }

    try {
        // =====================================================================
        // version (no identity needed)
        // =====================================================================
        if (command == "version") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb version\n");
                return 0;
            }
            std::printf("cdb %s\n", VERSION);
            return 0;
        }

        // =====================================================================
        // keygen
        // =====================================================================
        if (command == "keygen") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb keygen [--force]\n");
                return 0;
            }
            bool keygen_force = false;
            while (arg_idx < argc) {
                if (std::strcmp(argv[arg_idx], "--force") == 0) {
                    keygen_force = true;
                } else {
                    std::fprintf(stderr, "Unknown keygen option: %s\n", argv[arg_idx]);
                    return 1;
                }
                ++arg_idx;
            }
            return cmd::keygen(identity_dir_str, keygen_force);
        }

        // =====================================================================
        // whoami
        // =====================================================================
        if (command == "whoami") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb whoami\n");
                return 0;
            }
            return cmd::whoami(identity_dir_str);
        }

        // =====================================================================
        // export-key
        // =====================================================================
        if (command == "export-key") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr,
                    "Usage: cdb export-key [--format raw|hex|b64] [--out <file>]\n"
                    "                      [--signing-only | --kem-only]\n"
                    "\n"
                    "Default: raw 4160-byte concatenation (signing || kem) to stdout.\n"
                    "When stdout is a terminal and format is raw, refuses to print\n"
                    "binary and suggests --out or redirect.\n");
                return 0;
            }
            std::string format = "raw";
            std::string out_path;
            bool signing_only = false;
            bool kem_only = false;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--format") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --format requires raw|hex|b64\n");
                        return 1;
                    }
                    format = argv[++arg_idx];
                    ++arg_idx;
                } else if (std::strcmp(a, "--out") == 0 || std::strcmp(a, "-o") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --out requires a file path\n");
                        return 1;
                    }
                    out_path = argv[++arg_idx];
                    ++arg_idx;
                } else if (std::strcmp(a, "--signing-only") == 0) {
                    signing_only = true;
                    ++arg_idx;
                } else if (std::strcmp(a, "--kem-only") == 0) {
                    kem_only = true;
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown export-key option: %s\n", a);
                    return 1;
                }
            }
            if (signing_only && kem_only) {
                std::fprintf(stderr, "Error: --signing-only and --kem-only are mutually exclusive\n");
                return 1;
            }
            return cmd::export_key(identity_dir_str, format, out_path,
                                   signing_only, kem_only);
        }

        // =====================================================================
        // put <file>...
        //   --share <name|@group|file> (repeatable)
        //   --ttl <seconds|Ns|Nm|Nh|Nd>
        //   --stdin
        // =====================================================================
        if (command == "put") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr,
                    "Usage: cdb put <file>... [--share <name|@group|file>...] [--ttl <duration>]\n"
                    "                 [--stdin] [--name <name>] [--replace]\n"
                    "\n"
                    "Options:\n"
                    "  --name <name>   Tag the uploaded blob with a mutable user-facing name.\n"
                    "                  Emits an extra NAME pointer blob. Name is 1..65535 bytes\n"
                    "                  of opaque UTF-8 (the shell quotes it).\n"
                    "  --replace       With --name: also emit a BOMB-of-1 tombstoning the\n"
                    "                  content of the prior NAME binding. No-op if there is\n"
                    "                  no prior binding. Requires --name.\n");
                return 0;
            }
            std::vector<std::string> file_paths;
            std::vector<std::string> share_files;
            uint32_t ttl = 0;
            bool from_stdin = false;
            std::string name_value;
            bool name_given = false;
            bool replace = false;

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
                } else if (std::strcmp(a, "--name") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --name requires a value\n");
                        return 1;
                    }
                    name_value = argv[++arg_idx];
                    name_given = true;
                    if (name_value.empty()) {
                        std::fprintf(stderr, "Error: --name value must be 1..65535 bytes (empty not allowed)\n");
                        return 2;
                    }
                    if (name_value.size() > 65535) {
                        std::fprintf(stderr, "Error: --name value exceeds 65535 bytes\n");
                        return 2;
                    }
                    ++arg_idx;
                } else if (std::strcmp(a, "--replace") == 0) {
                    replace = true;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    if (!fs::exists(a)) {
                        std::fprintf(stderr, "Error: file not found: %s\n", a);
                        return 1;
                    }
                    if (fs::is_directory(a)) {
                        std::fprintf(stderr, "Error: %s is a directory (put accepts files only)\n", a);
                        return 1;
                    }
                    file_paths.push_back(a);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown put option: %s\n", a);
                    return 1;
                }
            }

            if (!from_stdin && file_paths.empty()) {
                std::fprintf(stderr, "Error: put requires file path(s) or --stdin\n");
                return 1;
            }

            if (replace && !name_given) {
                std::fprintf(stderr, "Error: --replace requires --name <name>\n");
                return 2;
            }

            if (name_given && (file_paths.size() + (from_stdin ? 1 : 0)) > 1) {
                std::fprintf(stderr,
                    "Error: --name binds a single content blob; pass exactly one file\n"
                    "       (or --stdin), not a batch.\n");
                return 2;
            }

            std::optional<std::string> name_opt;
            if (name_given) name_opt = name_value;

            return cmd::put(identity_dir_str, file_paths, share_files,
                           ttl, from_stdin, name_opt, replace, opts);
        }

        // =====================================================================
        // get <name> | <hash>...
        //   --stdout
        //   --from <name|hex>
        //   -o, --output-dir <dir>
        //
        // Dispatch (Phase 123 D-09 NAME resolution): if a positional token is
        // exactly 64 hex chars we treat it as a content hash (existing batch
        // flow). Otherwise the first positional is a Phase-123 NAME — fetch
        // via cmd::get_by_name with deterministic resolution (timestamp DESC,
        // content_hash DESC tiebreak).
        // =====================================================================
        if (command == "get") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr,
                    "Usage: cdb get <name> | <hash>... [--from <name|ns>] [-o <dir>]\n"
                    "                                  [--stdout] [--all] [--force]\n"
                    "\n"
                    "Positionals:\n"
                    "  <name>      NAME lookup. Enumerates NAME blobs in the current\n"
                    "              namespace and picks the winner by (timestamp DESC,\n"
                    "              content_hash DESC). Fetches the bound content blob.\n"
                    "              Only valid as a single positional.\n"
                    "  <hash>...   One or more 64-hex content hashes (batch fetch).\n");
                return 0;
            }
            std::string namespace_hex;
            std::string output_dir;
            bool to_stdout = false;
            bool get_all = false;
            bool get_force = false;

            std::vector<std::string> hash_hexes;
            std::vector<std::string> positionals;  // raw, for name-vs-hash dispatch below
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
                } else if (std::strcmp(a, "--all") == 0) {
                    get_all = true;
                    ++arg_idx;
                } else if (std::strcmp(a, "--force") == 0) {
                    get_force = true;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    positionals.emplace_back(a);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown get option: %s\n", a);
                    return 1;
                }
            }

            // Dispatch: if the first positional is exactly 64 hex chars AND every
            // positional is, we're in batch-hash mode. Otherwise, if the first
            // positional exists and is NOT 64 hex chars, it's a NAME lookup — only
            // one positional allowed.
            auto is_64_hex = [](const std::string& s) {
                if (s.size() != 64) return false;
                for (char c : s) {
                    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                              (c >= 'A' && c <= 'F');
                    if (!ok) return false;
                }
                return true;
            };

            bool name_mode = false;
            if (!positionals.empty() && !is_64_hex(positionals[0])) {
                name_mode = true;
            }

            if (name_mode) {
                if (positionals.size() > 1) {
                    std::fprintf(stderr,
                        "Error: cdb get <name> takes a single NAME positional "
                        "(got %zu). Use <hash>... for batch fetch.\n",
                        positionals.size());
                    return 1;
                }
                if (get_all) {
                    std::fprintf(stderr, "Error: --all is not compatible with NAME lookup\n");
                    return 1;
                }
                return cmd::get_by_name(identity_dir_str, positionals[0],
                                         namespace_hex, to_stdout,
                                         output_dir, get_force, opts);
            }

            // Hash mode: every positional must be 64 hex chars.
            for (auto& p : positionals) {
                if (!is_64_hex(p)) {
                    std::fprintf(stderr, "Error: invalid hash: %s\n", p.c_str());
                    return 1;
                }
                hash_hexes.emplace_back(std::move(p));
            }

            if (get_all) {
                if (namespace_hex.empty()) {
                    std::fprintf(stderr, "Error: --all requires --from <name|namespace>\n");
                    return 1;
                }
                // List the namespace, collect all hashes, then get them
                auto all_hashes = cmd::list_hashes(identity_dir_str, namespace_hex, opts);
                if (all_hashes.empty()) {
                    std::fprintf(stderr, "No blobs in namespace\n");
                    return 0;
                }
                // Merge with any explicitly provided hashes
                for (auto& h : hash_hexes) {
                    all_hashes.push_back(std::move(h));
                }
                return cmd::get(identity_dir_str, all_hashes, namespace_hex,
                               to_stdout, output_dir, get_force, opts);
            }

            if (hash_hexes.empty()) {
                std::fprintf(stderr, "Error: get requires at least one blob hash, a NAME, or --all --from <ns>\n");
                return 1;
            }

            return cmd::get(identity_dir_str, hash_hexes, namespace_hex,
                           to_stdout, output_dir, get_force, opts);
        }

        // =====================================================================
        // rm <hash>...
        //   --namespace <name|hex>
        //   -y / --yes
        //   --force (bypass target-existence check)
        //
        // D-06/D-07: ONE invocation emits ONE BOMB regardless of target
        // count. Separate shell invocations produce separate BOMBs — no daemon,
        // no cross-invocation coalescing. ≥1 target: one BOMB. Zero targets:
        // usage error (exit 2).
        // =====================================================================
        if (command == "rm") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr,
                    "Usage: cdb rm <hash>... [--namespace <name|hex>] [-y] [--force]\n"
                    "\n"
                    "Multiple targets in a single invocation are amortized into ONE\n"
                    "batched BOMB tombstone. Exit 2 if no targets given.\n");
                return 0;
            }
            std::string namespace_hex;
            bool skip_confirm = false;
            bool rm_force = false;
            std::vector<std::string> hash_hexes;  // target_hashes (per acceptance grep)

            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a name or hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (std::strcmp(a, "-y") == 0 || std::strcmp(a, "--yes") == 0) {
                    skip_confirm = true;
                    ++arg_idx;
                } else if (std::strcmp(a, "--force") == 0) {
                    rm_force = true;
                    ++arg_idx;
                } else if (a[0] != '-') {
                    if (std::strlen(a) != 64) {
                        std::fprintf(stderr, "Error: invalid hash (expected 64 hex chars): %s\n", a);
                        return 1;
                    }
                    hash_hexes.emplace_back(a);
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown rm option: %s\n", a);
                    return 1;
                }
            }

            if (hash_hexes.empty()) {
                std::fprintf(stderr, "Error: rm requires at least one blob hash (no targets)\n");
                return 2;
            }

            if (!skip_confirm) {
                if (hash_hexes.size() == 1) {
                    std::fprintf(stderr, "Delete blob %s? [y/N] ", hash_hexes[0].c_str());
                } else {
                    std::fprintf(stderr, "Delete %zu blobs via one BOMB tombstone? [y/N] ",
                                 hash_hexes.size());
                }
                int ch = std::fgetc(stdin);
                if (ch != 'y' && ch != 'Y') {
                    std::fprintf(stderr, "Aborted.\n");
                    return 0;
                }
            }

            return cmd::rm_batch(identity_dir_str, hash_hexes, namespace_hex, rm_force, opts);
        }

        // =====================================================================
        // reshare <hash>
        //   --share <name|@group|file> (repeatable)
        //   --ttl <seconds>
        //   --namespace <name|hex>
        // =====================================================================
        if (command == "reshare") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb reshare <hash> [--share <name|@group|file>...] [--ttl <duration>] [--namespace <name|hex>]\n");
                return 0;
            }
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
        // ls
        //   --namespace <name|hex>  --raw  --type <TYPE>
        // =====================================================================
        if (command == "ls") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr,
                    "Usage: cdb ls [--namespace <name|hex>] [--raw] [--type <TYPE>]\n"
                    "\n"
                    "Options:\n"
                    "  --namespace, -n <name|hex>  Filter by namespace (contact name or 64-hex)\n"
                    "  --raw                       Show all blobs including infrastructure types\n"
                    "  --type <TYPE>               Filter by type: CENV, PUBK, TOMB, DLGT, CDAT, CPAR, NAME, BOMB\n");
                return 0;
            }
            std::string namespace_hex;
            bool raw = false;
            std::string type_filter;

            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a name or hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (std::strcmp(a, "--raw") == 0) {
                    raw = true;
                    ++arg_idx;
                } else if (std::strcmp(a, "--type") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --type requires a type name (CENV, PUBK, TOMB, DLGT, CDAT, CPAR, NAME, BOMB)\n");
                        return 1;
                    }
                    type_filter = argv[++arg_idx];
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown ls option: %s\n", a);
                    return 1;
                }
            }

            return cmd::ls(identity_dir_str, namespace_hex, opts, raw, type_filter);
        }

        // =====================================================================
        // exists <hash>
        //   --namespace <name|hex>
        // =====================================================================
        if (command == "exists") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb exists <hash> [--namespace <name|hex>]\n");
                return 0;
            }
            std::string hash_hex;
            std::string namespace_hex;

            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a name or hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else if (a[0] != '-' && hash_hex.empty()) {
                    hash_hex = a;
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
        // info
        // =====================================================================
        if (command == "info") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb info\n");
                return 0;
            }
            if (arg_idx < argc) {
                std::fprintf(stderr, "Unknown info option: %s\n", argv[arg_idx]);
                return 1;
            }
            return cmd::info(identity_dir_str, opts);
        }

        // =====================================================================
        // stats
        // =====================================================================
        if (command == "stats") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb stats\n");
                return 0;
            }
            if (arg_idx < argc) {
                std::fprintf(stderr, "Unknown stats option: %s\n", argv[arg_idx]);
                return 1;
            }
            return cmd::stats(identity_dir_str, opts);
        }

        // =====================================================================
        // delegate <name|@group|pubkey_file>
        // =====================================================================
        if (command == "delegate") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb delegate <name|@group|pubkey_file>\n");
                return 0;
            }
            std::string target;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (a[0] != '-' && target.empty()) {
                    target = a;
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown delegate option: %s\n", a);
                    return 1;
                }
            }
            if (target.empty()) {
                std::fprintf(stderr, "Error: delegate requires a contact name, @group, or pubkey file\n");
                return 1;
            }
            return cmd::delegate(identity_dir_str, target, opts);
        }

        // =====================================================================
        // revoke <name|@group|pubkey_file>
        //   -y / --yes
        // =====================================================================
        if (command == "revoke") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb revoke <name|@group|pubkey_file> [-y]\n");
                return 0;
            }
            std::string target;
            bool skip_confirm = false;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "-y") == 0 || std::strcmp(a, "--yes") == 0) {
                    skip_confirm = true;
                    ++arg_idx;
                } else if (a[0] != '-' && target.empty()) {
                    target = a;
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown revoke option: %s\n", a);
                    return 1;
                }
            }
            if (target.empty()) {
                std::fprintf(stderr, "Error: revoke requires a contact name, @group, or pubkey file\n");
                return 1;
            }

            return cmd::revoke(identity_dir_str, target, skip_confirm, opts);
        }

        // =====================================================================
        // delegations
        //   --namespace <name|hex>
        // =====================================================================
        if (command == "delegations") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb delegations [--namespace <name|hex>]\n");
                return 0;
            }
            std::string namespace_hex;
            while (arg_idx < argc) {
                const char* a = argv[arg_idx];
                if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
                    if (arg_idx + 1 >= argc) {
                        std::fprintf(stderr, "Error: --namespace requires a name or hex value\n");
                        return 1;
                    }
                    namespace_hex = argv[++arg_idx];
                    ++arg_idx;
                } else {
                    std::fprintf(stderr, "Unknown delegations option: %s\n", a);
                    return 1;
                }
            }
            return cmd::delegations(identity_dir_str, namespace_hex, opts);
        }

        // =====================================================================
        // publish
        // =====================================================================
        if (command == "publish") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb publish\n");
                return 0;
            }
            if (arg_idx < argc) {
                std::fprintf(stderr, "Unknown publish option: %s\n", argv[arg_idx]);
                return 1;
            }
            return cmd::publish(identity_dir_str, opts);
        }

        // =====================================================================
        // contact add <name> <namespace_hex> [host[:port]]
        // contact rm <name>
        // contact list
        // =====================================================================
        if (command == "contact") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb contact add <name> <namespace>\n"
                             "       cdb contact rm <name>\n"
                             "       cdb contact list\n"
                             "       cdb contact import <file.json>\n"
                             "       cdb contact export\n");
                return 0;
            }
            if (arg_idx >= argc) {
                std::fprintf(stderr, "Usage: contact <add|rm|list|import|export> ...\n");
                return 1;
            }
            std::string subcmd = argv[arg_idx++];

            if (subcmd == "add") {
                if (arg_idx + 1 >= argc) {
                    std::fprintf(stderr, "Usage: contact add <name> <namespace_hex>\n");
                    return 1;
                }
                std::string name = argv[arg_idx++];
                std::string ns_hex = argv[arg_idx++];
                if (arg_idx < argc) {
                    std::fprintf(stderr, "Error: unexpected argument: %s\n", argv[arg_idx]);
                    return 1;
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

            if (subcmd == "import") {
                if (arg_idx >= argc) {
                    std::fprintf(stderr, "Usage: contact import <file.json>\n");
                    return 1;
                }
                std::string json_path = argv[arg_idx++];
                if (arg_idx < argc) {
                    std::fprintf(stderr, "Error: unexpected argument: %s\n", argv[arg_idx]);
                    return 1;
                }
                return cmd::contact_import(identity_dir_str, json_path, opts);
            }

            if (subcmd == "export") {
                return cmd::contact_export(identity_dir_str);
            }

            std::fprintf(stderr, "Unknown contact subcommand: %s\n", subcmd.c_str());
            return 1;
        }

        // =====================================================================
        // group create <name>
        // group add <group> <contact>...
        // group rm <group> [<contact>]
        // group list [<group>]
        // =====================================================================
        if (command == "group") {
            if (is_help_flag(argc, argv, arg_idx)) {
                std::fprintf(stderr, "Usage: cdb group create <name>\n"
                             "       cdb group add <group> <contact>...\n"
                             "       cdb group rm <group> [<contact>]\n"
                             "       cdb group list [<group>]\n");
                return 0;
            }
            if (arg_idx >= argc) {
                std::fprintf(stderr, "Usage: group <create|add|rm|list> ...\n");
                return 1;
            }
            std::string subcmd = argv[arg_idx++];

            if (subcmd == "create") {
                if (arg_idx >= argc) {
                    std::fprintf(stderr, "Usage: group create <name>\n");
                    return 1;
                }
                return cmd::group_create(identity_dir_str, argv[arg_idx]);
            }

            if (subcmd == "add") {
                if (arg_idx + 1 >= argc) {
                    std::fprintf(stderr, "Usage: group add <group> <contact>...\n");
                    return 1;
                }
                std::string group = argv[arg_idx++];
                std::vector<std::string> contacts;
                while (arg_idx < argc) {
                    contacts.push_back(argv[arg_idx++]);
                }
                return cmd::group_add(identity_dir_str, group, contacts);
            }

            if (subcmd == "rm") {
                if (arg_idx >= argc) {
                    std::fprintf(stderr, "Usage: group rm <group> [<contact>]\n");
                    return 1;
                }
                std::string group = argv[arg_idx++];
                if (arg_idx < argc) {
                    return cmd::group_rm_member(identity_dir_str, group, argv[arg_idx]);
                }
                return cmd::group_rm(identity_dir_str, group);
            }

            if (subcmd == "list") {
                if (arg_idx < argc) {
                    return cmd::group_list_members(identity_dir_str, argv[arg_idx]);
                }
                return cmd::group_list(identity_dir_str);
            }

            std::fprintf(stderr, "Unknown group subcommand: %s\n", subcmd.c_str());
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
