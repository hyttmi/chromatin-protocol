/// chromatindb_verify -- standalone crypto verification CLI for integration tests.
///
/// Decodes FlatBuffer-encoded blobs and independently verifies cryptographic
/// properties using the same code paths as the node itself (chromatindb_lib).
///
/// Subcommands:
///   hash        -- Recompute SHA3-256 signing digest and blob hash from FlatBuffer blob.
///   sig         -- Verify ML-DSA-87 signature against recomputed digest + embedded pubkey.
///   hash-fields -- Recompute signing digest from raw field values (no FlatBuffer needed).
///   sig-fields  -- Verify ML-DSA-87 signature from raw hex values (no FlatBuffer needed).
///
/// Input: FlatBuffer-encoded blob bytes from stdin or --file path (hash, sig).
///        Raw hex values via CLI flags (hash-fields, sig-fields).
/// Output: JSON to stdout. Exit 0 on success/valid, 1 on failure/invalid.

#include "db/crypto/signing.h"
#include "db/util/hex.h"
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

using chromatindb::util::to_hex;

/// Decode a hex string to a byte vector. Returns empty on invalid input
/// (non-throwing wrapper around chromatindb::util::from_hex for CLI error handling).
std::vector<uint8_t> from_hex_safe(const std::string& hex_str) {
    try {
        return chromatindb::util::from_hex(hex_str);
    } catch (const std::invalid_argument&) {
        return {};
    }
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
// Subcommand: hash (FlatBuffer input)
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
// Subcommand: sig (FlatBuffer input)
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
// Subcommand: hash-fields (raw field values -- no FlatBuffer needed)
// =============================================================================

struct HashFieldsArgs {
    std::string namespace_hex;
    std::string data_hex;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
};

int cmd_hash_fields(const HashFieldsArgs& args) {
    auto ns_bytes = from_hex_safe(args.namespace_hex);
    if (ns_bytes.size() != 32) {
        spdlog::error("--namespace-hex must be 64 hex chars (32 bytes), got {}",
                      args.namespace_hex.size());
        return 1;
    }

    auto data_bytes = from_hex_safe(args.data_hex);
    if (data_bytes.empty() && !args.data_hex.empty()) {
        spdlog::error("--data-hex contains invalid hex");
        return 1;
    }

    // Convert namespace to array
    std::array<uint8_t, 32> ns_array{};
    std::memcpy(ns_array.data(), ns_bytes.data(), 32);

    // Recompute signing digest: SHA3-256(namespace || data || ttl_be32 || timestamp_be64)
    auto signing_digest = chromatindb::wire::build_signing_input(
        ns_array, data_bytes, args.ttl, args.timestamp);

    nlohmann::json out;
    out["signing_digest"] = to_hex(signing_digest);

    std::cout << out.dump() << std::endl;
    return 0;
}

// =============================================================================
// Subcommand: sig-fields (raw hex values -- no FlatBuffer needed)
// =============================================================================

struct SigFieldsArgs {
    std::string digest_hex;
    std::string signature_hex;
    std::string pubkey_hex;
};

int cmd_sig_fields(const SigFieldsArgs& args) {
    auto digest_bytes = from_hex_safe(args.digest_hex);
    if (digest_bytes.size() != 32) {
        spdlog::error("--digest-hex must be 64 hex chars (32 bytes), got {}",
                      args.digest_hex.size());
        return 1;
    }

    auto sig_bytes = from_hex_safe(args.signature_hex);
    if (sig_bytes.empty()) {
        spdlog::error("--signature-hex is empty or contains invalid hex");
        return 1;
    }

    auto pk_bytes = from_hex_safe(args.pubkey_hex);
    if (pk_bytes.empty()) {
        spdlog::error("--pubkey-hex is empty or contains invalid hex");
        return 1;
    }

    // Verify ML-DSA-87 signature against digest + pubkey
    bool valid = chromatindb::crypto::Signer::verify(
        digest_bytes, sig_bytes, pk_bytes);

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
              << "  " << prog << " sig  [--file PATH]   Verify ML-DSA-87 signature\n"
              << "  " << prog << " hash-fields --namespace-hex HEX --data-hex HEX --ttl N --timestamp N\n"
              << "                            Compute signing digest from raw field values\n"
              << "  " << prog << " sig-fields --digest-hex HEX --signature-hex HEX --pubkey-hex HEX\n"
              << "                            Verify ML-DSA-87 signature from raw hex values\n\n"
              << "Input: FlatBuffer blob from stdin/--file (hash, sig) or hex flags (hash-fields, sig-fields).\n"
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

    // --- hash / sig: parse --file option ---
    if (subcommand == "hash" || subcommand == "sig") {
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
        } else {
            return cmd_sig(file_path);
        }
    }

    // --- hash-fields: parse raw field options ---
    if (subcommand == "hash-fields") {
        HashFieldsArgs hf_args;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--namespace-hex" && i + 1 < argc) {
                hf_args.namespace_hex = argv[++i];
            } else if (arg == "--data-hex" && i + 1 < argc) {
                hf_args.data_hex = argv[++i];
            } else if (arg == "--ttl" && i + 1 < argc) {
                hf_args.ttl = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--timestamp" && i + 1 < argc) {
                hf_args.timestamp = std::stoull(argv[++i]);
            } else {
                spdlog::error("unknown option for hash-fields: {}", arg);
                print_usage(argv[0]);
                return 1;
            }
        }

        if (hf_args.namespace_hex.empty()) {
            spdlog::error("hash-fields requires --namespace-hex");
            return 1;
        }

        return cmd_hash_fields(hf_args);
    }

    // --- sig-fields: parse raw hex options ---
    if (subcommand == "sig-fields") {
        SigFieldsArgs sf_args;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--digest-hex" && i + 1 < argc) {
                sf_args.digest_hex = argv[++i];
            } else if (arg == "--signature-hex" && i + 1 < argc) {
                sf_args.signature_hex = argv[++i];
            } else if (arg == "--pubkey-hex" && i + 1 < argc) {
                sf_args.pubkey_hex = argv[++i];
            } else {
                spdlog::error("unknown option for sig-fields: {}", arg);
                print_usage(argv[0]);
                return 1;
            }
        }

        if (sf_args.digest_hex.empty() || sf_args.signature_hex.empty() ||
            sf_args.pubkey_hex.empty()) {
            spdlog::error("sig-fields requires --digest-hex, --signature-hex, and --pubkey-hex");
            return 1;
        }

        return cmd_sig_fields(sf_args);
    }

    spdlog::error("unknown subcommand: {}", subcommand);
    print_usage(argv[0]);
    return 1;
}
