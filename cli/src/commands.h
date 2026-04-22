#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chromatindb::cli::cmd {

struct ConnectOpts {
    std::string host = "127.0.0.1";
    uint16_t port = 4200;
    std::string uds_path = "/run/chromatindb/node.sock";
    // Set to true when --uds was passed explicitly on the CLI, so the host
    // resolver won't clobber it even when config.json resolves a default node.
    bool uds_path_explicit = false;
    bool quiet = false;
};

int keygen(const std::string& identity_dir, bool force);
int whoami(const std::string& identity_dir);

/// Export public key(s) in one of several formats.
/// format: "raw" (binary), "hex", or "b64".
/// out_path: if non-empty, write to file instead of stdout.
/// signing_only / kem_only: restrict to just one key; default emits both
/// (signing || kem). Mutually exclusive.
int export_key(const std::string& identity_dir, const std::string& format,
               const std::string& out_path, bool signing_only, bool kem_only);

/// extension: when `name_opt` is set, also emit a NAME blob binding
/// that name to the uploaded content blob (1..65535 bytes of opaque UTF-8 per
/// D-04). When `replace` is true AND `name_opt` is set, also emit a BOMB-of-1
/// tombstoning the prior binding's content blob — write-before-delete order
/// is content → NAME → BOMB so a partial failure never leaves deleted content
/// without a pointer. Caller must ensure exactly one content blob per call
/// when `name_opt` is set (the plan-level invariant is enforced in main.cpp
/// at the argv layer).
int put(const std::string& identity_dir, const std::vector<std::string>& file_paths,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin,
        const std::optional<std::string>& name_opt, bool replace,
        const ConnectOpts& opts);

int get(const std::string& identity_dir, const std::vector<std::string>& hash_hexes,
        const std::string& namespace_hex, bool to_stdout,
        const std::string& output_dir, bool force_overwrite, const ConnectOpts& opts);

/// D-09 NAME lookup. Enumerates NAME blobs in the namespace via
/// ListRequest+type_filter infrastructure (no new transport type),
/// reads each candidate's full BlobData, filters to entries whose NAME payload
/// name == `name`, sorts by (blob.timestamp DESC, content_hash DESC) and fetches
/// the winner's target content blob. Exit 1 if no NAME matches. Stateless on
/// every call (D-09: no local name cache).
int get_by_name(const std::string& identity_dir, const std::string& name,
                const std::string& namespace_hex, bool to_stdout,
                const std::string& output_dir, bool force_overwrite,
                const ConnectOpts& opts);

/// List all blob hashes in a namespace. Returns vector of hex strings.
std::vector<std::string> list_hashes(const std::string& identity_dir,
                                      const std::string& namespace_hex,
                                      const ConnectOpts& opts);

/// Sign and send a tombstone for the target blob.
/// Pre-checks target existence (via Exists) and surfaces DeleteAck status
/// (stored vs. already-tombstoned) unless `force` is true.
int rm(const std::string& identity_dir, const std::string& hash_hex,
       const std::string& namespace_hex, bool force, const ConnectOpts& opts);

/// D-06/D-07 multi-target rm. Given N target hashes, emits ONE
/// BOMB blob (ttl=0, signed by caller's identity) covering all N targets.
/// Separate invocations produce separate BOMBs — no cross-invocation
/// coalescing, no daemon, no state file. Zero targets is a caller error and
/// should be rejected in main.cpp before getting here (exit 2).
int rm_batch(const std::string& identity_dir,
             const std::vector<std::string>& hash_hexes,
             const std::string& namespace_hex, bool force,
             const ConnectOpts& opts);

int reshare(const std::string& identity_dir, const std::string& hash_hex,
            const std::string& namespace_hex,
            const std::vector<std::string>& share_pubkey_files,
            uint32_t ttl, const ConnectOpts& opts);

int ls(const std::string& identity_dir, const std::string& namespace_hex,
       const ConnectOpts& opts, bool raw = false,
       const std::string& type_filter = "");

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

/// Grant write access. `target` is a contact name, `@group`, or pubkey file path.
int delegate(const std::string& identity_dir, const std::string& target,
             const ConnectOpts& opts);
/// Revoke write access. `target` is a contact name, `@group`, or pubkey file path.
/// Queries existing delegations first and only prompts for real delegates.
int revoke(const std::string& identity_dir, const std::string& target,
           bool skip_confirm, const ConnectOpts& opts);
int delegations(const std::string& identity_dir, const std::string& namespace_hex,
                const ConnectOpts& opts);

} // namespace chromatindb::cli::cmd
