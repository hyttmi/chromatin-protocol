#include "cli/src/commands.h"
#include "cli/src/chunked.h"
#include "cli/src/commands_internal.h"
#include "cli/src/connection.h"
#include "cli/src/contacts.h"
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/pubk_presence.h"
#include "cli/src/wire.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <unistd.h>

namespace chromatindb::cli::cmd {

namespace fs = std::filesystem;

// =============================================================================
// Helpers
// =============================================================================

static std::vector<uint8_t> read_file_bytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static std::vector<uint8_t> read_stdin_bytes() {
    std::cin >> std::noskipws;
    return {std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>()};
}

/// Resolve namespace: if hex provided, parse it; otherwise use identity's own.
static std::array<uint8_t, 32> resolve_namespace(
    const Identity& id, const std::string& namespace_or_name,
    const std::string& identity_dir = "") {

    if (namespace_or_name.empty()) {
        std::array<uint8_t, 32> ns{};
        auto span = id.namespace_id();
        std::memcpy(ns.data(), span.data(), 32);
        return ns;
    }

    // If it's 64 hex chars, treat as namespace hex
    if (namespace_or_name.size() == 64) {
        auto bytes = from_hex(namespace_or_name);
        if (bytes && bytes->size() == 32) {
            std::array<uint8_t, 32> ns{};
            std::memcpy(ns.data(), bytes->data(), 32);
            return ns;
        }
    }

    // Try contact name lookup
    if (!identity_dir.empty()) {
        auto db_path = identity_dir + "/contacts.db";
        if (fs::exists(db_path)) {
            ContactDB db(db_path);
            auto contact = db.get(namespace_or_name);
            if (contact) {
                auto bytes = from_hex(contact->namespace_hex);
                if (bytes && bytes->size() == 32) {
                    std::array<uint8_t, 32> ns{};
                    std::memcpy(ns.data(), bytes->data(), 32);
                    return ns;
                }
            }
        }
    }

    // Fall back to treating as hex
    auto bytes = from_hex(namespace_or_name);
    if (!bytes || bytes->size() != 32) {
        throw std::runtime_error("Invalid namespace hex (expected 64 hex chars)");
    }
    std::array<uint8_t, 32> ns{};
    std::memcpy(ns.data(), bytes->data(), 32);
    return ns;
}

/// Parse a 32-byte hash from hex string.
static std::array<uint8_t, 32> parse_hash(const std::string& hash_hex) {
    auto bytes = from_hex(hash_hex);
    if (!bytes || bytes->size() != 32) {
        throw std::runtime_error("Invalid hash hex (expected 64 hex chars)");
    }
    std::array<uint8_t, 32> hash{};
    std::memcpy(hash.data(), bytes->data(), 32);
    return hash;
}

// ListResponse entry stride: hash:32 + seq:8BE + type:4 + size:8BE + ts:8BE = 60 bytes.
// Kept as a named constant so every consumer references the same value.
static constexpr size_t LIST_ENTRY_SIZE = 60;

/// Format a byte count as "<humanized> (<raw> bytes)" for operator-facing output.
/// Uses binary units (KiB/MiB/GiB) because everything in this project is block/
/// page aligned. Returns the raw count unchanged below 1 KiB.
static std::string humanize_bytes(uint64_t n) {
    char buf[64];
    if (n < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu bytes", (unsigned long long)n);
        return buf;
    }
    constexpr const char* units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(n);
    int u = -1;
    do {
        v /= 1024.0;
        ++u;
    } while (v >= 1024.0 && u + 1 < static_cast<int>(sizeof(units) / sizeof(units[0])) - 1);
    std::snprintf(buf, sizeof(buf), "%.2f %s (%llu bytes)", v, units[u], (unsigned long long)n);
    return buf;
}

/// Format an uptime in seconds as "1d2h3m4s" (dropping leading zero units).
static std::string humanize_uptime(uint64_t secs) {
    uint64_t d = secs / 86400;
    uint64_t h = (secs % 86400) / 3600;
    uint64_t m = (secs % 3600) / 60;
    uint64_t s = secs % 60;
    char buf[64];
    if (d)      std::snprintf(buf, sizeof(buf), "%llud%lluh%llum%llus",
                              (unsigned long long)d, (unsigned long long)h,
                              (unsigned long long)m, (unsigned long long)s);
    else if (h) std::snprintf(buf, sizeof(buf), "%lluh%llum%llus",
                              (unsigned long long)h, (unsigned long long)m,
                              (unsigned long long)s);
    else if (m) std::snprintf(buf, sizeof(buf), "%llum%llus",
                              (unsigned long long)m, (unsigned long long)s);
    else        std::snprintf(buf, sizeof(buf), "%llus", (unsigned long long)s);
    return buf;
}

/// Locate the PUBK blob in a namespace using the server-side type filter
/// (flags bit 1, payload bytes 45-48 = PUBKEY_MAGIC). Returns the decoded blob
/// or nullopt if the namespace has no PUBK. Uses request IDs 1 + 2 on `conn`.
static std::optional<BlobData> find_pubkey_blob(
    Connection& conn, std::span<const uint8_t, 32> ns) {

    std::vector<uint8_t> list_payload(49, 0);
    std::memcpy(list_payload.data(), ns.data(), 32);
    // since_seq = 0 at offset 32 (zero-filled)
    store_u32_be(list_payload.data() + 40, 100);
    list_payload[44] = 0x02;  // flag: type_filter present
    std::memcpy(list_payload.data() + 45, PUBKEY_MAGIC.data(), 4);

    if (!conn.send(MsgType::ListRequest, list_payload, 1)) return std::nullopt;

    auto list_resp = conn.recv();
    if (!list_resp ||
        list_resp->type != static_cast<uint8_t>(MsgType::ListResponse) ||
        list_resp->payload.size() < 5) {
        return std::nullopt;
    }

    uint32_t count = load_u32_be(list_resp->payload.data());
    if (count == 0) return std::nullopt;

    size_t entries_size = static_cast<size_t>(count) * LIST_ENTRY_SIZE;
    if (list_resp->payload.size() < 4 + entries_size + 1) return std::nullopt;

    auto hash_span = std::span<const uint8_t>(list_resp->payload.data() + 4, 32);

    std::vector<uint8_t> read_payload(64);
    std::memcpy(read_payload.data(), ns.data(), 32);
    std::memcpy(read_payload.data() + 32, hash_span.data(), 32);

    if (!conn.send(MsgType::ReadRequest, read_payload, 2)) return std::nullopt;

    auto read_resp = conn.recv();
    if (!read_resp ||
        read_resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
        read_resp->payload.empty() ||
        read_resp->payload[0] != 0x01) {
        return std::nullopt;
    }

    auto blob = decode_blob(std::span<const uint8_t>(
        read_resp->payload.data() + 1, read_resp->payload.size() - 1));
    if (!blob || !is_pubkey_blob(blob->data)) return std::nullopt;
    return blob;
}

} // namespace chromatindb::cli::cmd

namespace chromatindb::cli {

// D-05: `decode_error_response` definition MOVED to cli/src/error_decoder.cpp
// so the [error_decoder] unit TEST_CASE in cli/tests/test_wire.cpp can link
// against it without pulling in the rest of commands.cpp's asio/spdlog/json
// dependencies. Declaration stays in cli/src/commands_internal.h; all 5
// call sites below continue to work unchanged.

// D-06: Connection-binding wrapper around classify_rm_target_impl<>. Mirrors
// plan 02's ensure_pubk / ensure_pubk_impl pattern exactly — production code
// uses this wrapper; unit tests call the template directly with a mocked
// Sender/Receiver pair (no real asio).
RmClassification classify_rm_target(
    Identity& id,
    Connection& conn,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target_hash,
    uint32_t& rid_counter) {
    auto send_fn = [&](MsgType t, std::span<const uint8_t> pl, uint32_t rid) {
        return conn.send(t, pl, rid);
    };
    auto recv_fn = [&]() { return conn.recv(); };
    // Phase 130 CLI-04: thread the live session cap into the template so the
    // CPAR manifest it decodes is validated against the current server cap.
    return classify_rm_target_impl(id, ns, target_hash, send_fn, recv_fn,
                                    rid_counter, conn.session_blob_cap());
}

} // namespace chromatindb::cli

