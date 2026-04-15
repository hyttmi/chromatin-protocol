#include "cli/src/commands.h"
#include "cli/src/connection.h"
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
    const Identity& id, const std::string& namespace_hex) {

    if (namespace_hex.empty()) {
        std::array<uint8_t, 32> ns{};
        auto span = id.namespace_id();
        std::memcpy(ns.data(), span.data(), 32);
        return ns;
    }

    auto bytes = from_hex(namespace_hex);
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
    const std::vector<std::string>& pubkey_files) {

    std::vector<std::vector<uint8_t>> kem_pks;
    kem_pks.reserve(pubkey_files.size());

    for (auto& path : pubkey_files) {
        auto data = read_file_bytes(path);
        auto [signing_pk, kem_pk] = Identity::load_public_keys(data);
        kem_pks.push_back(std::move(kem_pk));
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
    auto external_pks = load_recipient_kem_pubkeys(share_pubkey_files);
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

    if (from_stdin) {
        files.push_back({"", "", read_stdin_bytes()});
    } else {
        for (const auto& fp : file_paths) {
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

int get(const std::string& identity_dir, const std::string& hash_hex,
        const std::string& namespace_hex, bool to_stdout, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex);
    auto hash = parse_hash(hash_hex);

    // Build ReadRequest payload: [namespace:32][hash:32] = 64 bytes
    std::vector<uint8_t> payload(64);
    std::memcpy(payload.data(), ns.data(), 32);
    std::memcpy(payload.data() + 32, hash.data(), 32);

    Connection conn(id);
    if (!conn.connect(opts.host, opts.port, opts.uds_path)) {
        std::fprintf(stderr, "Error: failed to connect\n");
        return 1;
    }

    if (!conn.send(MsgType::ReadRequest, payload, 1)) {
        std::fprintf(stderr, "Error: failed to send ReadRequest\n");
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

    if (resp->type != static_cast<uint8_t>(MsgType::ReadResponse)) {
        std::fprintf(stderr, "Error: unexpected response type %u\n", resp->type);
        return 1;
    }

    // Parse ReadResponse: [status:1][flatbuffer_blob...]
    if (resp->payload.empty()) {
        std::fprintf(stderr, "Error: empty ReadResponse\n");
        return 1;
    }

    uint8_t status = resp->payload[0];
    if (status != 0x01) {
        std::fprintf(stderr, "not found\n");
        return 1;
    }

    if (resp->payload.size() < 2) {
        std::fprintf(stderr, "Error: ReadResponse has no blob data\n");
        return 1;
    }

    auto blob_bytes = std::span<const uint8_t>(
        resp->payload.data() + 1, resp->payload.size() - 1);

    auto blob = decode_blob(blob_bytes);
    if (!blob) {
        std::fprintf(stderr, "Error: failed to decode blob\n");
        return 1;
    }

    // Attempt envelope decryption
    std::vector<uint8_t> plaintext;
    if (envelope::is_envelope(blob->data)) {
        auto decrypted = envelope::decrypt(blob->data, id.kem_seckey(), id.kem_pubkey());
        if (!decrypted) {
            std::fprintf(stderr, "Error: cannot decrypt (not a recipient)\n");
            return 1;
        }
        plaintext = std::move(*decrypted);
    } else {
        plaintext = blob->data;
    }

    // Parse payload to extract filename and file data
    auto parsed = parse_put_payload(plaintext);
    std::string out_filename = parsed.name.empty() ? hash_hex : parsed.name;

    if (to_stdout) {
        std::cout.write(reinterpret_cast<const char*>(parsed.file_data.data()),
                        static_cast<std::streamsize>(parsed.file_data.size()));
        std::cout.flush();
    } else {
        std::ofstream f(out_filename, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "Error: cannot write to %s\n", out_filename.c_str());
            return 1;
        }
        f.write(reinterpret_cast<const char*>(parsed.file_data.data()),
                static_cast<std::streamsize>(parsed.file_data.size()));
        if (!opts.quiet) {
            std::fprintf(stderr, "saved: %s\n", out_filename.c_str());
        }
    }

    return 0;
}

// =============================================================================
// rm
// =============================================================================

int rm(const std::string& identity_dir, const std::string& hash_hex,
       const std::string& namespace_hex, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex);
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
    auto ns = resolve_namespace(id, namespace_hex);
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
    auto external_pks = load_recipient_kem_pubkeys(share_pubkey_files);

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
    auto ns = resolve_namespace(id, namespace_hex);

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
// exists
// =============================================================================

int exists(const std::string& identity_dir, const std::string& hash_hex,
           const std::string& namespace_hex, const ConnectOpts& opts) {

    auto id = Identity::load_from(identity_dir);
    auto ns = resolve_namespace(id, namespace_hex);
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

} // namespace chromatindb::cli::cmd
