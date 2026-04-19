#include "cli/src/commands.h"
#include "cli/src/connection.h"
#include "cli/src/contacts.h"
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

// ListResponse entry stride: hash:32 + seq:8BE + type:4 = 44 bytes (since Phase 117).
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

int put(const std::string& identity_dir, const std::vector<std::string>& file_paths,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin, const ConnectOpts& opts) {

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

    int errors = 0;

    // Build list of files to upload
    struct FileEntry { std::string path; std::string name; std::vector<uint8_t> data; };
    std::vector<FileEntry> files;

    // Client-side guard: read_file_bytes + envelope + FlatBuffer = ~3x file size in memory.
    // Node enforces MAX_BLOB_DATA_SIZE (default 500 MiB) on its side.
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
            auto signing_input = build_signing_input(ns, envelope_data, ttl, timestamp);
            auto signature = id.sign(signing_input);

            BlobData blob{};
            std::memcpy(blob.namespace_id.data(), ns.data(), 32);
            blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
            blob.data = std::move(envelope_data);
            blob.ttl = ttl;
            blob.timestamp = timestamp;
            blob.signature = std::move(signature);

            auto flatbuf = encode_blob(blob);

            uint32_t this_rid = rid++;
            if (!conn.send_async(MsgType::Data, flatbuf, this_rid)) {
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
        // not request order). recv() returns whichever WriteAck landed first.
        auto resp = conn.recv();
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

        // Phase B: drain one reply in arrival order.
        auto resp = conn.recv();
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

    // Pre-check target existence unless --force. Avoids creating redundant
    // tombstones for already-gone / never-existed blobs (backlog 999.2).
    if (!force) {
        std::vector<uint8_t> exists_payload(64);
        std::memcpy(exists_payload.data(), ns.data(), 32);
        std::memcpy(exists_payload.data() + 32, target_hash.data(), 32);

        if (!conn.send(MsgType::ExistsRequest, exists_payload, 1)) {
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

    // Build and send tombstone
    auto tombstone_data = make_tombstone_data(
        std::span<const uint8_t, 32>(target_hash.data(), 32));

    auto ns_span = std::span<const uint8_t, 32>(ns.data(), 32);
    auto timestamp = static_cast<uint64_t>(std::time(nullptr));
    auto signing_input = build_signing_input(ns_span, tombstone_data, 0, timestamp);
    auto signature = id.sign(signing_input);

    BlobData blob{};
    blob.namespace_id = ns;
    blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
    blob.data = std::move(tombstone_data);
    blob.ttl = 0;
    blob.timestamp = timestamp;
    blob.signature = std::move(signature);

    auto flatbuf = encode_blob(blob);

    if (!conn.send(MsgType::Delete, flatbuf, 2)) {
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
        std::fprintf(stderr, "Error: node rejected request\n");
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

    // Decrypt
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
    auto signing_input = build_signing_input(ns_span, new_envelope, ttl, timestamp);
    auto signature = id.sign(signing_input);

    BlobData new_blob{};
    new_blob.namespace_id = ns;
    new_blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
    new_blob.data = std::move(new_envelope);
    new_blob.ttl = ttl;
    new_blob.timestamp = timestamp;
    new_blob.signature = std::move(signature);

    auto flatbuf = encode_blob(new_blob);

    Connection conn2(id);
    if (!conn2.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect (put)\n");
        return 1;
    }

    if (!conn2.send(MsgType::Data, flatbuf, 1)) {
        std::fprintf(stderr, "Error: failed to send Data\n");
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
    auto del_signing_input = build_signing_input(ns_span, tombstone_data, 0, del_timestamp);
    auto del_signature = id.sign(del_signing_input);

    BlobData del_blob{};
    del_blob.namespace_id = ns;
    del_blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
    del_blob.data = std::move(tombstone_data);
    del_blob.ttl = 0;
    del_blob.timestamp = del_timestamp;
    del_blob.signature = std::move(del_signature);

    auto del_flatbuf = encode_blob(del_blob);

    Connection conn3(id);
    if (!conn3.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Warning: failed to connect for delete (new blob stored)\n");
        std::printf("%s\n", new_hash_hex.c_str());
        return 0;
    }

    if (!conn3.send(MsgType::Delete, del_flatbuf, 1)) {
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
            else {
                std::fprintf(stderr, "Error: unknown type '%s'. Known: CENV, PUBK, TOMB, DLGT, CDAT\n",
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
            std::fprintf(stderr, "Error: node rejected request\n");
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
        std::fprintf(stderr, "Error: node rejected request\n");
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
        std::fprintf(stderr, "Error: node rejected request\n");
        return 1;
    }

    if (resp->type != static_cast<uint8_t>(MsgType::NodeInfoResponse)) {
        std::fprintf(stderr, "Error: unexpected response type %u\n", resp->type);
        return 1;
    }

    // Parse NodeInfoResponse:
    // [version_len:1][version_str][git_hash_len:1][git_hash_str]
    // [uptime:8BE][peer_count:4BE][namespace_count:4BE][total_blobs:8BE]
    // [storage_used:8BE][storage_max:8BE]
    // [types_count:1][supported_types: types_count bytes]
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
        std::fprintf(stderr, "Error: node rejected request\n");
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
    uint32_t rid = 1;
    int errors = 0;

    for (const auto& t : targets) {
        auto delegation_data = make_delegation_data(t.signing_pk);

        auto timestamp = static_cast<uint64_t>(std::time(nullptr));
        auto signing_input = build_signing_input(ns, delegation_data, 0, timestamp);
        auto signature = id.sign(signing_input);

        BlobData blob{};
        std::memcpy(blob.namespace_id.data(), ns.data(), 32);
        blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
        blob.data = std::move(delegation_data);
        blob.ttl = 0;  // Permanent
        blob.timestamp = timestamp;
        blob.signature = std::move(signature);

        auto flatbuf = encode_blob(blob);

        if (!conn.send(MsgType::Data, flatbuf, rid++)) {
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
        auto signing_input = build_signing_input(ns, tombstone_data, 0, timestamp);
        auto signature = id.sign(signing_input);

        BlobData blob{};
        std::memcpy(blob.namespace_id.data(), ns.data(), 32);
        blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
        blob.data = std::move(tombstone_data);
        blob.ttl = 0;
        blob.timestamp = timestamp;
        blob.signature = std::move(signature);

        auto flatbuf = encode_blob(blob);

        if (!conn.send(MsgType::Delete, flatbuf, rid++)) {
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

    // Sign as permanent blob in our namespace
    auto timestamp = static_cast<uint64_t>(std::time(nullptr));
    auto signing_input = build_signing_input(ns, pubkey_data, 0, timestamp);
    auto signature = id.sign(signing_input);

    BlobData blob{};
    std::memcpy(blob.namespace_id.data(), ns.data(), 32);
    blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
    blob.data = std::move(pubkey_data);
    blob.ttl = 0;
    blob.timestamp = timestamp;
    blob.signature = std::move(signature);

    auto flatbuf = encode_blob(blob);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::Data, flatbuf, 1)) {
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