namespace chromatindb::cli::cmd {

/// Build the payload [metadata_len:4BE][metadata_json][file_data] for envelope.
static std::vector<uint8_t> build_put_payload(
    const std::string& filename, const std::vector<uint8_t>& file_data) {

    nlohmann::json meta;
    meta["name"] = filename;
    meta["size"] = file_data.size();
    auto meta_str = meta.dump();

    std::vector<uint8_t> payload;
    payload.reserve(4 + meta_str.size() + file_data.size());

    // metadata_len:4BE
    uint8_t len_be[4];
    store_u32_be(len_be, static_cast<uint32_t>(meta_str.size()));
    payload.insert(payload.end(), len_be, len_be + 4);

    // metadata JSON bytes
    payload.insert(payload.end(),
                   reinterpret_cast<const uint8_t*>(meta_str.data()),
                   reinterpret_cast<const uint8_t*>(meta_str.data()) + meta_str.size());

    // file data
    payload.insert(payload.end(), file_data.begin(), file_data.end());
    return payload;
}

/// Parse the payload produced by build_put_payload.
struct ParsedPayload {
    std::string name;
    std::vector<uint8_t> file_data;
};

static ParsedPayload parse_put_payload(std::span<const uint8_t> plaintext) {
    if (plaintext.size() < 4) {
        throw std::runtime_error("Payload too short");
    }

    uint32_t meta_len = load_u32_be(plaintext.data());
    if (4 + meta_len > plaintext.size()) {
        throw std::runtime_error("Metadata length exceeds payload");
    }

    auto meta_json = nlohmann::json::parse(
        plaintext.data() + 4,
        plaintext.data() + 4 + meta_len,
        nullptr, false);

    if (meta_json.is_discarded()) {
        throw std::runtime_error("Invalid metadata JSON");
    }

    ParsedPayload result;
    if (meta_json.contains("name") && meta_json["name"].is_string()) {
        result.name = meta_json["name"].get<std::string>();
    }

    size_t data_offset = 4 + meta_len;
    result.file_data.assign(
        plaintext.begin() + static_cast<ptrdiff_t>(data_offset),
        plaintext.end());

    return result;
}

/// Load recipient KEM pubkeys from files. Returns vector of (signing_pk, kem_pk) pairs.
static std::vector<std::vector<uint8_t>> load_recipient_kem_pubkeys(
    const std::vector<std::string>& share_args,
    const std::string& identity_dir) {

    std::vector<std::vector<uint8_t>> kem_pks;
    kem_pks.reserve(share_args.size());

    for (const auto& arg : share_args) {
        if (arg.size() > 1 && arg[0] == '@') {
            // @group resolution
            std::string group_name = arg.substr(1);
            auto db_path = identity_dir + "/contacts.db";
            ContactDB db(db_path);
            auto members = db.group_members(group_name);
            if (members.empty()) {
                throw std::runtime_error("Group '@" + group_name + "' is empty or does not exist");
            }
            for (const auto& contact : members) {
                if (contact.kem_pk.empty()) {
                    throw std::runtime_error("contact " + contact.name + " has no encryption key");
                }
                kem_pks.push_back(contact.kem_pk);
            }
        } else if (fs::exists(arg)) {
            // File path — load pubkey from file
            auto data = read_file_bytes(arg);
            auto [signing_pk, kem_pk] = Identity::load_public_keys(data);
            kem_pks.push_back(std::move(kem_pk));
        } else {
            // Contact name lookup
            auto db_path = identity_dir + "/contacts.db";
            ContactDB db(db_path);
            auto contact = db.get(arg);
            if (!contact) {
                throw std::runtime_error("Unknown contact or file: " + arg);
            }
            if (contact->kem_pk.empty()) {
                throw std::runtime_error("contact " + contact->name + " has no encryption key");
            }
            kem_pks.push_back(contact->kem_pk);
        }
    }
    return kem_pks;
}

/// Resolve a delegate/revoke target spec to one or more signing pubkeys.
/// Accepts: `@group` (all members), a contact name, or a pubkey file path.
/// Pairs each signing_pk with a human label for output.
struct SigningTarget {
    std::vector<uint8_t> signing_pk;
    std::string label;  // contact name, "<group>/<contact>", or file path
};

static std::vector<SigningTarget> resolve_signing_targets(
    const std::string& arg, const std::string& identity_dir) {

    std::vector<SigningTarget> out;

    if (arg.size() > 1 && arg[0] == '@') {
        std::string group_name = arg.substr(1);
        auto db_path = identity_dir + "/contacts.db";
        ContactDB db(db_path);
        auto members = db.group_members(group_name);
        if (members.empty()) {
            throw std::runtime_error("Group '@" + group_name + "' is empty or does not exist");
        }
        out.reserve(members.size());
        for (const auto& contact : members) {
            if (contact.signing_pk.empty()) {
                throw std::runtime_error("contact " + contact.name + " has no signing key");
            }
            out.push_back({contact.signing_pk, group_name + "/" + contact.name});
        }
        return out;
    }

    if (fs::exists(arg)) {
        auto data = read_file_bytes(arg);
        auto [signing_pk, kem_pk] = Identity::load_public_keys(data);
        out.push_back({std::move(signing_pk), arg});
        return out;
    }

    // Contact name lookup
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    auto contact = db.get(arg);
    if (!contact) {
        throw std::runtime_error("Unknown contact or file: " + arg);
    }
    if (contact->signing_pk.empty()) {
        throw std::runtime_error("contact " + contact->name + " has no signing key");
    }
    out.push_back({contact->signing_pk, contact->name});
    return out;
}

// =============================================================================
// keygen
// =============================================================================

int keygen(const std::string& identity_dir, bool force) {
    fs::path dir(identity_dir);

    if (!force && fs::exists(dir / "identity.key")) {
        std::fprintf(stderr, "Error: identity already exists at %s (use --force to overwrite)\n",
                     dir.c_str());
        return 1;
    }

    auto id = Identity::generate();
    id.save_to(dir);

    std::printf("%s\n", to_hex(id.namespace_id()).c_str());
    return 0;
}

// =============================================================================
// whoami
// =============================================================================

int whoami(const std::string& identity_dir) {
    auto id = Identity::load_from(identity_dir);
    std::printf("%s\n", to_hex(id.namespace_id()).c_str());
    return 0;
}

// =============================================================================
// export-key
// =============================================================================

static std::string base64_encode(std::span<const uint8_t> bytes) {
    static constexpr char ALPHABET[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out.push_back(ALPHABET[(v >> 18) & 0x3f]);
        out.push_back(ALPHABET[(v >> 12) & 0x3f]);
        out.push_back(ALPHABET[(v >> 6)  & 0x3f]);
        out.push_back(ALPHABET[v         & 0x3f]);
    }
    if (i < bytes.size()) {
        uint32_t v = uint32_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= uint32_t(bytes[i + 1]) << 8;
        out.push_back(ALPHABET[(v >> 18) & 0x3f]);
        out.push_back(ALPHABET[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? ALPHABET[(v >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

int export_key(const std::string& identity_dir, const std::string& format,
               const std::string& out_path, bool signing_only, bool kem_only) {

    auto id = Identity::load_from(identity_dir);

    // Select which slice of the 4160-byte export to emit.
    // Layout: [signing_pk:2592][kem_pk:1568]
    auto full = id.export_public_keys();
    if (full.size() != 4160) {
        std::fprintf(stderr, "Error: unexpected export size %zu (expected 4160)\n", full.size());
        return 1;
    }
    std::span<const uint8_t> bytes(full.data(), full.size());
    if (signing_only) bytes = bytes.subspan(0, 2592);
    else if (kem_only) bytes = bytes.subspan(2592, 1568);

    // Refuse to splatter raw binary onto an interactive terminal.
    bool write_to_stdout = out_path.empty();
    bool is_tty = write_to_stdout && ::isatty(fileno(stdout));
    if (format == "raw" && is_tty) {
        std::fprintf(stderr,
            "Error: refusing to write raw binary to a terminal.\n"
            "       Use --out <file>, redirect with '>', or pick --format hex|b64.\n");
        return 1;
    }

    std::string encoded;  // backing storage for hex/b64; empty if raw
    std::span<const uint8_t> to_write;
    if (format == "raw") {
        to_write = bytes;
    } else if (format == "hex") {
        encoded = to_hex(bytes) + "\n";
        to_write = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
    } else if (format == "b64") {
        encoded = base64_encode(bytes) + "\n";
        to_write = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
    } else {
        std::fprintf(stderr, "Error: unknown format '%s' (use raw|hex|b64)\n", format.c_str());
        return 1;
    }

    if (!out_path.empty()) {
        std::ofstream f(out_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "Error: cannot write to %s\n", out_path.c_str());
            return 1;
        }
        f.write(reinterpret_cast<const char*>(to_write.data()),
                static_cast<std::streamsize>(to_write.size()));
        std::fprintf(stderr, "wrote %zu bytes to %s\n", to_write.size(), out_path.c_str());
    } else {
        std::cout.write(reinterpret_cast<const char*>(to_write.data()),
                        static_cast<std::streamsize>(to_write.size()));
        std::cout.flush();
    }
    return 0;
}

// =============================================================================
// put
// =============================================================================

// Resolve a NAME to its current winner's target content_hash using
// the same D-01/D-02 rule that get_by_name applies (timestamp DESC, content_hash
// DESC tiebreak). Returns nullopt when no NAME binding exists. Used by
// `cmd::put --replace` at Step 0 to locate the prior content that --replace
// should BOMB. Operates over `conn` (caller-owned) with dedicated request ids
// so the caller's rid counter is not disturbed. Implemented in Task 03 block
// below; forward declaration kept here so the Task 02 put body compiles.
static std::optional<std::array<uint8_t, 32>> resolve_name_to_target_hash(
    Identity& id, Connection& conn,
    std::span<const uint8_t, 32> ns, const std::string& name);

// helper: build a signed NAME blob for a (name, target_content_hash)
// binding and submit it via `conn`. Returns the server-assigned blob_hash of
// the NAME record on success. The caller supplies `now` to keep the content /
// NAME / BOMB timestamps monotonically increasing so D-01 (highest timestamp
// wins) gives the intended writer precedence on replay. `rid` is this blob's
// request id in the caller's pipeline.
static std::optional<std::array<uint8_t, 32>> submit_name_blob(
    Identity& id, Connection& conn,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t> name_bytes,
    std::span<const uint8_t, 32> target_hash,
    uint32_t ttl, uint64_t now, uint32_t rid) {

    auto name_data = make_name_data(name_bytes, target_hash);
    auto ns_blob   = build_owned_blob(id, ns, name_data, ttl, now);
    auto envelope  = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

    if (!conn.send(MsgType::BlobWrite, envelope, rid)) return std::nullopt;
    auto resp = conn.recv();
    if (!resp || resp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
        resp->payload.size() < 32) {
        return std::nullopt;
    }
    std::array<uint8_t, 32> out{};
    std::memcpy(out.data(), resp->payload.data(), 32);
    return out;
}

// helper: sign and submit a pre-built BOMB payload (constructed by
// the caller via wire::make_bomb_data). The caller builds the data so the
// textual `make_bomb_data` call site is visible at each BOMB emitter
// (cmd::put --replace and cmd::rm_batch). ttl=0 is hard-coded (D-13).
static std::optional<std::array<uint8_t, 32>> submit_bomb_blob(
    Identity& id, Connection& conn,
    std::span<const uint8_t, 32> ns,
    std::vector<uint8_t> bomb_data,
    uint64_t now, uint32_t rid,
    const std::string& host_for_errors = "node") {

    // D-13 invariant — BOMB is permanent like single tombstone (ttl=0).
    auto ns_blob  = build_owned_blob(id, ns, bomb_data, 0, now);
    auto envelope = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

    // Rule-1 fix: BOMBs are structurally regular blobs
    // (data = [BOMB:4][count:4BE][target_hash:32]*N). The node's Delete=17
    // dispatcher routes to engine.delete_blob() which ONLY accepts tombstone
    // format (36 bytes: [magic:4][hash:32]). BOMBs must therefore ride the
    // BlobWrite=64 ingest path, where engine.ingest() dispatches on the
    // 4-byte magic and recognises BOMB as a batch-tombstone blob type.
    //
    // The ack type is WriteAck (not DeleteAck) — payload layout is the same
    // 41-byte [blob_hash:32][seq:8BE][status:1] shape, so only the MsgType
    // on the send + recv check needs to change.
    if (!conn.send(MsgType::BlobWrite, envelope, rid)) return std::nullopt;
    auto resp = conn.recv();
    if (!resp) return std::nullopt;

    // Surface ErrorResponse so the caller's error string isn't lost (pre-fix
    // this function silently discarded the node's rejection reason, making
    // BOMB failures indistinguishable from transport hiccups). The host
    // parameter threads `opts.host` from the caller so the D-05 wording
    // ("Error: namespace not yet initialized on node 127.0.0.1 ...") names
    // the real node rather than the placeholder "node". Default-arg keeps
    // backward-compat for any caller that hasn't wired host yet.
    if (resp->type == static_cast<uint8_t>(MsgType::ErrorResponse)) {
        std::fprintf(stderr, "%s\n",
            decode_error_response(resp->payload, host_for_errors, ns).c_str());
        return std::nullopt;
    }

    if ((resp->type != static_cast<uint8_t>(MsgType::WriteAck) &&
         resp->type != static_cast<uint8_t>(MsgType::DeleteAck)) ||
        resp->payload.size() < 32) {
        return std::nullopt;
    }
    std::array<uint8_t, 32> out{};
    std::memcpy(out.data(), resp->payload.data(), 32);
    return out;
}

int put(const std::string& identity_dir, const std::vector<std::string>& file_paths,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin,
        const std::optional<std::string>& name_opt, bool replace,
        const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);

    // Load recipient KEM pubkeys once (always include self)
    std::vector<std::span<const uint8_t>> recipient_spans;
    auto external_pks = load_recipient_kem_pubkeys(share_pubkey_files, identity_dir);
    auto self_kem_pk = id.kem_pubkey();
    recipient_spans.emplace_back(self_kem_pk);
    for (auto& pk : external_pks) {
        recipient_spans.emplace_back(std::span<const uint8_t>(pk));
    }

    auto ns = id.namespace_id();

    // One connection for all files
    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    // D-01: auto-PUBK probe+emit before the first owner-write to this ns.
    // Invocation-scoped cache means a batched put-many-files only probes once.
    {
        std::span<const uint8_t, 32> ns_span(ns.data(), 32);
        uint32_t pubk_rid = 0x2000;
        if (!ensure_pubk(id, conn, ns_span, pubk_rid)) {
            std::fprintf(stderr,
                "Error: failed to ensure namespace is published on node %s\n",
                opts.host.c_str());
            conn.close();
            return 1;
        }
    }

    int errors = 0;

    // content_hash captured from the single WriteAck when --name is set.
    // `content_hash_was_captured` stays false on unnamed puts so the NAME-emission
    // block below is a strict no-op for the legacy flow.
    std::array<uint8_t, 32> content_hash_captured{};
    bool content_hash_was_captured = false;

    // Step 0 (--replace only): look up the prior NAME binding BEFORE
    // emitting any content, so we know which old content_hash to BOMB later.
    // Writing proceeds even if the lookup returns nullopt (plan action step 0:
    // "warn; continue without BOMB").
    std::optional<std::array<uint8_t, 32>> replace_old_target;
    if (replace && name_opt.has_value()) {
        std::span<const uint8_t, 32> ns_s(ns.data(), 32);
        replace_old_target = resolve_name_to_target_hash(id, conn, ns_s, *name_opt);
    }

    // Build list of files to upload
    struct FileEntry { std::string path; std::string name; std::vector<uint8_t> data; };
    std::vector<FileEntry> files;

    // Client-side guard: read_file_bytes + envelope + FlatBuffer = ~3x file size in memory.
    // Node enforces Config::blob_max_bytes (default 4 MiB, advertised via NodeInfoResponse).
    static constexpr size_t MAX_FILE_SIZE = 500ULL * 1024 * 1024;

    if (from_stdin) {
        auto stdin_bytes = read_stdin_bytes();
        if (stdin_bytes.empty()) {
            std::fprintf(stderr, "Error: stdin is empty (refusing to upload a 0-byte blob)\n");
            return 1;
        }
        files.push_back({"", "", std::move(stdin_bytes)});
    } else {
        for (const auto& fp : file_paths) {
            auto fsize = fs::file_size(fp);
            if (fsize == 0) {
                std::fprintf(stderr, "Error: %s is empty (refusing to upload a 0-byte blob)\n", fp.c_str());
                return 1;
            }
            // D-14: reject files above the hard 1 TiB cap up-front,
            // before any transfer begins.
            if (fsize > chunked::MAX_CHUNKED_FILE_SIZE) {
                std::fprintf(stderr,
                    "Error: %s too large (%llu bytes, max 1 TiB)\n",
                    fp.c_str(),
                    static_cast<unsigned long long>(fsize));
                return 1;
            }
            // Phase 130 CLI-02/03 / CONTEXT.md D-04: files STRICTLY larger
            // than the live server cap take the chunked path. chunked::put_chunked
            // streams the file, pipelines N × CDAT chunks + 1 × CPAR manifest
            // over this same Connection, and prints the manifest hash to stdout.
            if (fsize > conn.session_blob_cap()) {
                // YAGNI: chunked (CPAR manifest) + --name is a valid
                // future combination but requires extra wiring (the manifest
                // hash, not the first CDAT, must be what the NAME binds to).
                // Reject explicitly so a caller doesn't silently bind the
                // wrong hash. Future phase can lift this.
                if (name_opt.has_value()) {
                    std::fprintf(stderr,
                        "Error: --name is not yet supported for large chunked files "
                        "(%s is %llu bytes, session cap %llu)\n",
                        fp.c_str(),
                        static_cast<unsigned long long>(fsize),
                        static_cast<unsigned long long>(conn.session_blob_cap()));
                    conn.close();
                    return 1;
                }
                auto fname = fs::path(fp).filename().string();
                int rc = chunked::put_chunked(id, ns, recipient_spans, fp,
                                               fname, ttl, conn, opts);
                if (rc != 0) ++errors;
                continue;   // skip the single-blob path for this file
            }
            if (fsize > MAX_FILE_SIZE) {
                std::fprintf(stderr, "Error: %s too large (%zu MiB, max ~500 MiB)\n",
                             fp.c_str(), static_cast<size_t>(fsize / (1024 * 1024)));
                return 1;
            }
            auto fname = fs::path(fp).filename().string();
            files.push_back({fp, fname, read_file_bytes(fp)});
        }
    }

    // 120-02 / PIPE-01: Pipeline the put fan-out.
    // Phase A: fire writes up to Connection::kPipelineDepth; backpressure is
    //          handled inside send_async (it pumps recv() into pending_replies_
    //          when in_flight_ == kPipelineDepth).
    // Phase B: drain one WriteAck at a time via recv() in arrival order (D-08),
    //          and look up which file each ack belongs to via the batch-local
    //          rid_to_index map.
    //
    // recv() (not recv_for) is intentional: D-08 wants per-item lines in
    // completion order, not request order. send_async's backpressure pump may
    // have already stashed some acks in Connection::pending_replies_; draining
    // those first is handled transparently by recv() -- the pumping helpers in
    // 120-01 reuse the same pending_replies_ map. Single-reader invariant
    // (PIPE-02 / D-09) is preserved: still exactly one caller of recv().
    uint32_t rid = 1;
    std::unordered_map<uint32_t, size_t> rid_to_index;  // batch-local
    size_t next_to_send = 0;
    size_t completed = 0;

    while (completed < files.size()) {
        // Phase A: greedy fill the pipeline window. send_async blocks via its
        // internal recv-pump when in_flight_ reaches depth, so the loop
        // self-regulates.
        if (next_to_send < files.size() &&
            rid_to_index.size() < Connection::kPipelineDepth) {
            auto& f = files[next_to_send];

            auto payload = build_put_payload(f.name, f.data);
            auto envelope_data = envelope::encrypt(payload, recipient_spans);

            auto timestamp = static_cast<uint64_t>(std::time(nullptr));
            auto ns_blob   = build_owned_blob(id, ns, envelope_data, ttl, timestamp);
            auto envelope  = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

            uint32_t this_rid = rid++;
            if (!conn.send_async(MsgType::BlobWrite, envelope, this_rid)) {
                std::fprintf(stderr, "Error: failed to send %s\n", f.name.c_str());
                ++errors;
                ++completed;
                ++next_to_send;
                continue;
            }
            rid_to_index[this_rid] = next_to_send;
            ++next_to_send;
            continue;  // keep filling the window before draining
        }

        // Phase B: drain one reply in arrival order (D-08 completion order,
        // not request order). recv_next() returns whichever WriteAck landed
        // first AND decrements in_flight_ (CR-01 / WR-04 fix — plain recv()
        // leaks the counter; a batch of 9+ small files would otherwise hang
        // on file 9 when the pipeline window fills).
        auto resp = conn.recv_next();
        if (!resp) {
            // Transport dead: every still-pending request becomes an error so
            // the user sees exactly which files are uncertain.
            for (auto& [pending_rid, idx] : rid_to_index) {
                (void)pending_rid;
                std::fprintf(stderr, "Error: connection lost while waiting for %s\n",
                             files[idx].name.c_str());
                ++errors;
                ++completed;
            }
            rid_to_index.clear();
            break;
        }

        auto it = rid_to_index.find(resp->request_id);
        if (it == rid_to_index.end()) {
            // Stray rid (server bug or orphaned reply). Mirrors the D-04
            // stance in connection.cpp recv_for: log at debug, drop, continue.
            spdlog::debug("cmd::put: discarding reply for unknown rid {} (type {})",
                          resp->request_id, static_cast<unsigned>(resp->type));
            continue;
        }

        size_t file_idx = it->second;
        rid_to_index.erase(it);
        ++completed;
        auto& f = files[file_idx];

        if (resp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
            resp->payload.size() < 41) {
            std::fprintf(stderr, "Error: bad response for %s\n", f.name.c_str());
            ++errors;
            continue;
        }

        auto hash_span = std::span<const uint8_t>(resp->payload.data(), 32);
        auto hash_hex = to_hex(hash_span);
        if (!opts.quiet && !f.name.empty() && files.size() > 1) {
            std::printf("%s  %s\n", hash_hex.c_str(), f.name.c_str());
        } else if (!opts.quiet) {
            std::printf("%s\n", hash_hex.c_str());
        }

        // If --name was passed, we drained exactly one content blob
        // above (main.cpp argv enforces files.size()==1 when name is set).
        // Capture its content_hash for the NAME-blob wiring below.
        if (name_opt.has_value()) {
            std::memcpy(content_hash_captured.data(), hash_span.data(), 32);
            content_hash_was_captured = true;
        }
    }

    // =======================================================================
    // content → NAME → BOMB write-before-delete ordering.
    //
    // At this point:
    //   - content_hash_captured holds the server-assigned hash of the content
    //     blob we just wrote (guaranteed when name_opt.has_value(), since the
    //     argv layer in main.cpp rejects name+batch combinations).
    //   - `replace_old_target` (set during Step 0 above) holds the prior
    //     NAME's target_hash when --replace was resolved to a live binding.
    //
    // Ordering rationale (plan-checker iteration fix): BOMB runs LAST so a
    // partial failure leaves prior content reachable by hash. Reversing would
    // risk deleting old content before the replacement was visible.
    // =======================================================================
    if (name_opt.has_value() && errors == 0 && content_hash_was_captured) {
        auto now = static_cast<uint64_t>(std::time(nullptr));
        // NAME timestamp >= content timestamp so resolution (D-01) lands on
        // this writer's binding. We use time(nullptr) directly — clock drift
        // within a single `cdb put` invocation is milliseconds at worst and
        // the outer second-granularity comparison absorbs it.
        std::span<const uint8_t, 32> ns_s(ns.data(), 32);
        std::span<const uint8_t, 32> target_s(content_hash_captured.data(), 32);
        std::span<const uint8_t> name_bytes(
            reinterpret_cast<const uint8_t*>(name_opt->data()),
            name_opt->size());

        auto name_hash = submit_name_blob(id, conn, ns_s, name_bytes,
                                           target_s, ttl, now, rid++);
        if (!name_hash) {
            std::fprintf(stderr, "Error: failed to write NAME blob for --name=%s\n",
                         name_opt->c_str());
            ++errors;
        } else if (!opts.quiet) {
            std::fprintf(stderr, "named: %s -> %s\n",
                         name_opt->c_str(),
                         to_hex(content_hash_captured).c_str());
        }

        // BOMB-of-1 for --replace AFTER NAME succeeds (write-before-delete).
        if (errors == 0 && replace && replace_old_target.has_value()) {
            std::array<uint8_t, 32> targets_arr[1] = {*replace_old_target};
            // Build BOMB payload via the Plan 01 codec helper (mirrors
            // cmd::rm_batch; feedback_no_duplicate_code.md — the wire helper
            // is the single source of truth for the BOMB layout).
            auto bomb_data = make_bomb_data(
                std::span<const std::array<uint8_t, 32>>(targets_arr, 1));
            auto bomb_now = static_cast<uint64_t>(std::time(nullptr));
            auto bomb_hash = submit_bomb_blob(id, conn, ns_s,
                                               std::move(bomb_data),
                                               bomb_now, rid++,
                                               opts.host);
            if (!bomb_hash) {
                std::fprintf(stderr, "Error: failed to write BOMB for --replace "
                             "(prior content %s remains reachable by hash)\n",
                             to_hex(*replace_old_target).c_str());
                ++errors;
            } else if (!opts.quiet) {
                std::fprintf(stderr, "replaced: %s tombstoned\n",
                             to_hex(*replace_old_target).c_str());
            }
        } else if (replace && !replace_old_target.has_value() && !opts.quiet) {
            // Plan action step 0 note: "if no prior NAME exists, warn".
            std::fprintf(stderr, "note: --replace had no prior binding for '%s'; "
                         "nothing to tombstone\n", name_opt->c_str());
        }
    }

    conn.close();
    return errors > 0 ? 1 : 0;
}

// =============================================================================
// get
// =============================================================================

int get(const std::string& identity_dir, const std::vector<std::string>& hash_hexes,
        const std::string& namespace_hex, bool to_stdout,
        const std::string& output_dir, bool force_overwrite, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    // 120-02 / PIPE-01: Pipeline the get fan-out. Same two-phase shape as
    // cmd::put above: Phase A fires ReadRequests up to Connection::kPipelineDepth,
    // Phase B drains ReadResponses via recv() in arrival order (D-08), mapping
    // each reply back to its hash via the batch-local rid_to_index.
    uint32_t rid = 1;
    std::unordered_map<uint32_t, size_t> rid_to_index;  // batch-local
    size_t next_to_send = 0;
    size_t completed = 0;
    int errors = 0;

    while (completed < hash_hexes.size()) {
        // Phase A: greedy fill the window.
        if (next_to_send < hash_hexes.size() &&
            rid_to_index.size() < Connection::kPipelineDepth) {
            const auto& hash_hex = hash_hexes[next_to_send];

            std::array<uint8_t, 32> hash;
            try {
                hash = parse_hash(hash_hex);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "Error: invalid hash %s: %s\n",
                             hash_hex.c_str(), e.what());
                ++errors;
                ++completed;
                ++next_to_send;
                continue;
            }

            std::vector<uint8_t> payload(64);
            std::memcpy(payload.data(), ns.data(), 32);
            std::memcpy(payload.data() + 32, hash.data(), 32);

            uint32_t this_rid = rid++;
            if (!conn.send_async(MsgType::ReadRequest, payload, this_rid)) {
                std::fprintf(stderr, "Error: failed to send ReadRequest for %s\n",
                             hash_hex.c_str());
                ++errors;
                ++completed;
                ++next_to_send;
                continue;
            }
            rid_to_index[this_rid] = next_to_send;
            ++next_to_send;
            continue;  // keep filling before draining
        }

        // Phase B: drain one reply in arrival order. recv_next() decrements
        // in_flight_ (CR-01 / WR-04 fix — plain recv() leaks the counter
        // and would stall a get batch of 9+ hashes on the 9th).
        auto resp = conn.recv_next();
        if (!resp) {
            for (auto& [pending_rid, idx] : rid_to_index) {
                (void)pending_rid;
                std::fprintf(stderr, "Error: connection lost while fetching %s\n",
                             hash_hexes[idx].c_str());
                ++errors;
                ++completed;
            }
            rid_to_index.clear();
            break;
        }

        auto it = rid_to_index.find(resp->request_id);
        if (it == rid_to_index.end()) {
            spdlog::debug("cmd::get: discarding reply for unknown rid {} (type {})",
                          resp->request_id, static_cast<unsigned>(resp->type));
            continue;
        }

        size_t hash_idx = it->second;
        rid_to_index.erase(it);
        ++completed;
        const auto& hash_hex = hash_hexes[hash_idx];

        if (resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
            resp->payload.empty()) {
            std::fprintf(stderr, "Error: bad response for %s\n", hash_hex.c_str());
            ++errors;
            continue;
        }

        if (resp->payload[0] != 0x01) {
            std::fprintf(stderr, "Error: blob not found: %s\n", hash_hex.c_str());
            ++errors;
            continue;
        }

        auto blob_bytes = std::span<const uint8_t>(
            resp->payload.data() + 1, resp->payload.size() - 1);
        auto blob = decode_blob(blob_bytes);
        if (!blob) {
            std::fprintf(stderr, "%s: failed to decode blob\n", hash_hex.c_str());
            ++errors;
            continue;
        }

        // CHUNK-03: type-prefix dispatch before envelope::decrypt.
        // Per D-13 the outer CDAT/CPAR magic lives on blob.data BEFORE the
        // CENV envelope. CDAT -> error (raw chunks are never user-facing).
        // CPAR -> envelope-decrypt the manifest and hand off to get_chunked,
        // reusing THIS Connection (one PQ handshake per cdb get). Other ->
        // fall through to the existing single-blob path unchanged.
        switch (chunked::classify_blob_data(blob->data)) {
            case chunked::GetDispatch::CDAT: {
                std::fprintf(stderr,
                    "Error: %s is a raw chunk (CDAT) — fetch the CPAR manifest instead\n",
                    hash_hex.c_str());
                ++errors;
                continue;
            }
            case chunked::GetDispatch::CPAR: {
                if (to_stdout) {
                    // Streaming a multi-GiB plaintext to stdout bypasses the
                    // plaintext_sha3 integrity gate. YAGNI: a future tee-to-stdout
                    // path could still integrity-verify before emit.
                    std::fprintf(stderr,
                        "Error: cdb get --stdout is not supported for chunked manifests (%s)\n",
                        hash_hex.c_str());
                    ++errors;
                    continue;
                }
                auto envelope_bytes = std::span<const uint8_t>(
                    blob->data.data() + 4, blob->data.size() - 4);
                auto manifest_plain = envelope::decrypt(envelope_bytes,
                    id.kem_seckey(), id.kem_pubkey());
                if (!manifest_plain) {
                    std::fprintf(stderr,
                        "%s: cannot decrypt manifest (not a recipient)\n",
                        hash_hex.c_str());
                    ++errors;
                    continue;
                }
                // Phase 130 CLI-04 / CONTEXT.md D-06: validate manifest
                // against the live session cap. If chunk_size_bytes > cap the
                // decoder fprintf's both values and returns nullopt.
                auto manifest = decode_manifest_payload(*manifest_plain,
                                                        conn.session_blob_cap());
                if (!manifest) {
                    std::fprintf(stderr,
                        "Error: %s has an invalid CPAR manifest\n",
                        hash_hex.c_str());
                    ++errors;
                    continue;
                }
                std::string out_filename = manifest->filename.empty()
                                             ? hash_hex : manifest->filename;
                auto out_path = output_dir.empty()
                                  ? out_filename
                                  : output_dir + "/" + out_filename;
                int rc = chunked::get_chunked(
                    id,
                    std::span<const uint8_t, 32>(ns.data(), 32),
                    *manifest, out_path, force_overwrite, conn, opts);
                if (rc != 0) ++errors;
                continue;
            }
            case chunked::GetDispatch::Other:
                // Fall through to the existing envelope::is_envelope branch.
                break;
        }

        std::vector<uint8_t> plaintext;
        if (envelope::is_envelope(blob->data)) {
            auto decrypted = envelope::decrypt(blob->data, id.kem_seckey(), id.kem_pubkey());
            if (!decrypted) {
                std::fprintf(stderr, "%s: cannot decrypt (not a recipient)\n", hash_hex.c_str());
                ++errors;
                continue;
            }
            plaintext = std::move(*decrypted);
        } else {
            plaintext = blob->data;
        }

        ParsedPayload parsed;
        try {
            parsed = parse_put_payload(plaintext);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", hash_hex.c_str(), e.what());
            ++errors;
            continue;
        }
        std::string out_filename = parsed.name.empty() ? hash_hex : parsed.name;

        if (to_stdout) {
            std::cout.write(reinterpret_cast<const char*>(parsed.file_data.data()),
                            static_cast<std::streamsize>(parsed.file_data.size()));
            std::cout.flush();
        } else {
            auto out_path = output_dir.empty() ? out_filename
                : output_dir + "/" + out_filename;

            if (!force_overwrite && fs::exists(out_path)) {
                std::fprintf(stderr, "Error: %s already exists (use --force to overwrite)\n",
                             out_path.c_str());
                ++errors;
                continue;
            }

            std::ofstream f(out_path, std::ios::binary);
            if (!f) {
                std::fprintf(stderr, "Error: cannot write to %s\n", out_path.c_str());
                ++errors;
                continue;
            }
            f.write(reinterpret_cast<const char*>(parsed.file_data.data()),
                    static_cast<std::streamsize>(parsed.file_data.size()));
            if (!opts.quiet) {
                std::fprintf(stderr, "saved: %s\n", out_path.c_str());
            }
        }
    }

    conn.close();
    return errors > 0 ? 1 : 0;
}

// =============================================================================
// rm
// =============================================================================

int rm(const std::string& identity_dir, const std::string& hash_hex,
       const std::string& namespace_hex, bool force, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);
    auto target_hash = parse_hash(hash_hex);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    // D-01: auto-PUBK probe+emit before the first owner-write tombstone. The
    // single-target rm targets the writer's own namespace; the probe uses a
    // dedicated rid range (0x2000+) separate from this command's rid_counter.
    {
        std::span<const uint8_t, 32> ns_span(ns.data(), 32);
        uint32_t pubk_rid = 0x2000;
        if (!ensure_pubk(id, conn, ns_span, pubk_rid)) {
            std::fprintf(stderr,
                "Error: failed to ensure namespace is published on node %s\n",
                opts.host.c_str());
            conn.close();
            return 1;
        }
    }

    // monotonic rid counter so the inserted ReadRequest (type-prefix
    // dispatch) doesn't collide with the existing ExistsRequest and Delete rids.
    // Each send step allocates a fresh rid; the counter is self-documenting.
    uint32_t rid_counter = 1;

    // Pre-check target existence unless --force. Avoids creating redundant
    // tombstones for already-gone / never-existed blobs (backlog 999.2).
    if (!force) {
        std::vector<uint8_t> exists_payload(64);
        std::memcpy(exists_payload.data(), ns.data(), 32);
        std::memcpy(exists_payload.data() + 32, target_hash.data(), 32);

        if (!conn.send(MsgType::ExistsRequest, exists_payload, rid_counter++)) {
            std::fprintf(stderr, "Error: failed to send ExistsRequest\n");
            conn.close();
            return 1;
        }

        auto exists_resp = conn.recv();
        if (!exists_resp ||
            exists_resp->type != static_cast<uint8_t>(MsgType::ExistsResponse) ||
            exists_resp->payload.size() < 33) {
            std::fprintf(stderr, "Error: failed to probe target existence\n");
            conn.close();
            return 1;
        }

        if (exists_resp->payload[0] != 0x01) {
            std::fprintf(stderr, "Error: blob not found: %s (use --force to tombstone anyway)\n",
                         hash_hex.c_str());
            conn.close();
            return 1;
        }
    }

    // CHUNK-04 + D-06: classify the target before
    // deleting. Dispatch:
    //
    //   Plain          -> fall through to the single-tombstone path below.
    //   Cdat           -> user tried to remove a raw chunk; tell them to
    //                     tombstone the CPAR manifest instead (defense-in-depth).
    //   CparWithChunks -> delegate cascade to `chunked::rm_chunked` using
    //                     the chunk hashes the classifier extracted from
    //                     the manifest. rm_chunked's plan_tombstone_targets
    //                     only reads `segment_count` and `chunk_hashes`, so
    //                     a minimal ManifestData with those two fields is
    //                     sufficient (behavior unchanged vs. pre-124).
    //   FetchFailed    -> warn-friendly message; fall through (the node will
    //                     accept a plain tombstone or reject it, same as pre-124
    //                     recoverable pre-read failure path).
    //
    // Accepted cost: one extra round-trip on every `cdb rm` invocation vs.
    // adding a new wire message type, which would violate the dumb-DB
    // principle (D-08). Documented in 119-01-SUMMARY.md.
    {
        std::span<const uint8_t, 32> ns_s(ns.data(), 32);
        std::span<const uint8_t, 32> th_s(target_hash.data(), 32);
        auto rc = classify_rm_target(id, conn, ns_s, th_s, rid_counter);
        switch (rc.kind) {
            case RmClassification::Kind::Cdat:
                std::fprintf(stderr,
                    "Error: %s is a raw chunk (CDAT). Remove the CPAR "
                    "manifest instead.\n", hash_hex.c_str());
                conn.close();
                return 1;
            case RmClassification::Kind::CparWithChunks: {
                // Reconstitute a minimal ManifestData for rm_chunked. The
                // cascade-delete path only consults segment_count +
                // chunk_hashes (via plan_tombstone_targets at
                // chunked.cpp:43-59); other fields are unused for rm.
                ManifestData minimal_manifest{};
                minimal_manifest.segment_count = static_cast<uint32_t>(
                    rc.cascade_targets.size());
                minimal_manifest.chunk_hashes.reserve(
                    rc.cascade_targets.size() * 32);
                for (const auto& h : rc.cascade_targets) {
                    minimal_manifest.chunk_hashes.insert(
                        minimal_manifest.chunk_hashes.end(),
                        h.begin(), h.end());
                }
                int rrc = chunked::rm_chunked(id, ns_s, minimal_manifest,
                                               th_s, conn, opts);
                conn.close();
                return rrc;
            }
            case RmClassification::Kind::FetchFailed:
                // Recoverable — the node may simply not have the blob, or
                // the read failed transiently. Fall through to the plain
                // tombstone path; the user's intent is to tombstone this
                // hash regardless. Preserves pre-124 behavior.
                break;
            case RmClassification::Kind::Plain:
                // Nothing to cascade; fall through.
                break;
        }
    }

    // Build and send tombstone
    auto tombstone_data = make_tombstone_data(
        std::span<const uint8_t, 32>(target_hash.data(), 32));

    auto ns_span = std::span<const uint8_t, 32>(ns.data(), 32);
    auto timestamp = static_cast<uint64_t>(std::time(nullptr));
    auto ns_blob   = build_owned_blob(id, ns_span, tombstone_data, 0, timestamp);
    auto envelope  = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

    if (!conn.send(MsgType::Delete, envelope, rid_counter++)) {
        std::fprintf(stderr, "Error: failed to send Delete\n");
        conn.close();
        return 1;
    }

    auto resp = conn.recv();
    conn.close();

    if (!resp) {
        std::fprintf(stderr, "Error: no response\n");
        return 1;
    }

    if (resp->type == static_cast<uint8_t>(MsgType::ErrorResponse)) {
        std::fprintf(stderr, "%s\n",
            decode_error_response(resp->payload, opts.host,
                std::span<const uint8_t, 32>(ns.data(), 32)).c_str());
        return 1;
    }

    if (resp->type != static_cast<uint8_t>(MsgType::DeleteAck)) {
        std::fprintf(stderr, "Error: unexpected response type %u\n", resp->type);
        return 1;
    }

    // DeleteAck: [tombstone_hash:32][seq_num:8BE][status:1] = 41 bytes.
    // status: 0 = tombstone stored (fresh delete), 1 = target already tombstoned.
    if (resp->payload.size() < 41) {
        std::fprintf(stderr, "Error: invalid DeleteAck payload (%zu bytes)\n",
                      resp->payload.size());
        return 1;
    }

    uint8_t status = resp->payload[40];
    if (!opts.quiet) {
        if (status == 0) {
            std::fprintf(stderr, "deleted: %s\n", hash_hex.c_str());
        } else {
            std::fprintf(stderr, "already tombstoned: %s\n", hash_hex.c_str());
        }
        spdlog::debug("tombstone hash: {}",
                      to_hex(std::span<const uint8_t>(resp->payload.data(), 32)));
    }

    return 0;
}

// =============================================================================
// D-06/D-07: rm_batch — one invocation, ONE BOMB covering all targets
// =============================================================================

int rm_batch(const std::string& identity_dir,
             const std::vector<std::string>& hash_hexes,
             const std::string& namespace_hex, bool force,
             const ConnectOpts& opts) {

    if (hash_hexes.empty()) {
        // Defense in depth — main.cpp already rejects this, but callers might
        // arrive here via other paths in the future.
        std::fprintf(stderr, "Error: rm_batch requires at least one target hash\n");
        return 2;
    }

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);
    auto ns_span = std::span<const uint8_t, 32>(ns.data(), 32);

    // Decode all target hashes up front so malformed input is rejected before
    // we open a transport connection.
    std::vector<std::array<uint8_t, 32>> targets;
    targets.reserve(hash_hexes.size());
    for (const auto& h : hash_hexes) {
        try {
            targets.push_back(parse_hash(h));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Error: invalid hash %s: %s\n", h.c_str(), e.what());
            return 1;
        }
    }

    // Defense-in-depth: deduplicate identical targets so the BOMB's
    // declared count matches the number of distinct tombstoned blobs.
    // Uses a sorted-unique pass; O(N log N) and allocation-free on the
    // hot path for small N.
    {
        std::vector<std::array<uint8_t, 32>> sorted(targets);
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b){
                      return std::memcmp(a.data(), b.data(), 32) < 0;
                  });
        auto last = std::unique(sorted.begin(), sorted.end());
        sorted.erase(last, sorted.end());
        if (sorted.size() != targets.size() && !opts.quiet) {
            std::fprintf(stderr,
                "note: dropped %zu duplicate target(s); BOMB covers %zu unique\n",
                targets.size() - sorted.size(), sorted.size());
        }
        targets = std::move(sorted);
    }

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    // D-01: auto-PUBK probe+emit before the BOMB write; tombstones target the
    // writer's own namespace.
    {
        uint32_t pubk_rid = 0x2000;
        if (!ensure_pubk(id, conn, ns_span, pubk_rid)) {
            std::fprintf(stderr,
                "Error: failed to ensure namespace is published on node %s\n",
                opts.host.c_str());
            conn.close();
            return 1;
        }
    }

