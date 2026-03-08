// chromatindb_bench -- Standalone benchmark binary for chromatindb.
//
// Measures crypto operations, data path, and network operations.
// Outputs markdown-formatted tables suitable for README.md.
//
// Usage: chromatindb_bench
//
// No arguments. Uses temporary storage that is cleaned up on exit.

#include "db/crypto/aead.h"
#include "db/crypto/hash.h"
#include "db/crypto/kem.h"
#include "db/crypto/signing.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/net/handshake.h"
#include "db/storage/storage.h"
#include "db/sync/sync_protocol.h"
#include "db/wire/codec.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::high_resolution_clock;

// =============================================================================
// Helpers
// =============================================================================

/// RAII temp directory that cleans up on destruction.
struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path() / "chromatindb_bench";
        fs::create_directories(base);
        // Use random suffix for uniqueness
        std::mt19937 rng(static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<uint64_t> dist;
        path = base / std::to_string(dist(rng));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Generate random bytes of given size.
static std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> buf(n);
    std::mt19937 rng(42);  // Deterministic seed for reproducibility
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& b : buf) b = static_cast<uint8_t>(dist(rng));
    return buf;
}

/// Create a signed blob for benchmarking.
static chromatindb::wire::BlobData make_signed_blob(
    const chromatindb::identity::NodeIdentity& id,
    const std::vector<uint8_t>& data,
    uint32_t ttl,
    uint64_t timestamp)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data = data;
    blob.ttl = ttl;
    blob.timestamp = timestamp;

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

/// Benchmark result for a single benchmark.
struct BenchResult {
    std::string name;
    int iterations;
    double total_ms;
    double avg_us;
    double ops_sec;
};

/// Run a benchmark: warmup, then timed iterations.
static BenchResult run_bench(
    const std::string& name,
    int iterations,
    int warmup,
    const std::function<void()>& fn)
{
    // Warmup
    for (int i = 0; i < warmup; ++i) {
        fn();
    }

    // Timed run
    auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        fn();
    }
    auto end = Clock::now();

    double total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    double total_ms = total_ns / 1'000'000.0;
    double avg_us = (total_ns / iterations) / 1'000.0;
    double ops_sec = (iterations / total_ms) * 1'000.0;

    return {name, iterations, total_ms, avg_us, ops_sec};
}

/// Print a markdown table from benchmark results.
static void print_table(const std::vector<BenchResult>& results) {
    std::cout << "| Benchmark | Iterations | Total (ms) | Avg (us) | Ops/sec |\n";
    std::cout << "|-----------|------------|------------|----------|----------|\n";
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << r.iterations
                  << " | " << std::fixed << std::setprecision(1) << r.total_ms
                  << " | " << std::fixed << std::setprecision(1) << r.avg_us
                  << " | " << std::fixed << std::setprecision(0) << r.ops_sec
                  << " |\n";
    }
    std::cout << "\n";
}

// =============================================================================
// Benchmark groups
// =============================================================================

static std::vector<BenchResult> bench_sha3() {
    std::vector<BenchResult> results;

    auto data_64 = random_bytes(64);
    auto data_1k = random_bytes(1024);
    auto data_64k = random_bytes(64 * 1024);
    auto data_1m = random_bytes(1024 * 1024);

    results.push_back(run_bench("SHA3-256 (64 B)", 1000, 2, [&] {
        chromatindb::crypto::sha3_256(data_64);
    }));
    results.push_back(run_bench("SHA3-256 (1 KiB)", 1000, 2, [&] {
        chromatindb::crypto::sha3_256(data_1k);
    }));
    results.push_back(run_bench("SHA3-256 (64 KiB)", 100, 2, [&] {
        chromatindb::crypto::sha3_256(data_64k);
    }));
    results.push_back(run_bench("SHA3-256 (1 MiB)", 100, 2, [&] {
        chromatindb::crypto::sha3_256(data_1m);
    }));

    return results;
}

static std::vector<BenchResult> bench_signing() {
    std::vector<BenchResult> results;

    auto msg = random_bytes(64);

    // Keygen
    results.push_back(run_bench("ML-DSA-87 keygen", 10, 1, [&] {
        chromatindb::crypto::Signer s;
        s.generate_keypair();
    }));

    // Sign (pre-generate keypair)
    chromatindb::crypto::Signer signer;
    signer.generate_keypair();

    results.push_back(run_bench("ML-DSA-87 sign (64 B)", 100, 2, [&] {
        signer.sign(msg);
    }));

    // Verify (pre-generate signature)
    auto sig = signer.sign(msg);
    auto pk = std::vector<uint8_t>(signer.export_public_key().begin(),
                                    signer.export_public_key().end());

    results.push_back(run_bench("ML-DSA-87 verify (64 B)", 100, 2, [&] {
        chromatindb::crypto::Signer::verify(msg, sig, pk);
    }));

    return results;
}

