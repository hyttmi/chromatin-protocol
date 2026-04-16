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
        const std::string& output_dir, bool force_overwrite, const ConnectOpts& opts);

/// List all blob hashes in a namespace. Returns vector of hex strings.
std::vector<std::string> list_hashes(const std::string& identity_dir,
                                      const std::string& namespace_hex,
                                      const ConnectOpts& opts);

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

int publish(const std::string& identity_dir, const ConnectOpts& opts);
int contact_add(const std::string& identity_dir, const std::string& name,
                const std::string& namespace_hex, const ConnectOpts& opts);
int contact_rm(const std::string& identity_dir, const std::string& name);
int contact_list(const std::string& identity_dir);

// Group commands
int group_create(const std::string& identity_dir, const std::string& name);
int group_add(const std::string& identity_dir, const std::string& group,
              const std::vector<std::string>& contacts);
int group_rm(const std::string& identity_dir, const std::string& group);
int group_rm_member(const std::string& identity_dir, const std::string& group,
                    const std::string& contact);
int group_list(const std::string& identity_dir);
int group_list_members(const std::string& identity_dir, const std::string& group);

// Contact import/export
int contact_import(const std::string& identity_dir, const std::string& json_path,
                   const ConnectOpts& opts);
int contact_export(const std::string& identity_dir);

int delegate(const std::string& identity_dir, const std::string& pubkey_file,
             const ConnectOpts& opts);
int revoke(const std::string& identity_dir, const std::string& pubkey_file,
           const ConnectOpts& opts);
int delegations(const std::string& identity_dir, const std::string& namespace_hex,
                const ConnectOpts& opts);

} // namespace chromatindb::cli::cmd