    uint32_t rid_counter = 1;

    // Pre-flight existence check unless --force. For batch mode we don't abort
    // the whole BOMB on first-missing — missing targets are rare and the BOMB
    // layout accepts them (D-14: node does not verify target existence). We
    // only warn once so the user sees what's going on.
    if (!force) {
        size_t missing = 0;
        for (const auto& t : targets) {
            std::vector<uint8_t> exists_payload(64);
            std::memcpy(exists_payload.data(), ns.data(), 32);
            std::memcpy(exists_payload.data() + 32, t.data(), 32);
            if (!conn.send(MsgType::ExistsRequest, exists_payload, rid_counter++)) {
                std::fprintf(stderr, "Error: failed to probe target existence\n");
                conn.close();
                return 1;
            }
            auto r = conn.recv();
            if (!r || r->type != static_cast<uint8_t>(MsgType::ExistsResponse) ||
                r->payload.size() < 1) {
                std::fprintf(stderr, "Error: bad ExistsResponse\n");
                conn.close();
                return 1;
            }
            if (r->payload[0] != 0x01) {
                if (!opts.quiet) {
                    std::fprintf(stderr, "warning: target not found: %s\n",
                                 to_hex(t).c_str());
                }
                ++missing;
            }
        }
        if (missing == targets.size()) {
            std::fprintf(stderr,
                "Error: none of the %zu targets exist (use --force to BOMB anyway)\n",
                targets.size());
            conn.close();
            return 1;
        }
    }