static std::vector<BenchResult> bench_kem() {
    std::vector<BenchResult> results;

    // Keygen
    results.push_back(run_bench("ML-KEM-1024 keygen", 10, 1, [&] {
        chromatindb::crypto::KEM k;
        k.generate_keypair();
    }));

    // Pre-generate keypair for encaps/decaps
    chromatindb::crypto::KEM kem;
    kem.generate_keypair();

    auto pk = std::vector<uint8_t>(kem.export_public_key().begin(),
                                    kem.export_public_key().end());

    results.push_back(run_bench("ML-KEM-1024 encaps", 100, 2, [&] {
        kem.encaps(pk);
    }));

    // Pre-generate ciphertext for decaps
    auto [ct, _ss] = kem.encaps(pk);
    auto sk = std::vector<uint8_t>(kem.export_secret_key().begin(),
                                    kem.export_secret_key().end());

    results.push_back(run_bench("ML-KEM-1024 decaps", 100, 2, [&] {
        kem.decaps(ct, sk);
    }));

    return results;
}

static std::vector<BenchResult> bench_aead() {
    std::vector<BenchResult> results;

    auto key = chromatindb::crypto::AEAD::keygen();
    std::vector<uint8_t> nonce(chromatindb::crypto::AEAD::NONCE_SIZE, 0);
    std::vector<uint8_t> ad = {0x01, 0x02, 0x03, 0x04};

    auto pt_64 = random_bytes(64);
    auto pt_1k = random_bytes(1024);
    auto pt_64k = random_bytes(64 * 1024);
    auto pt_1m = random_bytes(1024 * 1024);

    // Encrypt
    results.push_back(run_bench("ChaCha20-Poly1305 encrypt (64 B)", 1000, 2, [&] {
        chromatindb::crypto::AEAD::encrypt(pt_64, ad, nonce, key.span());
    }));
    results.push_back(run_bench("ChaCha20-Poly1305 encrypt (1 KiB)", 1000, 2, [&] {
        chromatindb::crypto::AEAD::encrypt(pt_1k, ad, nonce, key.span());
    }));
    results.push_back(run_bench("ChaCha20-Poly1305 encrypt (64 KiB)", 100, 2, [&] {
        chromatindb::crypto::AEAD::encrypt(pt_64k, ad, nonce, key.span());
    }));
    results.push_back(run_bench("ChaCha20-Poly1305 encrypt (1 MiB)", 100, 2, [&] {
        chromatindb::crypto::AEAD::encrypt(pt_1m, ad, nonce, key.span());
    }));

    // Decrypt (pre-encrypt ciphertexts)
    auto ct_64 = chromatindb::crypto::AEAD::encrypt(pt_64, ad, nonce, key.span());
    auto ct_1k = chromatindb::crypto::AEAD::encrypt(pt_1k, ad, nonce, key.span());
    auto ct_64k = chromatindb::crypto::AEAD::encrypt(pt_64k, ad, nonce, key.span());
    auto ct_1m = chromatindb::crypto::AEAD::encrypt(pt_1m, ad, nonce, key.span());

    results.push_back(run_bench("ChaCha20-Poly1305 decrypt (64 B)", 1000, 2, [&] {
        chromatindb::crypto::AEAD::decrypt(ct_64, ad, nonce, key.span());
    }));
    results.push_back(run_bench("ChaCha20-Poly1305 decrypt (1 KiB)", 1000, 2, [&] {
        chromatindb::crypto::AEAD::decrypt(ct_1k, ad, nonce, key.span());
    }));
    results.push_back(run_bench("ChaCha20-Poly1305 decrypt (64 KiB)", 100, 2, [&] {
        chromatindb::crypto::AEAD::decrypt(ct_64k, ad, nonce, key.span());
    }));
    results.push_back(run_bench("ChaCha20-Poly1305 decrypt (1 MiB)", 100, 2, [&] {
        chromatindb::crypto::AEAD::decrypt(ct_1m, ad, nonce, key.span());
    }));

    return results;
}

