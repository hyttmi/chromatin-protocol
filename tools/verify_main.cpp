/// chromatindb_verify -- standalone crypto verification CLI for integration tests.
///
/// Decodes FlatBuffer-encoded blobs and independently verifies cryptographic
/// properties using the same code paths as the node itself (chromatindb_lib).
///
/// Subcommands:
///   hash  -- Recompute SHA3-256 signing digest and blob hash from raw fields.
///   sig   -- Verify ML-DSA-87 signature against recomputed digest + embedded pubkey.
///
/// Input: FlatBuffer-encoded blob bytes from stdin or --file path.
/// Output: JSON to stdout. Exit 0 on success/valid, 1 on failure/invalid.

#include "db/crypto/signing.h"
#include "db/wire/codec.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// =============================================================================
// Hex encoding
// =============================================================================

std::string to_hex(const uint8_t* data, size_t len) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result.push_back(hex_chars[data[i] >> 4]);
        result.push_back(hex_chars[data[i] & 0x0F]);
    }
    return result;
}

template <size_t N>
std::string to_hex(const std::array<uint8_t, N>& arr) {
    return to_hex(arr.data(), arr.size());
}

// =============================================================================
// Read blob bytes from stdin or file
// =============================================================================

std::vector<uint8_t> read_blob_bytes(const std::string& file_path) {
    if (file_path.empty()) {
        // Read from stdin (binary mode)
        std::cin >> std::noskipws;
        return {std::istream_iterator<uint8_t>(std::cin),
                std::istream_iterator<uint8_t>()};
    }

    if (!std::filesystem::exists(file_path)) {
        spdlog::error("file not found: {}", file_path);
        return {};
    }

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        spdlog::error("cannot open file: {}", file_path);
        return {};
    }

    return {std::istreambuf_iterator<char>(ifs),
            std::istreambuf_iterator<char>()};
}

// =============================================================================
// Subcommand: hash
// =============================================================================

int cmd_hash(const std::string& file_path) {
    auto blob_bytes = read_blob_bytes(file_path);
    if (blob_bytes.empty()) {
        spdlog::error("no input data");
        return 1;
    }

    // Decode FlatBuffer
    chromatindb::wire::BlobData blob;
    try {
        blob = chromatindb::wire::decode_blob(blob_bytes);
    } catch (const std::exception& e) {
        spdlog::error("decode failed: {}", e.what());
        return 1;
    }

    // Recompute signing digest: SHA3-256(namespace || data || ttl || timestamp)
    auto signing_digest = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);

    // Compute full blob hash: SHA3-256(encoded_blob)
    auto full_hash = chromatindb::wire::blob_hash(blob_bytes);

    // Output JSON
    nlohmann::json out;
    out["signing_digest"] = to_hex(signing_digest);
    out["blob_hash"] = to_hex(full_hash);
    out["namespace_id"] = to_hex(blob.namespace_id);
    out["pubkey_size"] = blob.pubkey.size();
    out["data_size"] = blob.data.size();
    out["ttl"] = blob.ttl;
    out["timestamp"] = blob.timestamp;

    std::cout << out.dump() << std::endl;
    return 0;
}

// =============================================================================
// Subcommand: sig
// =============================================================================

int cmd_sig(const std::string& file_path) {
    auto blob_bytes = read_blob_bytes(file_path);
    if (blob_bytes.empty()) {
        spdlog::error("no input data");
        return 1;
    }

    // Decode FlatBuffer
    chromatindb::wire::BlobData blob;
    try {
        blob = chromatindb::wire::decode_blob(blob_bytes);
    } catch (const std::exception& e) {
        spdlog::error("decode failed: {}", e.what());
        return 1;
    }

    // Recompute signing digest
    auto signing_digest = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);

    // Verify ML-DSA-87 signature against digest + embedded pubkey
    bool valid = chromatindb::crypto::Signer::verify(
        signing_digest, blob.signature, blob.pubkey);

    nlohmann::json out;
    out["valid"] = valid;

    std::cout << out.dump() << std::endl;
    return valid ? 0 : 1;
}

// =============================================================================
// Usage
// =============================================================================

void print_usage(const char* prog) {
    std::cerr << "chromatindb_verify -- crypto verification tool for integration tests\n\n"
              << "Usage:\n"
              << "  " << prog << " hash [--file PATH]   Recompute signing digest and blob hash\n"
              << "  " << prog << " sig  [--file PATH]   Verify ML-DSA-87 signature\n\n"
              << "Input: FlatBuffer-encoded blob bytes from stdin (default) or --file.\n"
              << "Output: JSON to stdout.\n";
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    // Configure spdlog to stderr (keep stdout clean for JSON output)
    auto logger = spdlog::stderr_color_mt("verify");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::warn);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string subcommand = argv[1];

    if (subcommand == "--help" || subcommand == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    // Parse --file option
    std::string file_path;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else {
            spdlog::error("unknown option: {}", arg);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (subcommand == "hash") {
        return cmd_hash(file_path);
    } else if (subcommand == "sig") {
        return cmd_sig(file_path);
    } else {
        spdlog::error("unknown subcommand: {}", subcommand);
        print_usage(argv[0]);
        return 1;
    }
}