    // D-06: BOMB cascade across CPAR manifests. For each target,
    // classify via `classify_rm_target`; CPAR manifests contribute their chunk
    // hashes to the BOMB target list. Partial-failure policy per RESEARCH Q6
    // (option 3: warn-and-continue): a FetchFailed manifest is BOMBed
    // manifest-only, with a stderr warning and a final summary line.
    //
    // T-124-04 mitigation: batched rm of a chunked manifest no longer leaks
    // orphan chunks on the node (the pre-124 cmd::rm_batch did nothing with
    // CPAR targets beyond hashing them into the BOMB).
    std::vector<std::array<uint8_t, 32>> bomb_targets;
    bomb_targets.reserve(targets.size());
    size_t manifests_full = 0;
    size_t manifests_failed = 0;

    for (const auto& t : targets) {
        std::span<const uint8_t, 32> t_span(t.data(), 32);
        auto rc = classify_rm_target(id, conn, ns_span, t_span, rid_counter);
        // The target itself is ALWAYS included in the BOMB. For CparWithChunks
        // we additionally include every chunk hash.
        bomb_targets.push_back(t);

        switch (rc.kind) {
            case RmClassification::Kind::CparWithChunks:
                for (const auto& ch : rc.cascade_targets) {
                    bomb_targets.push_back(ch);
                }
                ++manifests_full;
                break;
            case RmClassification::Kind::FetchFailed: {
                // Warn and continue: BOMB the manifest alone.
                auto short_hash = to_hex(
                    std::span<const uint8_t>(t.data(), 8));
                std::fprintf(stderr,
                    "warning: cascade fetch failed for %s; BOMBing manifest only\n",
                    short_hash.c_str());
                ++manifests_failed;
                break;
            }
            case RmClassification::Kind::Cdat:
                // Raw CDAT as a batch target: treat as a plain tombstone
                // (the node will accept — we don't know which manifest owns
                // this chunk from here, so no cascade). Caller is presumed
                // intentional; this is not a user-facing error.
                break;
            case RmClassification::Kind::Plain:
                // Nothing to expand.
                break;
        }
    }