static std::vector<BenchResult> bench_data_path() {
    std::vector<BenchResult> results;

    TempDir tmpdir;

    auto id = chromatindb::identity::NodeIdentity::generate();
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    auto data_1k = random_bytes(1024);
    auto data_64k = random_bytes(64 * 1024);

    // Blob ingest: sign + validate + store
    {
        chromatindb::storage::Storage storage(tmpdir.path.string());
        chromatindb::engine::BlobEngine engine(storage);

        int iters = 100;
        int i = 0;
        results.push_back(run_bench("Blob ingest (1 KiB)", iters, 0, [&] {
            auto blob = make_signed_blob(id, data_1k, 604800, now + (i++));
            engine.ingest(blob);
        }));
    }
    {
        TempDir tmpdir2;
        chromatindb::storage::Storage storage(tmpdir2.path.string());
        chromatindb::engine::BlobEngine engine(storage);

        int iters = 100;
        int i = 0;
        results.push_back(run_bench("Blob ingest (64 KiB)", iters, 0, [&] {
            auto blob = make_signed_blob(id, data_64k, 604800, now + 10000 + (i++));
            engine.ingest(blob);
        }));
    }

    // Blob retrieval: get_blob by namespace + hash
    {
        TempDir tmpdir3;
        chromatindb::storage::Storage storage(tmpdir3.path.string());
        chromatindb::engine::BlobEngine engine(storage);

        // Seed with one blob of each size
        auto blob_1k = make_signed_blob(id, data_1k, 604800, now + 20000);
        auto r1k = engine.ingest(blob_1k);

        auto blob_64k = make_signed_blob(id, data_64k, 604800, now + 20001);
        auto r64k = engine.ingest(blob_64k);

        auto hash_1k = r1k.ack->blob_hash;
        auto hash_64k = r64k.ack->blob_hash;

        results.push_back(run_bench("Blob retrieve (1 KiB)", 1000, 2, [&] {
            engine.get_blob(id.namespace_id(), hash_1k);
        }));

        results.push_back(run_bench("Blob retrieve (64 KiB)", 1000, 2, [&] {
            engine.get_blob(id.namespace_id(), hash_64k);
        }));
    }

    // Blob encode/decode round-trip
    {
        auto blob_1k = make_signed_blob(id, data_1k, 604800, now + 30000);

        results.push_back(run_bench("Blob encode (1 KiB)", 1000, 2, [&] {
            chromatindb::wire::encode_blob(blob_1k);
        }));

        auto encoded = chromatindb::wire::encode_blob(blob_1k);
        results.push_back(run_bench("Blob decode (1 KiB)", 1000, 2, [&] {
            chromatindb::wire::decode_blob(encoded);
        }));
    }

    // Sync throughput: blobs/sec between two in-memory engines
    {
        TempDir src_dir;
        TempDir dst_dir;

        chromatindb::storage::Storage src_storage(src_dir.path.string());
        chromatindb::storage::Storage dst_storage(dst_dir.path.string());
        chromatindb::engine::BlobEngine src_engine(src_storage);
        chromatindb::engine::BlobEngine dst_engine(dst_storage);
        chromatindb::sync::SyncProtocol src_sync(src_engine, src_storage);
        chromatindb::sync::SyncProtocol dst_sync(dst_engine, dst_storage);

        // Seed source with 100 x 1 KiB blobs
        constexpr int sync_blob_count = 100;
        for (int i = 0; i < sync_blob_count; ++i) {
            auto data = random_bytes(1024);
            auto blob = make_signed_blob(id, data, 604800, now + 40000 + i);
            src_engine.ingest(blob);
        }

        // Measure full sync exchange
        int sync_iters = 10;
        results.push_back(run_bench(
            "Sync throughput (100x1KiB)", sync_iters, 0, [&] {
                // Fresh destination each iteration
                TempDir iter_dst_dir;
                chromatindb::storage::Storage iter_dst_storage(iter_dst_dir.path.string());
                chromatindb::engine::BlobEngine iter_dst_engine(iter_dst_storage);
                chromatindb::sync::SyncProtocol iter_dst_sync(iter_dst_engine, iter_dst_storage);

                // Source: list namespaces
                auto namespaces = src_engine.list_namespaces();

                for (const auto& ns_info : namespaces) {
                    // Source: collect hashes
                    auto src_hashes = src_sync.collect_namespace_hashes(ns_info.namespace_id);

                    // Destination: collect own hashes (empty)
                    auto dst_hashes = iter_dst_sync.collect_namespace_hashes(ns_info.namespace_id);

                    // Destination: diff
                    auto missing = chromatindb::sync::SyncProtocol::diff_hashes(dst_hashes, src_hashes);

                    // Source: fetch blobs for missing hashes
                    auto blobs = src_sync.get_blobs_by_hashes(ns_info.namespace_id, missing);

                    // Destination: ingest
                    iter_dst_sync.ingest_blobs(blobs);
                }
            }));

        // Report as blobs/sec: each iteration syncs sync_blob_count blobs
        auto& sync_result = results.back();
        double total_blobs = sync_iters * sync_blob_count;
        double blobs_per_sec = (total_blobs / sync_result.total_ms) * 1000.0;
        sync_result.ops_sec = blobs_per_sec;
        // Adjust avg to be per-blob
        sync_result.avg_us = (sync_result.total_ms * 1000.0) / total_blobs;
    }

    return results;
}

