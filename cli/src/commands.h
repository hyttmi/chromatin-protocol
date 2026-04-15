#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chromatindb::cli::cmd {

struct ConnectOpts {
    std::string host = "127.0.0.1";
    uint16_t port = 4200;
    std::string uds_path = "/run/chromatindb/node.sock";
    bool quiet = false;
};

int keygen(const std::string& identity_dir, bool force);
int whoami(const std::string& identity_dir);
int export_key(const std::string& identity_dir);

int put(const std::string& identity_dir, const std::vector<std::string>& file_paths,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin, const ConnectOpts& opts);

int get(const std::string& identity_dir, const std::vector<std::string>& hash_hexes,
        const std::string& namespace_hex, bool to_stdout,
        const std::string& output_dir, const ConnectOpts& opts);

int rm(const std::string& identity_dir, const std::string& hash_hex,
       const std::string& namespace_hex, const ConnectOpts& opts);

int reshare(const std::string& identity_dir, const std::string& hash_hex,
            const std::string& namespace_hex,
            const std::vector<std::string>& share_pubkey_files,
            uint32_t ttl, const ConnectOpts& opts);

int ls(const std::string& identity_dir, const std::string& namespace_hex,
       const ConnectOpts& opts);

int exists(const std::string& identity_dir, const std::string& hash_hex,
           const std::string& namespace_hex, const ConnectOpts& opts);

int info(const std::string& identity_dir, const ConnectOpts& opts);
int stats(const std::string& identity_dir, const ConnectOpts& opts);

int delegate(const std::string& identity_dir, const std::string& pubkey_file,
             const ConnectOpts& opts);
int revoke(const std::string& identity_dir, const std::string& pubkey_file,
           const ConnectOpts& opts);
int delegations(const std::string& identity_dir, const std::string& namespace_hex,
                const ConnectOpts& opts);

} // namespace chromatindb::cli::cmd