    // Dedup + sort after cascade expansion: a chunk hash that also appears as
    // an explicit batch target would otherwise appear twice in the BOMB count.
    std::sort(bomb_targets.begin(), bomb_targets.end(),
              [](const auto& a, const auto& b){
                  return std::memcmp(a.data(), b.data(), 32) < 0;
              });
    bomb_targets.erase(
        std::unique(bomb_targets.begin(), bomb_targets.end()),
        bomb_targets.end());

    // Emit the single BOMB.
    auto now = static_cast<uint64_t>(std::time(nullptr));
    // Build BOMB payload via the Plan 01 codec helper: [BOMB:4][count:4BE][hash:32]×N.
    // Centralising byte layout in wire::make_bomb_data keeps this matching the
    // node-side BOMB parser byte-for-byte (feedback_no_duplicate_code.md).
    auto bomb_data = make_bomb_data(
        std::span<const std::array<uint8_t, 32>>(bomb_targets.data(),
                                                   bomb_targets.size()));
    auto bomb_hash = submit_bomb_blob(id, conn, ns_span,
        std::move(bomb_data),
        now, rid_counter++,
        opts.host);

    conn.close();

    if (!bomb_hash) {
        std::fprintf(stderr, "Error: BOMB submission failed\n");
        return 1;
    }

    if (!opts.quiet) {
        std::fprintf(stderr, "BOMB %s tombstoned %zu target(s)\n",
                     to_hex(*bomb_hash).c_str(), bomb_targets.size());
        // D-06 cascade summary — only print when a cascade actually happened.
        if (manifests_full > 0 || manifests_failed > 0) {
            std::fprintf(stderr,
                "cascade: %zu manifests fully tombstoned, %zu manifests failed to fetch (manifest-only)\n",
                manifests_full, manifests_failed);
        }
    }
    std::printf("%s\n", to_hex(*bomb_hash).c_str());
    return 0;
}

// =============================================================================
// D-09 NAME resolution helpers + cmd::get_by_name
// =============================================================================

namespace {

// Parse a ListResponse-style entry stream (TYPE-03 layout, 60 bytes
// per entry). Emits `(hash, seq, timestamp)` triples for every entry whose
// 4-byte type matches `type_prefix`. The list-endpoint is already type-filtered
// server-side, but we double-check here defense-in-depth.
struct NameListEntry {
    std::array<uint8_t, 32> blob_hash{};
    uint64_t seq = 0;
    uint64_t timestamp = 0;
};

std::vector<NameListEntry> enumerate_name_blobs(
    Connection& conn, std::span<const uint8_t, 32> ns) {

    std::vector<NameListEntry> out;
    uint64_t since_seq = 0;
    constexpr uint32_t kPageLimit = 1000;  // plan "reasonable cap"
    uint32_t rid = 0x1000;  // avoid colliding with other in-flight rids

    for (;;) {
        // ListRequest payload layout:
        //   [ns:32][since_seq:8BE][limit:4BE][flags:1][type_filter:4]
        std::vector<uint8_t> payload(49, 0);
        std::memcpy(payload.data(), ns.data(), 32);
        store_u64_be(payload.data() + 32, since_seq);
        store_u32_be(payload.data() + 40, kPageLimit);
        payload[44] = 0x02;  // type_filter present (D-09)
        std::memcpy(payload.data() + 45, NAME_MAGIC_CLI.data(), 4);

        if (!conn.send(MsgType::ListRequest, payload, rid++)) return out;
        auto resp = conn.recv();
        if (!resp || resp->type != static_cast<uint8_t>(MsgType::ListResponse) ||
            resp->payload.size() < 5) {
            return out;
        }

        auto& p = resp->payload;
        uint32_t count = load_u32_be(p.data());
        size_t entries_size = static_cast<size_t>(count) * LIST_ENTRY_SIZE;
        if (p.size() < 4 + entries_size + 1) return out;

        const uint8_t* entries = p.data() + 4;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* entry = entries + static_cast<size_t>(i) * LIST_ENTRY_SIZE;
            const uint8_t* type_ptr = entry + 40;
            if (std::memcmp(type_ptr, NAME_MAGIC_CLI.data(), 4) != 0) {
                // Server ignored our filter — skip defensively.
                since_seq = load_u64_be(entry + 32);
                continue;
            }
            NameListEntry e;
            std::memcpy(e.blob_hash.data(), entry, 32);
            e.seq       = load_u64_be(entry + 32);
            e.timestamp = load_u64_be(entry + 52);
            out.push_back(e);
            since_seq = e.seq;
        }

        uint8_t has_more = p[4 + entries_size];
        if (has_more == 0 || count == 0) break;
    }

    return out;
}

// Fetch a single blob body via ReadRequest. Returns nullopt on any RPC or
// decode error (caller treats missing entries as "ignore and move on").
std::optional<BlobData> fetch_blob(Connection& conn,
                                    std::span<const uint8_t, 32> ns,
                                    std::span<const uint8_t, 32> hash,
                                    uint32_t rid) {
    std::vector<uint8_t> payload(64);
    std::memcpy(payload.data(),      ns.data(),   32);
    std::memcpy(payload.data() + 32, hash.data(), 32);

    if (!conn.send(MsgType::ReadRequest, payload, rid)) return std::nullopt;
    auto resp = conn.recv();
    if (!resp || resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
        resp->payload.empty() || resp->payload[0] != 0x01) {
        return std::nullopt;
    }
    return decode_blob(std::span<const uint8_t>(
        resp->payload.data() + 1, resp->payload.size() - 1));
}

// Shared D-01/D-02 winner selection: pass in a vector of candidate NAME blobs
// (already filtered to matching name), return the winner's BlobData. Nullopt
// if the input is empty.
struct NameCandidate {
    std::array<uint8_t, 32> blob_hash{};  // hash of the NAME blob itself
    uint64_t timestamp = 0;
    std::array<uint8_t, 32> target_hash{};  // from the parsed NAME payload
};

std::optional<NameCandidate> pick_name_winner(std::vector<NameCandidate> v) {
    if (v.empty()) return std::nullopt;
    std::sort(v.begin(), v.end(),
        [](const NameCandidate& a, const NameCandidate& b) {
            if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
            // D-02: content_hash DESC tiebreak. Plan text says
            // "content_hash DESC" meaning we sort the NAME blob's own
            // hash (the list-entry hash) DESC — that's what's
            // reproducible across clients since the NAME blob's hash is
            // what ListRequest returns.
            return std::memcmp(a.blob_hash.data(), b.blob_hash.data(), 32) > 0;
        });
    return v.front();
}

} // anonymous namespace

// Implementation of the forward declaration above cmd::put. Operates on a
// caller-owned connection; does NOT close it. Uses its own rid range so it
// doesn't clash with the caller's pipeline.
static std::optional<std::array<uint8_t, 32>> resolve_name_to_target_hash(
    Identity& id, Connection& conn,
    std::span<const uint8_t, 32> ns, const std::string& name) {

    (void)id;  // signature symmetry — Identity not currently needed here.

    auto refs = enumerate_name_blobs(conn, ns);
    if (refs.empty()) return std::nullopt;

    std::vector<NameCandidate> matches;
    uint32_t rid = 0x2000;
    for (const auto& ref : refs) {
        auto blob = fetch_blob(conn, ns,
            std::span<const uint8_t, 32>(ref.blob_hash.data(), 32), rid++);
        if (!blob) continue;

        auto parsed = parse_name_payload(blob->data);
        if (!parsed) continue;

        // memcmp on opaque bytes per D-04 — names are not strings.
        if (parsed->name.size() != name.size()) continue;
        if (std::memcmp(parsed->name.data(), name.data(), name.size()) != 0) continue;

        NameCandidate c;
        c.blob_hash   = ref.blob_hash;
        c.timestamp   = blob->timestamp;   // outer blob.timestamp IS the seq (D-01)
        c.target_hash = parsed->target_hash;
        matches.push_back(c);
    }

    auto winner = pick_name_winner(std::move(matches));
    if (!winner) return std::nullopt;
    return winner->target_hash;
}

int get_by_name(const std::string& identity_dir, const std::string& name,
                const std::string& namespace_hex, bool to_stdout,
                const std::string& output_dir, bool force_overwrite,
                const ConnectOpts& opts) {

    if (name.empty()) {
        std::fprintf(stderr, "Error: get_by_name requires a non-empty name\n");
        return 1;
    }
    if (name.size() > 65535) {
        std::fprintf(stderr, "Error: name length %zu exceeds 65535 bytes\n", name.size());
        return 1;
    }

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    std::span<const uint8_t, 32> ns_s(ns.data(), 32);
    auto target_hash = resolve_name_to_target_hash(id, conn, ns_s, name);
    conn.close();

    if (!target_hash) {
        std::fprintf(stderr, "Error: name not found: %s\n", name.c_str());
        return 1;
    }

    // Delegate the content fetch to the existing cmd::get path — it already
    // handles CENV decrypt, CPAR chunked manifests, stdout/output-dir, and
    // force-overwrite. This keeps the NAME → content_hash → file pipeline a
    // single code path (feedback_no_duplicate_code.md).
    std::vector<std::string> hash_hexes{to_hex(*target_hash)};
    std::string ns_hex_for_get = namespace_hex;  // preserve caller's choice
    return get(identity_dir, hash_hexes, ns_hex_for_get, to_stdout,
               output_dir, force_overwrite, opts);
}