static std::vector<BenchResult> bench_network() {
    std::vector<BenchResult> results;

    // PQ handshake: full 4-step protocol (in-memory, no TCP)
    {
        auto id_init = chromatindb::identity::NodeIdentity::generate();
        auto id_resp = chromatindb::identity::NodeIdentity::generate();

        results.push_back(run_bench("PQ handshake (full)", 10, 1, [&] {
            chromatindb::net::HandshakeInitiator initiator(id_init);
            chromatindb::net::HandshakeResponder responder(id_resp);

            // Step 1: Initiator sends KEM pubkey
            auto kem_pk_msg = initiator.start();

            // Step 2: Responder receives KEM pubkey, returns ciphertext
            auto [resp_err, kem_ct_msg] = responder.receive_kem_pubkey(kem_pk_msg);

            // Step 3: Initiator receives ciphertext, derives keys
            initiator.receive_kem_ciphertext(kem_ct_msg);

            // Step 4: Mutual auth -- initiator sends auth, responder verifies
            auto init_auth = initiator.create_auth_message();
            responder.verify_peer_auth(init_auth);

            // Step 5: Responder sends auth, initiator verifies
            auto resp_auth = responder.create_auth_message();
            initiator.verify_peer_auth(resp_auth);
        }));
    }

    // Notification dispatch latency: measure ingest-to-callback time
    {
        TempDir tmpdir;

        chromatindb::storage::Storage storage(tmpdir.path.string());
        chromatindb::engine::BlobEngine engine(storage);
        chromatindb::sync::SyncProtocol sync_proto(engine, storage);

        auto id = chromatindb::identity::NodeIdentity::generate();
        uint64_t now = static_cast<uint64_t>(std::time(nullptr));

        // Prepare blobs ahead of time
        constexpr int notify_count = 100;
        std::vector<chromatindb::wire::BlobData> blobs;
        blobs.reserve(notify_count);
        for (int i = 0; i < notify_count; ++i) {
            auto data = random_bytes(256);
            blobs.push_back(make_signed_blob(id, data, 604800, now + 50000 + i));
        }

        // Set up notification callback that records timestamps
        std::vector<Clock::time_point> callback_times;
        callback_times.reserve(notify_count);
        sync_proto.set_on_blob_ingested(
            [&](const std::array<uint8_t, 32>&,
                const std::array<uint8_t, 32>&,
                uint64_t, uint32_t, bool) {
                callback_times.push_back(Clock::now());
            });

        // Measure: ingest blobs one at a time, capture dispatch latency
        std::vector<double> latencies_us;
        latencies_us.reserve(notify_count);

        for (const auto& blob : blobs) {
            std::vector<chromatindb::wire::BlobData> single = {blob};
            callback_times.clear();

            auto before = Clock::now();
            sync_proto.ingest_blobs(single);

            if (!callback_times.empty()) {
                auto delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    callback_times[0] - before).count();
                latencies_us.push_back(static_cast<double>(delta_ns) / 1000.0);
            }
        }

        // Compute average latency
        double total_us = 0;
        for (auto l : latencies_us) total_us += l;
        double avg_us = latencies_us.empty() ? 0 : total_us / latencies_us.size();
        double ops_sec = avg_us > 0 ? 1'000'000.0 / avg_us : 0;

        results.push_back({
            "Notification dispatch",
            static_cast<int>(latencies_us.size()),
            total_us / 1000.0,
            avg_us,
            ops_sec
        });
    }

    return results;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "# chromatindb Benchmark Results\n\n";
    std::cout << "_Run with `chromatindb_bench`. Results vary by hardware._\n\n";

    std::cout << "### Crypto Operations\n\n";
    auto sha3_results = bench_sha3();
    auto sign_results = bench_signing();
    auto kem_results = bench_kem();
    auto aead_results = bench_aead();

    std::vector<BenchResult> crypto_all;
    crypto_all.insert(crypto_all.end(), sha3_results.begin(), sha3_results.end());
    crypto_all.insert(crypto_all.end(), sign_results.begin(), sign_results.end());
    crypto_all.insert(crypto_all.end(), kem_results.begin(), kem_results.end());
    crypto_all.insert(crypto_all.end(), aead_results.begin(), aead_results.end());
    print_table(crypto_all);

    std::cout << "### Data Path\n\n";
    auto data_results = bench_data_path();
    print_table(data_results);

    std::cout << "### Network Operations\n\n";
    auto net_results = bench_network();
    print_table(net_results);

    return 0;
}
