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
        if (fs::exists(arg)) {
            // It's a file path — load pubkey from file
            auto data = read_file_bytes(arg);
            auto [signing_pk, kem_pk] = Identity::load_public_keys(data);
            kem_pks.push_back(std::move(kem_pk));
        } else {
            // Treat as contact name — look up in contacts db
            auto db_path = identity_dir + "/contacts.db";
            ContactDB db(db_path);
            auto contact = db.get(arg);
            if (!contact) {
                throw std::runtime_error("Unknown contact or file: " + arg);
            }
            kem_pks.push_back(contact->kem_pk);
        }
    }
    return kem_pks;
}

// =============================================================================
// keygen
// =============================================================================

int keygen(const std::string& identity_dir, bool force) {
    fs::path dir(identity_dir);

    if (!force && fs::exists(dir / "identity.key")) {
        std::fprintf(stderr, "Identity already exists at %s\n", dir.c_str());
        std::fprintf(stderr, "Use --force to overwrite.\n");
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

int export_key(const std::string& identity_dir) {
    auto id = Identity::load_from(identity_dir);
    auto keys = id.export_public_keys();
    std::cout.write(reinterpret_cast<const char*>(keys.data()),
                    static_cast<std::streamsize>(keys.size()));
    std::cout.flush();
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

    uint32_t rid = 1;
    int errors = 0;

    // Build list of files to upload
    struct FileEntry { std::string path; std::string name; std::vector<uint8_t> data; };
    std::vector<FileEntry> files;

    // Safety limit to prevent OOM — envelope encryption buffers the full file.
    // The node enforces its own MAX_BLOB_DATA_SIZE (default 500 MiB, configurable).
    static constexpr size_t MAX_FILE_SIZE = 2ULL * 1024 * 1024 * 1024;  // 2 GiB client-side guard

    if (from_stdin) {
        files.push_back({"", "", read_stdin_bytes()});
    } else {
        for (const auto& fp : file_paths) {
            auto fsize = fs::file_size(fp);
            if (fsize > MAX_FILE_SIZE) {
                std::fprintf(stderr, "Error: %s too large (%zu MiB, max ~500 MiB)\n",
                             fp.c_str(), static_cast<size_t>(fsize / (1024 * 1024)));
                return 1;
            }
            auto fname = fs::path(fp).filename().string();
            files.push_back({fp, fname, read_file_bytes(fp)});
        }
    }

    for (auto& f : files) {
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

        if (!conn.send(MsgType::Data, flatbuf, rid++)) {
            std::fprintf(stderr, "Error: failed to send %s\n", f.name.c_str());
            ++errors;
            continue;
        }

        auto resp = conn.recv();
        if (!resp || resp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
            resp->payload.size() < 41) {
            std::fprintf(stderr, "Error: bad response for %s\n", f.name.c_str());
            ++errors;
            continue;
        }

        auto hash_span = std::span<const uint8_t>(resp->payload.data(), 32);
        auto hash_hex = to_hex(hash_span);
        if (!opts.quiet && !f.name.empty() && files.size() > 1) {
            std::printf("%s  %s\n", hash_hex.c_str(), f.name.c_str());
        } else {
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

    uint32_t rid = 1;
    int errors = 0;

    for (const auto& hash_hex : hash_hexes) {
        auto hash = parse_hash(hash_hex);

        std::vector<uint8_t> payload(64);
        std::memcpy(payload.data(), ns.data(), 32);
        std::memcpy(payload.data() + 32, hash.data(), 32);

        if (!conn.send(MsgType::ReadRequest, payload, rid++)) {
            std::fprintf(stderr, "Error: failed to send ReadRequest for %s\n", hash_hex.c_str());
            ++errors;
            continue;
        }

        auto resp = conn.recv();
        if (!resp || resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
            resp->payload.empty()) {
            std::fprintf(stderr, "Error: bad response for %s\n", hash_hex.c_str());
            ++errors;
            continue;
        }

        if (resp->payload[0] != 0x01) {
            std::fprintf(stderr, "%s: not found\n", hash_hex.c_str());
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
                std::fprintf(stderr, "skip: %s already exists (use --force to overwrite)\n",
                             out_path.c_str());
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
       const std::string& namespace_hex, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);
    auto target_hash = parse_hash(hash_hex);

    // Build tombstone
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

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::Delete, flatbuf, 1)) {
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

    // DeleteAck: [hash:32][seq_num:8BE][status:1] = 41 bytes
    if (resp->payload.size() < 41) {
        std::fprintf(stderr, "Error: invalid DeleteAck payload (%zu bytes)\n",
                      resp->payload.size());
        return 1;
    }

    if (!opts.quiet) {
        auto del_hash = std::span<const uint8_t>(resp->payload.data(), 32);
        std::fprintf(stderr, "deleted: %s\n", to_hex(del_hash).c_str());
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
       const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex, identity_dir);

    uint64_t since_seq = 0;

    for (;;) {
        // Build ListRequest: [namespace:32][since_seq:8BE][limit:4BE] = 44 bytes
        std::vector<uint8_t> payload(44);
        std::memcpy(payload.data(), ns.data(), 32);
        store_u64_be(payload.data() + 32, since_seq);
        store_u32_be(payload.data() + 40, 100);

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

        // Parse ListResponse: [count:4BE][entries: N * (hash:32 + seq_num:8BE)][has_more:1]
        auto& p = resp->payload;
        if (p.size() < 5) {  // at least count(4) + has_more(1)
            std::fprintf(stderr, "Error: ListResponse too short\n");
            return 1;
        }

        uint32_t count = load_u32_be(p.data());
        size_t entries_size = static_cast<size_t>(count) * 40;  // 32 + 8 per entry
        if (p.size() < 4 + entries_size + 1) {
            std::fprintf(stderr, "Error: ListResponse truncated\n");
            return 1;
        }

        const uint8_t* entries = p.data() + 4;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* entry = entries + static_cast<size_t>(i) * 40;
            auto hash_span = std::span<const uint8_t>(entry, 32);
            std::printf("%s\n", to_hex(hash_span).c_str());

            // Track last seq_num for pagination
            since_seq = load_u64_be(entry + 32);
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
        size_t entries_size = static_cast<size_t>(count) * 40;
        if (p.size() < 4 + entries_size + 1) return hashes;

        const uint8_t* entries = p.data() + 4;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* entry = entries + static_cast<size_t>(i) * 40;
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
    uint8_t git_len = read_u8();
    auto git_hash = read_string(git_len);

    auto uptime = read_u64();
    auto peer_count = read_u32();
    auto namespace_count = read_u32();
    auto total_blobs = read_u64();
    auto storage_used = read_u64();
    auto storage_max = read_u64();

    std::printf("Version: %s\n", version.c_str());
    std::printf("Git: %s\n", git_hash.c_str());
    std::printf("Uptime: %lus\n", static_cast<unsigned long>(uptime));
    std::printf("Peers: %u\n", peer_count);
    std::printf("Namespaces: %u\n", namespace_count);
    std::printf("Blobs: %lu\n", static_cast<unsigned long>(total_blobs));
    std::printf("Storage: %lu / %lu bytes\n",
                static_cast<unsigned long>(storage_used),
                static_cast<unsigned long>(storage_max));

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

    std::printf("Blobs: %lu\n", static_cast<unsigned long>(count));
    std::printf("Size: %lu bytes\n", static_cast<unsigned long>(bytes));
    std::printf("Quota: %lu bytes\n", static_cast<unsigned long>(quota));

    return 0;
}

// =============================================================================
// delegate — grant write access to another identity
// =============================================================================

int delegate(const std::string& identity_dir, const std::string& pubkey_file,
             const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);

    // Load delegate's exported public keys and extract signing pubkey
    auto exported = read_file_bytes(pubkey_file);
    auto [delegate_signing_pk, delegate_kem_pk] = Identity::load_public_keys(exported);

    // Build delegation data: [0xDE1E6A7E][delegate_signing_pubkey:2592]
    auto delegation_data = make_delegation_data(delegate_signing_pk);

    // Sign as a regular blob in our namespace
    auto ns = id.namespace_id();
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

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::Data, flatbuf, 1)) {
        std::fprintf(stderr, "Error: failed to send delegation\n");
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

    auto hash_span = std::span<const uint8_t>(resp->payload.data(), 32);
    auto delegate_ns = sha3_256(delegate_signing_pk);

    if (!opts.quiet) {
        std::fprintf(stderr, "delegated write access to %s\n",
                     to_hex(std::span<const uint8_t>(delegate_ns.data(), 32)).c_str());
    }
    std::printf("%s\n", to_hex(hash_span).c_str());

    return 0;
}

// =============================================================================
// revoke — revoke write access by tombstoning delegation blob
// =============================================================================

int revoke(const std::string& identity_dir, const std::string& pubkey_file,
           const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);

    // Load delegate's signing pubkey to find their delegation
    auto exported = read_file_bytes(pubkey_file);
    auto [delegate_signing_pk, delegate_kem_pk] = Identity::load_public_keys(exported);
    auto delegate_pk_hash = sha3_256(delegate_signing_pk);

    auto ns = id.namespace_id();

    // Query DelegationList to find the delegation blob hash
    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    std::vector<uint8_t> list_payload(ns.data(), ns.data() + 32);
    if (!conn.send(MsgType::DelegationListRequest, list_payload, 1)) {
        std::fprintf(stderr, "Error: failed to send DelegationListRequest\n");
        conn.close();
        return 1;
    }

    auto list_resp = conn.recv();
    if (!list_resp || list_resp->type != static_cast<uint8_t>(MsgType::DelegationListResponse) ||
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

    // Find matching delegation by delegate_pk_hash
    std::array<uint8_t, 32> delegation_blob_hash{};
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 4 + i * 64;
        auto pk_hash = std::span<const uint8_t>(list_resp->payload.data() + off, 32);
        if (std::equal(pk_hash.begin(), pk_hash.end(), delegate_pk_hash.data())) {
            std::memcpy(delegation_blob_hash.data(),
                        list_resp->payload.data() + off + 32, 32);
            found = true;
            break;
        }
    }

    if (!found) {
        std::fprintf(stderr, "Error: no delegation found for that key\n");
        conn.close();
        return 1;
    }

    // Tombstone the delegation blob
    auto tombstone_data = make_tombstone_data(
        std::span<const uint8_t, 32>(delegation_blob_hash.data(), 32));

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

    if (!conn.send(MsgType::Delete, flatbuf, 2)) {
        std::fprintf(stderr, "Error: failed to send Delete\n");
        conn.close();
        return 1;
    }

    auto del_resp = conn.recv();
    conn.close();

    if (!del_resp || del_resp->type != static_cast<uint8_t>(MsgType::DeleteAck)) {
        std::fprintf(stderr, "Error: bad DeleteAck\n");
        return 1;
    }

    if (!opts.quiet) {
        std::fprintf(stderr, "revoked delegation for %s\n",
                     to_hex(std::span<const uint8_t>(delegate_pk_hash.data(), 32)).c_str());
    }

    return 0;
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

    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 4 + i * 64;
        auto pk_hash = std::span<const uint8_t>(resp->payload.data() + off, 32);
        auto blob_hash = std::span<const uint8_t>(resp->payload.data() + off + 32, 32);
        std::printf("%s  %s\n", to_hex(pk_hash).c_str(), to_hex(blob_hash).c_str());
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

    // Check if already published: list own namespace, look for PUBK blob
    {
        Connection check_conn(id);
        if (check_conn.connect(opts.host, opts.port, opts.uds_path)) {
            // ListRequest: [namespace:32][since_seq:8BE=0][limit:4BE=100]
            std::vector<uint8_t> list_payload(44, 0);
            std::memcpy(list_payload.data(), ns.data(), 32);
            store_u32_be(list_payload.data() + 40, 100);

            if (check_conn.send(MsgType::ListRequest, list_payload, 1)) {
                auto list_resp = check_conn.recv();
                if (list_resp && list_resp->type == static_cast<uint8_t>(MsgType::ListResponse) &&
                    list_resp->payload.size() >= 5) {

                    uint32_t count = load_u32_be(list_resp->payload.data());
                    uint32_t rid = 2;
                    for (uint32_t i = 0; i < count; ++i) {
                        size_t off = 4 + i * 40;
                        auto hash = std::span<const uint8_t>(list_resp->payload.data() + off, 32);

                        std::vector<uint8_t> read_payload(64);
                        std::memcpy(read_payload.data(), ns.data(), 32);
                        std::memcpy(read_payload.data() + 32, hash.data(), 32);

                        if (!check_conn.send(MsgType::ReadRequest, read_payload, rid++))
                            continue;

                        auto read_resp = check_conn.recv();
                        if (!read_resp || read_resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
                            read_resp->payload.empty() || read_resp->payload[0] != 0x01)
                            continue;

                        auto blob_bytes = std::span<const uint8_t>(
                            read_resp->payload.data() + 1, read_resp->payload.size() - 1);
                        auto blob = decode_blob(blob_bytes);
                        if (!blob) continue;

                        if (is_pubkey_blob(blob->data)) {
                            check_conn.close();
                            if (!opts.quiet) {
                                std::fprintf(stderr, "already published: %s\n", ns_hex.c_str());
                            }
                            std::printf("%s\n", ns_hex.c_str());
                            return 0;
                        }
                    }
                }
            }
            check_conn.close();
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
        std::fprintf(stderr, "published: %s\n", ns_hex.c_str());
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
    auto target_ns = *from_hex(namespace_hex);
    if (target_ns.size() != 32) {
        std::fprintf(stderr, "Error: namespace must be 64 hex chars\n");
        return 1;
    }

    // Fetch blob list from target namespace to find PUBK blob
    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    // ListRequest: [namespace:32][since_seq:8BE=0][limit:4BE=100]
    std::vector<uint8_t> list_payload(44, 0);
    std::memcpy(list_payload.data(), target_ns.data(), 32);
    store_u32_be(list_payload.data() + 40, 100);

    if (!conn.send(MsgType::ListRequest, list_payload, 1)) {
        std::fprintf(stderr, "Error: failed to list namespace\n");
        conn.close();
        return 1;
    }

    auto list_resp = conn.recv();
    if (!list_resp || list_resp->type != static_cast<uint8_t>(MsgType::ListResponse) ||
        list_resp->payload.size() < 5) {
        std::fprintf(stderr, "Error: bad ListResponse\n");
        conn.close();
        return 1;
    }

    uint32_t count = load_u32_be(list_resp->payload.data());

    // Try each hash — fetch blob, check for PUBK magic
    uint32_t rid = 2;
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 4 + i * 40;
        auto hash = std::span<const uint8_t>(list_resp->payload.data() + off, 32);

        // ReadRequest: [namespace:32][hash:32]
        std::vector<uint8_t> read_payload(64);
        std::memcpy(read_payload.data(), target_ns.data(), 32);
        std::memcpy(read_payload.data() + 32, hash.data(), 32);

        if (!conn.send(MsgType::ReadRequest, read_payload, rid++)) continue;

        auto read_resp = conn.recv();
        if (!read_resp || read_resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
            read_resp->payload.empty() || read_resp->payload[0] != 0x01) {
            continue;
        }

        auto blob_bytes = std::span<const uint8_t>(
            read_resp->payload.data() + 1, read_resp->payload.size() - 1);
        auto blob = decode_blob(blob_bytes);
        if (!blob) continue;

        if (is_pubkey_blob(blob->data)) {
            // Found it! Extract signing_pk and kem_pk
            std::vector<uint8_t> signing_pk(blob->data.begin() + 4, blob->data.begin() + 4 + 2592);
            std::vector<uint8_t> kem_pk(blob->data.begin() + 4 + 2592, blob->data.end());

            conn.close();

            // Save to contacts db
            auto db_path = identity_dir + "/contacts.db";
            ContactDB db(db_path);
            db.add(name, signing_pk, kem_pk);

            auto ns_hex = to_hex(std::span<const uint8_t>(target_ns.data(), 32));
            if (!opts.quiet) {
                std::fprintf(stderr, "added contact: %s (%s)\n", name.c_str(), ns_hex.c_str());
            }
            return 0;
        }
    }

    conn.close();
    std::fprintf(stderr, "Error: no published pubkey found in namespace %s\n",
                 namespace_hex.c_str());
    return 1;
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

} // namespace chromatindb::cli::cmd