// =============================================================================
// reshare
// =============================================================================

int reshare(const std::string& identity_dir, const std::string& hash_hex,
            const std::string& namespace_hex,
            const std::vector<std::string>& share_pubkey_files,
            uint32_t ttl, const ConnectOpts& opts) {

    // Step 1: Fetch and decrypt the original blob
    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);
    auto hash = parse_hash(hash_hex);

    // -- GET --
    std::vector<uint8_t> read_payload(64);
    std::memcpy(read_payload.data(), ns.data(), 32);
    std::memcpy(read_payload.data() + 32, hash.data(), 32);

    Connection conn1(id);
    if (!conn1.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect (get)\n");
        return 1;
    }

    if (!conn1.send(MsgType::ReadRequest, read_payload, 1)) {
        std::fprintf(stderr, "Error: failed to send ReadRequest\n");
        conn1.close();
        return 1;
    }

    auto read_resp = conn1.recv();
    conn1.close();

    if (!read_resp || read_resp->type != static_cast<uint8_t>(MsgType::ReadResponse)) {
        std::fprintf(stderr, "Error: failed to read blob\n");
        return 1;
    }

    if (read_resp->payload.empty() || read_resp->payload[0] != 0x01) {
        std::fprintf(stderr, "Error: blob not found\n");
        return 1;
    }

    auto blob_bytes = std::span<const uint8_t>(
        read_resp->payload.data() + 1, read_resp->payload.size() - 1);

    auto blob = decode_blob(blob_bytes);
    if (!blob) {
        std::fprintf(stderr, "Error: failed to decode blob\n");
        return 1;
    }

    std::vector<uint8_t> plaintext;
    if (envelope::is_envelope(blob->data)) {
        auto decrypted = envelope::decrypt(blob->data, id.kem_seckey(), id.kem_pubkey());
        if (!decrypted) {
            std::fprintf(stderr, "Error: cannot decrypt original blob\n");
            return 1;
        }
        plaintext = std::move(*decrypted);
    } else {
        plaintext = blob->data;
    }

    // Step 2: Re-encrypt with new recipients
    std::vector<std::span<const uint8_t>> recipient_spans;
    auto external_pks = load_recipient_kem_pubkeys(share_pubkey_files, identity_dir);

    auto self_kem_pk = id.kem_pubkey();
    recipient_spans.emplace_back(self_kem_pk);

    for (auto& pk : external_pks) {
        recipient_spans.emplace_back(std::span<const uint8_t>(pk));
    }

    auto new_envelope = envelope::encrypt(plaintext, recipient_spans);

    // Step 3: PUT the new blob
    auto ns_span = std::span<const uint8_t, 32>(ns.data(), 32);
    auto timestamp = static_cast<uint64_t>(std::time(nullptr));
    auto new_ns_blob  = build_owned_blob(id, ns_span, new_envelope, ttl, timestamp);
    auto new_envelope_bytes = encode_blob_write_body(new_ns_blob.target_namespace, new_ns_blob.blob);

    Connection conn2(id);
    if (!conn2.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect (put)\n");
        return 1;
    }

    // D-01: auto-PUBK probe+emit on conn2 before the reshare's BlobWrite.
    // The shared invocation-scoped cache means conn3's tombstone Send below
    // inherits the cache hit (if this probe emitted, or if prior commands in
    // the same process already marked own_ns present).
    {
        uint32_t pubk_rid = 0x2000;
        if (!ensure_pubk(id, conn2, ns_span, pubk_rid)) {
            std::fprintf(stderr,
                "Error: failed to ensure namespace is published on node %s\n",
                opts.host.c_str());
            conn2.close();
            return 1;
        }
    }

    if (!conn2.send(MsgType::BlobWrite, new_envelope_bytes, 1)) {
        std::fprintf(stderr, "Error: failed to send\n");
        conn2.close();
        return 1;
    }

    auto write_resp = conn2.recv();
    conn2.close();

    if (!write_resp || write_resp->type != static_cast<uint8_t>(MsgType::WriteAck)) {
        std::fprintf(stderr, "Error: put failed\n");
        return 1;
    }

    if (write_resp->payload.size() < 41) {
        std::fprintf(stderr, "Error: invalid WriteAck\n");
        return 1;
    }

    auto new_hash_span = std::span<const uint8_t>(write_resp->payload.data(), 32);
    auto new_hash_hex = to_hex(new_hash_span);

    // Step 4: Delete the old blob
    auto tombstone_data = make_tombstone_data(
        std::span<const uint8_t, 32>(hash.data(), 32));

    auto del_timestamp = static_cast<uint64_t>(std::time(nullptr));
    auto del_ns_blob   = build_owned_blob(id, ns_span, tombstone_data, 0, del_timestamp);
    auto del_envelope  = encode_blob_write_body(del_ns_blob.target_namespace, del_ns_blob.blob);

    Connection conn3(id);
    if (!conn3.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Warning: failed to connect for delete (new blob stored)\n");
        std::printf("%s\n", new_hash_hex.c_str());
        return 0;
    }

    if (!conn3.send(MsgType::Delete, del_envelope, 1)) {
        std::fprintf(stderr, "Warning: failed to send Delete (new blob stored)\n");
        conn3.close();
        std::printf("%s\n", new_hash_hex.c_str());
        return 0;
    }

    auto del_resp = conn3.recv();
    conn3.close();

    if (!del_resp || del_resp->type != static_cast<uint8_t>(MsgType::DeleteAck)) {
        std::fprintf(stderr, "Warning: delete of old blob failed (new blob stored)\n");
    }

    std::printf("%s\n", new_hash_hex.c_str());
    return 0;
}

// =============================================================================
// ls
// =============================================================================

int ls(const std::string& identity_dir, const std::string& namespace_hex,
       const ConnectOpts& opts, bool raw, const std::string& type_filter) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);

    uint64_t since_seq = 0;
    bool first_page = true;

    for (;;) {
        // Determine ListRequest payload size based on flags needed
        // Per D-07: 44 bytes = no flags, 45 = flags, 49 = flags + type_filter
        bool send_type_filter = !type_filter.empty();
        size_t payload_size = 44;
        uint8_t flags = 0;

        if (raw) {
            flags |= 0x01;  // bit 0: include_all (per D-08)
            payload_size = 45;
        }
        if (send_type_filter) {
            flags |= 0x02;  // bit 1: type_filter present (per D-09)
            payload_size = 49;
        } else if (raw) {
            payload_size = 45;
        }

        std::vector<uint8_t> payload(payload_size);
        std::memcpy(payload.data(), ns.data(), 32);
        store_u64_be(payload.data() + 32, since_seq);
        store_u32_be(payload.data() + 40, 100);

        if (payload_size >= 45) {
            payload[44] = flags;
        }
        if (send_type_filter) {
            // Map type label string back to magic bytes for server-side filter
            std::array<uint8_t, 4> filter_bytes{};
            if (type_filter == "CENV") filter_bytes = CENV_MAGIC;
            else if (type_filter == "PUBK") filter_bytes = PUBKEY_MAGIC;
            else if (type_filter == "TOMB") filter_bytes = TOMBSTONE_MAGIC_CLI;
            else if (type_filter == "DLGT") filter_bytes = DELEGATION_MAGIC_CLI;
            else if (type_filter == "CDAT") filter_bytes = CDAT_MAGIC;
            else if (type_filter == "CPAR") filter_bytes = CPAR_MAGIC;
            // Rule-2 add: NAME and BOMB are valid type filters;
            // the node's type_filter already honours their 4-byte magic values
            // (db/tests/peer/test_list_by_magic.cpp covers both). Exposing them
            // here lets users `ls --raw --type BOMB` to enumerate batch tombstones
            // and `--type NAME` to enumerate name pointers — required for the
            // D-08 E2E matrix.
            else if (type_filter == "NAME") filter_bytes = NAME_MAGIC_CLI;
            else if (type_filter == "BOMB") filter_bytes = BOMB_MAGIC_CLI;
            else {
                std::fprintf(stderr, "Error: unknown type '%s'. Known: CENV, PUBK, TOMB, DLGT, CDAT, CPAR, NAME, BOMB\n",
                             type_filter.c_str());
                return 1;
            }
            std::memcpy(payload.data() + 45, filter_bytes.data(), 4);
        }

        Connection conn(id);
        if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
            std::fprintf(stderr, "Error: failed to connect\n");
            return 1;
        }

        if (!conn.send(MsgType::ListRequest, payload, 1)) {
            std::fprintf(stderr, "Error: failed to send ListRequest\n");
            conn.close();
            return 1;
        }

        auto resp = conn.recv();
        conn.close();

        if (!resp) {
            std::fprintf(stderr, "Error: no response\n");
            return 1;
        }

        if (resp->type == static_cast<uint8_t>(MsgType::ErrorResponse)) {
            std::fprintf(stderr, "%s\n",
                decode_error_response(resp->payload, opts.host,
                    std::span<const uint8_t, 32>(ns.data(), 32)).c_str());
            return 1;
        }

        if (resp->type != static_cast<uint8_t>(MsgType::ListResponse)) {
            std::fprintf(stderr, "Error: unexpected response type %u\n", resp->type);
            return 1;
        }

        // Parse ListResponse:
        //   [count:4BE]
        //   [entries: N * (hash:32 | seq:8BE | type:4 | size:8BE | ts:8BE)]
        //   [has_more:1]
        auto& p = resp->payload;
        if (p.size() < 5) {  // at least count(4) + has_more(1)
            std::fprintf(stderr, "Error: ListResponse too short\n");
            return 1;
        }

        uint32_t count = load_u32_be(p.data());
        size_t entries_size = static_cast<size_t>(count) * LIST_ENTRY_SIZE;
        if (p.size() < 4 + entries_size + 1) {
            std::fprintf(stderr, "Error: ListResponse truncated\n");
            return 1;
        }

        // Header (only on the first page — suppress on subsequent pagination turns).
        if (first_page) {
            std::printf("%-64s  %-4s  %10s  %s\n",
                        "HASH", "TYPE", "SIZE", "TIMESTAMP");
            first_page = false;
        }

        const uint8_t* entries = p.data() + 4;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* entry = entries + static_cast<size_t>(i) * LIST_ENTRY_SIZE;
            auto hash_span = std::span<const uint8_t>(entry, 32);
            uint64_t seq = load_u64_be(entry + 32);
            const uint8_t* type_ptr = entry + 40;
            uint64_t size = load_u64_be(entry + 44);
            uint64_t ts   = load_u64_be(entry + 52);

            // Per D-21: --type bypasses hide list
            // Per D-13/D-22: default mode hides PUBK, CDAT, DLGT
            if (!raw && type_filter.empty() && is_hidden_type(type_ptr)) {
                since_seq = seq;
                continue;
            }

            // Format timestamp as "YYYY-MM-DD HH:MM:SS" UTC.
            char tsbuf[32] = "-";
            if (ts > 0) {
                std::time_t t = static_cast<std::time_t>(ts);
                std::tm tm_utc{};
                if (gmtime_r(&t, &tm_utc)) {
                    std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tm_utc);
                }
            }

            // Size: humanize only for non-tiny values; keep an aligned column.
            char sizebuf[24];
            if (size < 1024) {
                std::snprintf(sizebuf, sizeof(sizebuf), "%llu", (unsigned long long)size);
            } else {
                double v = size;
                const char* u = "K";
                if (v >= 1024.0 * 1024.0) { v /= 1024.0 * 1024.0; u = "M"; }
                else                      { v /= 1024.0;          u = "K"; }
                if (v >= 1024.0)          { v /= 1024.0;          u = "G"; }
                std::snprintf(sizebuf, sizeof(sizebuf), "%.1f%s", v, u);
            }

            std::printf("%s  %-4s  %10s  %s\n",
                        to_hex(hash_span).c_str(), type_label(type_ptr),
                        sizebuf, tsbuf);

            since_seq = seq;
        }

        uint8_t has_more = p[4 + entries_size];
        if (has_more == 0 || count == 0) {
            break;
        }
    }

    return 0;
}

// =============================================================================
// list_hashes — return all blob hashes in a namespace
// =============================================================================

std::vector<std::string> list_hashes(const std::string& identity_dir,
                                      const std::string& namespace_hex,
                                      const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);

    std::vector<std::string> hashes;
    uint64_t since_seq = 0;

    for (;;) {
        std::vector<uint8_t> payload(44);
        std::memcpy(payload.data(), ns.data(), 32);
        store_u64_be(payload.data() + 32, since_seq);
        store_u32_be(payload.data() + 40, 100);

        Connection conn(id);
        if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
            return hashes;
        }

        if (!conn.send(MsgType::ListRequest, payload, 1)) {
            conn.close();
            return hashes;
        }

        auto resp = conn.recv();
        conn.close();

        if (!resp || resp->type != static_cast<uint8_t>(MsgType::ListResponse) ||
            resp->payload.size() < 5) {
            return hashes;
        }

        auto& p = resp->payload;

        uint32_t count = load_u32_be(p.data());
        size_t entries_size = static_cast<size_t>(count) * LIST_ENTRY_SIZE;
        if (p.size() < 4 + entries_size + 1) return hashes;

        const uint8_t* entries = p.data() + 4;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* entry = entries + static_cast<size_t>(i) * LIST_ENTRY_SIZE;
            auto hash_span = std::span<const uint8_t>(entry, 32);
            hashes.push_back(to_hex(hash_span));
            since_seq = load_u64_be(entry + 32);
        }

        uint8_t has_more = p[4 + entries_size];
        if (has_more == 0 || count == 0) break;
    }

    return hashes;
}

// =============================================================================
// exists
// =============================================================================

int exists(const std::string& identity_dir, const std::string& hash_hex,
           const std::string& namespace_hex, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);
    auto hash = parse_hash(hash_hex);

    // ExistsRequest: [namespace:32][hash:32] = 64 bytes
    std::vector<uint8_t> payload(64);
    std::memcpy(payload.data(), ns.data(), 32);
    std::memcpy(payload.data() + 32, hash.data(), 32);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::ExistsRequest, payload, 1)) {
        std::fprintf(stderr, "Error: failed to send ExistsRequest\n");
        conn.close();
        return 1;
    }

    auto resp = conn.recv();
    conn.close();

    if (!resp) {
        std::fprintf(stderr, "Error: no response\n");
        return 1;
    }

    if (resp->type == static_cast<uint8_t>(MsgType::ErrorResponse)) {
        std::fprintf(stderr, "%s\n",
            decode_error_response(resp->payload, opts.host,
                std::span<const uint8_t, 32>(ns.data(), 32)).c_str());
        return 1;
    }

    if (resp->type != static_cast<uint8_t>(MsgType::ExistsResponse)) {
        std::fprintf(stderr, "Error: unexpected response type %u\n", resp->type);
        return 1;
    }

    // ExistsResponse: [exists:1][hash:32] = 33 bytes
    if (resp->payload.size() < 33) {
        std::fprintf(stderr, "Error: invalid ExistsResponse (%zu bytes)\n",
                      resp->payload.size());
        return 1;
    }

    if (resp->payload[0] == 0x01) {
        std::printf("exists\n");
        return 0;
    } else {
        std::printf("not found\n");
        return 1;
    }
}

// =============================================================================
// info
// =============================================================================

int info(const std::string& identity_dir, const ConnectOpts& opts) {
    auto id = Identity::load_from(identity_dir);

    // NodeInfoRequest: empty payload
    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::NodeInfoRequest, {}, 1)) {
        std::fprintf(stderr, "Error: failed to send NodeInfoRequest\n");
        conn.close();
        return 1;
    }

    auto resp = conn.recv();
    conn.close();

    if (!resp) {
        std::fprintf(stderr, "Error: no response\n");
        return 1;
    }

    if (resp->type == static_cast<uint8_t>(MsgType::ErrorResponse)) {
        // cmd::info is global (no namespace in scope); pass zero-filled
        // ns_hint — decoder's short-handle will be all zeros for codes that
        // mention it (0x08), and cmd::info cannot trigger 0x08 anyway since
        // it's a pure read. The decoder wording stays correct either way.
        std::array<uint8_t, 32> zero_ns{};
        std::fprintf(stderr, "%s\n",
            decode_error_response(resp->payload, opts.host,
                std::span<const uint8_t, 32>(zero_ns.data(), 32)).c_str());
        return 1;
    }

    if (resp->type != static_cast<uint8_t>(MsgType::NodeInfoResponse)) {
        std::fprintf(stderr, "Error: unexpected response type %u\n", resp->type);
        return 1;
    }

    // Parse NodeInfoResponse (wire layout):
    //   [version_len:1][version:version_len bytes]
    //   [uptime:8 BE]
    //   [peer_count:4 BE][namespace_count:4 BE]
    //   [total_blobs:8 BE]
    //   [storage_used:8 BE][storage_max:8 BE]
    //   [max_blob_data_bytes:8 BE]
    //   [max_frame_bytes:4 BE]
    //   [rate_limit_bytes_per_sec:8 BE]
    //   [max_subscriptions_per_connection:4 BE]
    //   [types_count:1][supported_types:types_count bytes]
    auto& p = resp->payload;
    size_t off = 0;

    auto read_u8 = [&]() -> uint8_t {
        if (off >= p.size()) throw std::runtime_error("NodeInfoResponse truncated");
        return p[off++];
    };
    auto read_string = [&](uint8_t len) -> std::string {
        if (off + len > p.size()) throw std::runtime_error("NodeInfoResponse truncated");
        std::string s(reinterpret_cast<const char*>(p.data() + off), len);
        off += len;
        return s;
    };
    auto read_u32 = [&]() -> uint32_t {
        if (off + 4 > p.size()) throw std::runtime_error("NodeInfoResponse truncated");
        auto val = load_u32_be(p.data() + off);
        off += 4;
        return val;
    };
    auto read_u64 = [&]() -> uint64_t {
        if (off + 8 > p.size()) throw std::runtime_error("NodeInfoResponse truncated");
        auto val = load_u64_be(p.data() + off);
        off += 8;
        return val;
    };

    uint8_t ver_len = read_u8();
    auto version = read_string(ver_len);

    auto uptime = read_u64();
    auto peer_count = read_u32();
    auto namespace_count = read_u32();
    auto total_blobs = read_u64();
    auto storage_used = read_u64();
    auto storage_max = read_u64();
    auto max_blob_data_bytes = read_u64();
    auto max_frame_bytes = read_u32();
    auto rate_limit_bytes_per_sec = read_u64();
    auto max_subscriptions = read_u32();

    std::printf("Version:    %s\n", version.c_str());
    std::printf("Uptime:     %s\n", humanize_uptime(uptime).c_str());
    std::printf("Peers:      %u\n", peer_count);
    std::printf("Namespaces: %u\n", namespace_count);
    std::printf("Blobs:      %llu\n", (unsigned long long)total_blobs);
    std::printf("Used:       %s\n", humanize_bytes(storage_used).c_str());
    if (storage_max == 0) {
        std::printf("Quota:      unlimited\n");
    } else {
        std::printf("Quota:      %s\n", humanize_bytes(storage_max).c_str());
    }
    std::printf("Max blob:   %s\n", humanize_bytes(max_blob_data_bytes).c_str());
    std::printf("Max frame:  %s\n", humanize_bytes(max_frame_bytes).c_str());
    if (rate_limit_bytes_per_sec == 0) {
        std::printf("Rate limit: unlimited\n");
    } else {
        std::printf("Rate limit: %s/s\n", humanize_bytes(rate_limit_bytes_per_sec).c_str());
    }
    if (max_subscriptions == 0) {
        std::printf("Max subs:   unlimited\n");
    } else {
        std::printf("Max subs:   %u\n", max_subscriptions);
    }

    return 0;
}

// =============================================================================
// stats
// =============================================================================

int stats(const std::string& identity_dir, const ConnectOpts& opts) {
    auto id = Identity::load_from(identity_dir);
    auto ns = id.namespace_id();

    // StatsRequest: [namespace:32]
    std::vector<uint8_t> payload(32);
    std::memcpy(payload.data(), ns.data(), 32);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::StatsRequest, payload, 1)) {
        std::fprintf(stderr, "Error: failed to send StatsRequest\n");
        conn.close();
        return 1;
    }

    auto resp = conn.recv();
    conn.close();

    if (!resp) {
        std::fprintf(stderr, "Error: no response\n");
        return 1;
    }

    if (resp->type == static_cast<uint8_t>(MsgType::ErrorResponse)) {
        std::fprintf(stderr, "%s\n",
            decode_error_response(resp->payload, opts.host,
                std::span<const uint8_t, 32>(ns.data(), 32)).c_str());
        return 1;
    }

    if (resp->type != static_cast<uint8_t>(MsgType::StatsResponse)) {
        std::fprintf(stderr, "Error: unexpected response type %u\n", resp->type);
        return 1;
    }

    // StatsResponse: [count:8BE][bytes:8BE][quota:8BE] = 24 bytes
    if (resp->payload.size() < 24) {
        std::fprintf(stderr, "Error: invalid StatsResponse (%zu bytes)\n",
                      resp->payload.size());
        return 1;
    }

    auto count = load_u64_be(resp->payload.data());
    auto bytes = load_u64_be(resp->payload.data() + 8);
    auto quota = load_u64_be(resp->payload.data() + 16);

    std::printf("Blobs: %llu\n", (unsigned long long)count);
    std::printf("Size:  %s\n", humanize_bytes(bytes).c_str());
    if (quota == 0) {
        std::printf("Quota: unlimited\n");
    } else {
        std::printf("Quota: %s\n", humanize_bytes(quota).c_str());
    }

    return 0;
}

// =============================================================================
// delegate — grant write access to another identity
// =============================================================================

int delegate(const std::string& identity_dir, const std::string& target,
             const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto targets = resolve_signing_targets(target, identity_dir);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    auto ns = id.namespace_id();

    // D-01: auto-PUBK probe+emit before the delegation-blob loop. target_ns is
    // own_ns (delegator writes DLGT blob in their OWN namespace).
    {
        uint32_t pubk_rid = 0x2000;
        if (!ensure_pubk(id, conn, ns, pubk_rid)) {
            std::fprintf(stderr,
                "Error: failed to ensure namespace is published on node %s\n",
                opts.host.c_str());
            conn.close();
            return 1;
        }
    }

    uint32_t rid = 1;
    int errors = 0;

    for (const auto& t : targets) {
        auto delegation_data = make_delegation_data(t.signing_pk);

        auto timestamp = static_cast<uint64_t>(std::time(nullptr));
        // ttl=0 — delegation is permanent until revoked.
        auto ns_blob   = build_owned_blob(id, ns, delegation_data, 0, timestamp);
        auto envelope  = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

        if (!conn.send(MsgType::BlobWrite, envelope, rid++)) {
            std::fprintf(stderr, "Error: failed to send delegation for %s\n", t.label.c_str());
            ++errors;
            continue;
        }

        auto resp = conn.recv();
        if (!resp || resp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
            resp->payload.size() < 41) {
            std::fprintf(stderr, "Error: bad response for %s\n", t.label.c_str());
            ++errors;
            continue;
        }

        auto hash_span = std::span<const uint8_t>(resp->payload.data(), 32);
        auto hash_hex = to_hex(hash_span);
        if (!opts.quiet) {
            std::fprintf(stderr, "delegated: %s\n", t.label.c_str());
        }
        std::printf("%s\n", hash_hex.c_str());
    }

    conn.close();
    return errors > 0 ? 1 : 0;
}

// =============================================================================
// revoke — revoke write access by tombstoning delegation blob
// =============================================================================

int revoke(const std::string& identity_dir, const std::string& target,
           bool skip_confirm, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto targets = resolve_signing_targets(target, identity_dir);
    auto ns = id.namespace_id();

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    // D-01: auto-PUBK probe+emit before the revoke tombstones. Revoke writes
    // are in the writer's own namespace.
    {
        uint32_t pubk_rid = 0x2000;
        if (!ensure_pubk(id, conn, ns, pubk_rid)) {
            std::fprintf(stderr,
                "Error: failed to ensure namespace is published on node %s\n",
                opts.host.c_str());
            conn.close();
            return 1;
        }
    }

    // Fetch current delegations once so we can map target signing_pk → blob hash
    // and skip targets that aren't delegated (P5: no prompt for non-delegates).
    std::vector<uint8_t> list_payload(ns.data(), ns.data() + 32);
    if (!conn.send(MsgType::DelegationListRequest, list_payload, 1)) {
        std::fprintf(stderr, "Error: failed to send DelegationListRequest\n");
        conn.close();
        return 1;
    }

    auto list_resp = conn.recv();
    if (!list_resp ||
        list_resp->type != static_cast<uint8_t>(MsgType::DelegationListResponse) ||
        list_resp->payload.size() < 4) {
        std::fprintf(stderr, "Error: bad DelegationListResponse\n");
        conn.close();
        return 1;
    }

    uint32_t count = load_u32_be(list_resp->payload.data());
    if (list_resp->payload.size() < 4 + count * 64) {
        std::fprintf(stderr, "Error: truncated DelegationListResponse\n");
        conn.close();
        return 1;
    }

    // Collect matches: (target, delegation_blob_hash).
    struct Match {
        SigningTarget target;
        std::array<uint8_t, 32> blob_hash{};
    };
    std::vector<Match> matches;

    for (const auto& t : targets) {
        auto pk_hash = sha3_256(t.signing_pk);
        for (uint32_t i = 0; i < count; ++i) {
            size_t off = 4 + i * 64;
            if (std::equal(pk_hash.data(), pk_hash.data() + 32,
                           list_resp->payload.data() + off)) {
                Match m;
                m.target = t;
                std::memcpy(m.blob_hash.data(),
                            list_resp->payload.data() + off + 32, 32);
                matches.push_back(std::move(m));
                break;
            }
        }
    }

    if (matches.empty()) {
        std::fprintf(stderr, "Error: no delegations found for: %s\n", target.c_str());
        conn.close();
        return 1;
    }

    // Confirm unless -y. Show exactly what will be revoked.
    if (!skip_confirm) {
        std::fprintf(stderr, "Revoke delegation for:\n");
        for (const auto& m : matches) {
            std::fprintf(stderr, "  %s\n", m.target.label.c_str());
        }
        std::fprintf(stderr, "Proceed? [y/N] ");
        int ch = std::fgetc(stdin);
        if (ch != 'y' && ch != 'Y') {
            std::fprintf(stderr, "Aborted.\n");
            conn.close();
            return 0;
        }
    }

    uint32_t rid = 2;
    int errors = 0;
    for (const auto& m : matches) {
        auto tombstone_data = make_tombstone_data(
            std::span<const uint8_t, 32>(m.blob_hash.data(), 32));

        auto timestamp = static_cast<uint64_t>(std::time(nullptr));
        auto ns_blob   = build_owned_blob(id, ns, tombstone_data, 0, timestamp);
        auto envelope  = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

        if (!conn.send(MsgType::Delete, envelope, rid++)) {
            std::fprintf(stderr, "Error: failed to send Delete for %s\n",
                         m.target.label.c_str());
            ++errors;
            continue;
        }

        auto del_resp = conn.recv();
        if (!del_resp || del_resp->type != static_cast<uint8_t>(MsgType::DeleteAck)) {
            std::fprintf(stderr, "Error: bad DeleteAck for %s\n",
                         m.target.label.c_str());
            ++errors;
            continue;
        }

        if (!opts.quiet) {
            std::fprintf(stderr, "revoked: %s\n", m.target.label.c_str());
        }
    }

    conn.close();
    return errors > 0 ? 1 : 0;
}

// =============================================================================
// delegations — list active delegations
// =============================================================================

int delegations(const std::string& identity_dir, const std::string& namespace_hex,
                const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    std::vector<uint8_t> payload(ns.data(), ns.data() + 32);
    if (!conn.send(MsgType::DelegationListRequest, payload, 1)) {
        std::fprintf(stderr, "Error: failed to send DelegationListRequest\n");
        conn.close();
        return 1;
    }

    auto resp = conn.recv();
    conn.close();

    if (!resp || resp->type != static_cast<uint8_t>(MsgType::DelegationListResponse) ||
        resp->payload.size() < 4) {
        std::fprintf(stderr, "Error: bad response\n");
        return 1;
    }

    uint32_t count = load_u32_be(resp->payload.data());
    if (count == 0) {
        if (!opts.quiet) std::fprintf(stderr, "no delegations\n");
        return 0;
    }

    // Build delegate-namespace → contact-name lookup from the local contacts db.
    std::map<std::string, std::string> ns_to_name;
    {
        auto db_path = identity_dir + "/contacts.db";
        if (fs::exists(db_path)) {
            ContactDB db(db_path);
            for (const auto& c : db.list()) {
                if (c.signing_pk.empty()) continue;
                auto ns_arr = sha3_256(c.signing_pk);
                ns_to_name[to_hex(std::span<const uint8_t>(ns_arr.data(), 32))] = c.name;
            }
        }
    }

    std::printf("%-20s  %-64s  %s\n", "DELEGATE", "NAMESPACE", "BLOB");
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 4 + i * 64;
        auto pk_hash = std::span<const uint8_t>(resp->payload.data() + off, 32);
        auto blob_hash = std::span<const uint8_t>(resp->payload.data() + off + 32, 32);
        auto ns_hex = to_hex(pk_hash);
        const auto it = ns_to_name.find(ns_hex);
        const std::string name = it != ns_to_name.end() ? it->second : "(unknown)";
        std::printf("%-20s  %s  %s\n", name.c_str(), ns_hex.c_str(), to_hex(blob_hash).c_str());
    }

    return 0;
}

// =============================================================================
// publish — store our pubkey blob on the node
// =============================================================================

int publish(const std::string& identity_dir, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = id.namespace_id();
    auto ns_hex = to_hex(ns);

    // Already published? Fetch via server-side PUBK type filter.
    {
        Connection check_conn(id);
        if (check_conn.connect(opts.host, opts.port, opts.uds_path)) {
            auto existing = find_pubkey_blob(check_conn, ns);
            check_conn.close();
            if (existing) {
                if (!opts.quiet) {
                    std::fprintf(stderr, "already published\n");
                }
                std::printf("%s\n", ns_hex.c_str());
                return 0;
            }
        }
    }

    // Build PUBK blob: [magic:4][signing_pk:2592][kem_pk:1568]
    auto pubkey_data = make_pubkey_data(id.signing_pubkey(), id.kem_pubkey());

    // Sign as permanent blob in our namespace (ttl=0).
    auto timestamp = static_cast<uint64_t>(std::time(nullptr));
    auto ns_blob   = build_owned_blob(id, ns, pubkey_data, 0, timestamp);
    auto envelope  = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::BlobWrite, envelope, 1)) {
        std::fprintf(stderr, "Error: failed to send\n");
        conn.close();
        return 1;
    }

    auto resp = conn.recv();
    conn.close();

    if (!resp || resp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
        resp->payload.size() < 41) {
        std::fprintf(stderr, "Error: bad response\n");
        return 1;
    }

    // RESEARCH Open Q #1 (RESOLVED): cmd::publish bypasses ensure_pubk (chicken-
    // and-egg — publish IS the PUBK writer). After a successful WriteAck, seed
    // the invocation-scoped cache so subsequent owner-writes in the same
    // process skip the probe.
    mark_pubk_present_for_invocation(std::span<const uint8_t, 32>(ns.data(), 32));

    if (!opts.quiet) {
        std::fprintf(stderr, "published\n");
    }
    std::printf("%s\n", ns_hex.c_str());

    return 0;
}

// =============================================================================
// contact add — fetch pubkey from node by namespace, save to contacts db
// =============================================================================

int contact_add(const std::string& identity_dir, const std::string& name,
                const std::string& namespace_hex, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto target_ns_vec = *from_hex(namespace_hex);
    if (target_ns_vec.size() != 32) {
        std::fprintf(stderr, "Error: namespace must be 64 hex chars\n");
        return 1;
    }
    auto target_ns = std::span<const uint8_t, 32>(target_ns_vec.data(), 32);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    auto blob = find_pubkey_blob(conn, target_ns);
    conn.close();

    if (!blob) {
        std::fprintf(stderr, "Error: no published pubkey found in namespace %s\n",
                     namespace_hex.c_str());
        return 1;
    }

    std::vector<uint8_t> signing_pk(blob->data.begin() + 4, blob->data.begin() + 4 + 2592);
    std::vector<uint8_t> kem_pk(blob->data.begin() + 4 + 2592, blob->data.end());

    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    db.add(name, signing_pk, kem_pk);

    if (!opts.quiet) {
        std::fprintf(stderr, "added contact: %s (%s)\n", name.c_str(), namespace_hex.c_str());
    }
    return 0;
}

// =============================================================================
// contact rm
// =============================================================================

int contact_rm(const std::string& identity_dir, const std::string& name) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    if (db.remove(name)) {
        std::fprintf(stderr, "removed: %s\n", name.c_str());
        return 0;
    }
    std::fprintf(stderr, "Error: contact not found: %s\n", name.c_str());
    return 1;
}

// =============================================================================
// contact list
// =============================================================================

int contact_list(const std::string& identity_dir) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    auto contacts = db.list();
    if (contacts.empty()) {
        std::fprintf(stderr, "no contacts\n");
        return 0;
    }
    for (const auto& c : contacts) {
        std::printf("%-20s %s\n", c.name.c_str(), c.namespace_hex.c_str());
    }
    return 0;
}

// =============================================================================
// group create
// =============================================================================

int group_create(const std::string& identity_dir, const std::string& name) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    db.group_create(name);
    std::fprintf(stderr, "created group: %s\n", name.c_str());
    return 0;
}

// =============================================================================
// group add <group> <contact>...
// =============================================================================

int group_add(const std::string& identity_dir, const std::string& group,
              const std::vector<std::string>& contacts) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    for (const auto& c : contacts) {
        db.group_add_member(group, c);
        std::fprintf(stderr, "added %s to group %s\n", c.c_str(), group.c_str());
    }
    return 0;
}

// =============================================================================
// group rm <group> -- or -- group rm <group> <contact>
// =============================================================================

int group_rm(const std::string& identity_dir, const std::string& group) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    if (db.group_remove(group)) {
        std::fprintf(stderr, "removed group: %s\n", group.c_str());
        return 0;
    }
    std::fprintf(stderr, "Error: group not found: %s\n", group.c_str());
    return 1;
}

int group_rm_member(const std::string& identity_dir, const std::string& group,
                    const std::string& contact) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    if (db.group_remove_member(group, contact)) {
        std::fprintf(stderr, "removed %s from group %s\n", contact.c_str(), group.c_str());
        return 0;
    }
    std::fprintf(stderr, "Error: %s not in group %s\n", contact.c_str(), group.c_str());
    return 1;
}

// =============================================================================
// group list [<name>]
// =============================================================================

int group_list(const std::string& identity_dir) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    auto groups = db.group_list();
    if (groups.empty()) {
        std::fprintf(stderr, "no groups\n");
        return 0;
    }
    for (const auto& [name, count] : groups) {
        std::printf("%-20s %d member%s\n", name.c_str(), count, count == 1 ? "" : "s");
    }
    return 0;
}

int group_list_members(const std::string& identity_dir, const std::string& group) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    auto members = db.group_members(group);
    if (members.empty()) {
        std::fprintf(stderr, "no members in group: %s\n", group.c_str());
        return 0;
    }
    for (const auto& c : members) {
        std::printf("%-20s %s\n", c.name.c_str(), c.namespace_hex.c_str());
    }
    return 0;
}

// =============================================================================
// contact import <file.json> [host[:port]]
// =============================================================================

int contact_import(const std::string& identity_dir, const std::string& json_path,
                   const ConnectOpts& opts) {
    std::ifstream f(json_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "Error: cannot open %s\n", json_path.c_str());
        return 1;
    }

    auto json = nlohmann::json::parse(f, nullptr, false);
    if (json.is_discarded() || !json.is_array()) {
        std::fprintf(stderr, "Error: expected JSON array in %s\n", json_path.c_str());
        return 1;
    }

    int imported = 0, failed = 0;
    auto total = json.size();

    for (const auto& entry : json) {
        if (!entry.contains("name") || !entry["name"].is_string() ||
            !entry.contains("namespace") || !entry["namespace"].is_string()) {
            std::fprintf(stderr, "  skip: invalid entry (missing name or namespace)\n");
            ++failed;
            continue;
        }

        std::string name = entry["name"].get<std::string>();
        std::string ns = entry["namespace"].get<std::string>();

        if (ns.size() != 64) {
            std::fprintf(stderr, "  skip: %s: namespace must be 64 hex chars\n", name.c_str());
            ++failed;
            continue;
        }

        int rc = contact_add(identity_dir, name, ns, opts);
        if (rc != 0) {
            std::fprintf(stderr, "  skip: %s: pubkey fetch failed\n", name.c_str());
            ++failed;
            continue;
        }
        ++imported;
    }

    std::fprintf(stderr, "Imported %d/%zu contacts. Failed: %d\n",
                 imported, total, failed);
    return (failed > 0 && imported == 0) ? 1 : 0;
}

// =============================================================================
// contact export
// =============================================================================

int contact_export(const std::string& identity_dir) {
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);
    auto contacts = db.list();

    auto json = nlohmann::json::array();
    for (const auto& c : contacts) {
        json.push_back({
            {"name", c.name},
            {"namespace", c.namespace_hex}
        });
    }

    std::printf("%s\n", json.dump(2).c_str());
    return 0;
}

} // namespace chromatindb::cli::cmd
