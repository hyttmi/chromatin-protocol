// Performance benchmark for Kademlia operations.
// Uses real TCP sockets, real PQ crypto, real mdbx storage on localhost.
//
// Usage: ./chromatin-bench

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "config/config.h"
#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "kademlia/node_id.h"
#include "kademlia/routing_table.h"
#include "kademlia/tcp_transport.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

using namespace chromatin;
using namespace std::chrono;
using Clock = steady_clock;

// ---------------------------------------------------------------------------
// Test node (same as unit tests)
// ---------------------------------------------------------------------------

struct BenchNode {
    config::Config cfg;
    crypto::KeyPair keypair;
    kademlia::NodeInfo info;
    std::unique_ptr<kademlia::TcpTransport> transport;
    std::unique_ptr<kademlia::RoutingTable> table;
    std::unique_ptr<storage::Storage> storage;
    std::unique_ptr<replication::ReplLog> repl_log;
    std::unique_ptr<kademlia::Kademlia> kad;
    std::thread recv_thread;
    std::filesystem::path db_path;

    ~BenchNode() { stop(); }

    void stop() {
        transport->stop();
        if (recv_thread.joinable()) recv_thread.join();
    }

    void start_recv() {
        recv_thread = std::thread([this]() {
            transport->run([this](const kademlia::Message& msg,
                                  const std::string& from, uint16_t port) {
                kad->handle_message(msg, from, port);
            });
        });
    }
};

static std::unique_ptr<BenchNode> make_node() {
    auto n = std::make_unique<BenchNode>();
    n->keypair = crypto::generate_keypair();
    auto nid = kademlia::NodeId::from_pubkey(n->keypair.public_key);

    n->db_path = std::filesystem::temp_directory_path() /
                 ("chromatin_bench_" + std::to_string(reinterpret_cast<uintptr_t>(n.get())));
    std::filesystem::create_directories(n->db_path);
    n->storage = std::make_unique<storage::Storage>(n->db_path / "bench.mdbx");
    n->transport = std::make_unique<kademlia::TcpTransport>("127.0.0.1", 0);

    n->info.id = nid;
    n->info.address = "127.0.0.1";
    n->info.tcp_port = n->transport->local_port();
    n->info.ws_port = 0;
    n->info.pubkey = n->keypair.public_key;
    n->info.last_seen = Clock::now();

    n->table = std::make_unique<kademlia::RoutingTable>();
    n->repl_log = std::make_unique<replication::ReplLog>(*n->storage);
    n->cfg.name_pow_difficulty = 8;
    n->cfg.contact_pow_difficulty = 8;
    n->kad = std::make_unique<kademlia::Kademlia>(
        n->cfg, n->info, *n->transport, *n->table, *n->storage, *n->repl_log, n->keypair);

    return n;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct BenchResult {
    std::string name;
    double mean_ms;
    double min_ms;
    double max_ms;
    double p50_ms;
    double p99_ms;
    int iterations;
};

static BenchResult compute_stats(const std::string& name, std::vector<double>& samples) {
    BenchResult r;
    r.name = name;
    r.iterations = static_cast<int>(samples.size());

    std::sort(samples.begin(), samples.end());
    r.min_ms = samples.front();
    r.max_ms = samples.back();
    r.mean_ms = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
    r.p50_ms = samples[samples.size() / 2];
    r.p99_ms = samples[static_cast<size_t>(samples.size() * 0.99)];
    return r;
}

static void print_results(const std::vector<BenchResult>& results) {
    std::cout << "\n";
    std::cout << std::left << std::setw(32) << "Benchmark"
              << std::right << std::setw(8) << "N"
              << std::setw(12) << "Mean(ms)"
              << std::setw(12) << "P50(ms)"
              << std::setw(12) << "P99(ms)"
              << std::setw(12) << "Min(ms)"
              << std::setw(12) << "Max(ms)"
              << "\n";
    std::cout << std::string(100, '-') << "\n";

    for (const auto& r : results) {
        std::cout << std::left << std::setw(32) << r.name
                  << std::right << std::setw(8) << r.iterations
                  << std::setw(12) << std::fixed << std::setprecision(3) << r.mean_ms
                  << std::setw(12) << r.p50_ms
                  << std::setw(12) << r.p99_ms
                  << std::setw(12) << r.min_ms
                  << std::setw(12) << r.max_ms
                  << "\n";
    }
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

// Keypair generation
static BenchResult bench_keygen(int n) {
    std::vector<double> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        auto kp = crypto::generate_keypair();
        auto t1 = Clock::now();
        (void)kp;
        samples.push_back(duration<double, std::milli>(t1 - t0).count());
    }
    return compute_stats("keygen (ML-DSA-87)", samples);
}

// Sign a message
static BenchResult bench_sign(int n) {
    auto kp = crypto::generate_keypair();
    std::vector<uint8_t> data(256, 0x42);
    std::vector<double> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        auto sig = crypto::sign(data, kp.secret_key);
        auto t1 = Clock::now();
        (void)sig;
        samples.push_back(duration<double, std::milli>(t1 - t0).count());
    }
    return compute_stats("sign (ML-DSA-87, 256B)", samples);
}

// Verify a signature
static BenchResult bench_verify(int n) {
    auto kp = crypto::generate_keypair();
    std::vector<uint8_t> data(256, 0x42);
    auto sig = crypto::sign(data, kp.secret_key);
    std::vector<double> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        bool ok = crypto::verify(data, sig, kp.public_key);
        auto t1 = Clock::now();
        (void)ok;
        samples.push_back(duration<double, std::milli>(t1 - t0).count());
    }
    return compute_stats("verify (ML-DSA-87, 256B)", samples);
}

// SHA3-256
static BenchResult bench_sha3(int n) {
    std::vector<uint8_t> data(1024, 0xAB);
    std::vector<double> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        auto h = crypto::sha3_256(data);
        auto t1 = Clock::now();
        (void)h;
        samples.push_back(duration<double, std::milli>(t1 - t0).count());
    }
    return compute_stats("SHA3-256 (1KB)", samples);
}

// mdbx put + get round-trip
static BenchResult bench_storage(int n) {
    auto tmp = std::filesystem::temp_directory_path() / "chromatin_bench_storage";
    std::filesystem::create_directories(tmp);
    storage::Storage st(tmp / "bench.mdbx");

    std::vector<double> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        crypto::Hash key{};
        key.fill(static_cast<uint8_t>(i & 0xFF));
        key[0] = static_cast<uint8_t>((i >> 8) & 0xFF);
        std::vector<uint8_t> val(512, static_cast<uint8_t>(i));

        auto t0 = Clock::now();
        st.put(storage::TABLE_PROFILES, key, val);
        auto got = st.get(storage::TABLE_PROFILES, key);
        auto t1 = Clock::now();
        (void)got;
        samples.push_back(duration<double, std::milli>(t1 - t0).count());
    }

    // Storage cleans up in destructor — just remove temp dir after scope
    std::filesystem::remove_all(tmp);
    return compute_stats("mdbx put+get (512B)", samples);
}

// TCP serialize + deserialize round-trip (no network)
static BenchResult bench_tcp_serde(int n) {
    auto kp = crypto::generate_keypair();
    kademlia::Message msg;
    msg.type = kademlia::MessageType::STORE;
    msg.sender = kademlia::NodeId::from_pubkey(kp.public_key);
    msg.payload.resize(512, 0xCD);
    sign_message(msg, kp.secret_key);

    auto buf = kademlia::serialize_message(msg);

    std::vector<double> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        auto ser = kademlia::serialize_message(msg);
        auto parsed = kademlia::deserialize_message(ser);
        auto t1 = Clock::now();
        (void)parsed;
        samples.push_back(duration<double, std::milli>(t1 - t0).count());
    }
    return compute_stats("TCP serde (512B payload)", samples);
}

// PING/PONG round-trip over real TCP (localhost)
static BenchResult bench_ping_pong(int n) {
    auto n1 = make_node();
    auto n2 = make_node();
    n1->start_recv();
    n2->start_recv();
    std::this_thread::sleep_for(milliseconds(50));

    // Bootstrap so they know each other
    n2->kad->bootstrap({{"127.0.0.1", n1->info.tcp_port}});
    std::this_thread::sleep_for(milliseconds(500));

    std::vector<double> samples;
    samples.reserve(n);

    for (int i = 0; i < n; ++i) {
        auto old_entry = n1->table->find(n2->info.id);
        auto before = old_entry ? old_entry->last_seen : Clock::now();

        auto t0 = Clock::now();

        // Send PING from n1 to n2
        kademlia::Message ping;
        ping.type = kademlia::MessageType::PING;
        ping.sender = n1->info.id;
        ping.sender_port = n1->info.tcp_port;
        ping.payload = {};
        sign_message(ping, n1->keypair.secret_key);
        n1->transport->send("127.0.0.1", n2->info.tcp_port, ping);

        // Poll for PONG (last_seen update on n2's entry in n1's table)
        bool got_pong = false;
        for (int j = 0; j < 100; ++j) {
            std::this_thread::sleep_for(microseconds(500));
            auto entry = n1->table->find(n2->info.id);
            if (entry && entry->last_seen > before) {
                got_pong = true;
                break;
            }
        }

        auto t1 = Clock::now();
        if (got_pong) {
            samples.push_back(duration<double, std::milli>(t1 - t0).count());
        }
    }

    n1->stop();
    n2->stop();
    std::filesystem::remove_all(n1->db_path);
    std::filesystem::remove_all(n2->db_path);

    return compute_stats("PING/PONG RTT (localhost)", samples);
}

// STORE with quorum (2 nodes, R=2, W=2)
static BenchResult bench_store_quorum(int n) {
    auto n1 = make_node();
    auto n2 = make_node();
    n1->start_recv();
    n2->start_recv();
    std::this_thread::sleep_for(milliseconds(50));

    n2->kad->bootstrap({{"127.0.0.1", n1->info.tcp_port}});
    std::this_thread::sleep_for(milliseconds(500));

    std::vector<double> samples;
    samples.reserve(n);

    for (int i = 0; i < n; ++i) {
        crypto::Hash key{};
        key.fill(0);
        key[0] = static_cast<uint8_t>(i & 0xFF);
        key[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        std::vector<uint8_t> val(256, static_cast<uint8_t>(i));

        auto t0 = Clock::now();
        n1->kad->store(key, 0x00, val);

        // Wait for quorum (pending cleared)
        bool quorum = false;
        for (int j = 0; j < 200; ++j) {
            std::this_thread::sleep_for(microseconds(500));
            if (!n1->kad->pending_store_status(key).has_value()) {
                quorum = true;
                break;
            }
        }

        auto t1 = Clock::now();
        if (quorum) {
            samples.push_back(duration<double, std::milli>(t1 - t0).count());
        }
    }

    n1->stop();
    n2->stop();
    std::filesystem::remove_all(n1->db_path);
    std::filesystem::remove_all(n2->db_path);

    return compute_stats("STORE + quorum (W=2, 256B)", samples);
}

// STORE with 3 nodes (R=3, W=2)
static BenchResult bench_store_3node(int n) {
    auto n1 = make_node();
    auto n2 = make_node();
    auto n3 = make_node();
    n1->start_recv();
    n2->start_recv();
    n3->start_recv();
    std::this_thread::sleep_for(milliseconds(50));

    n2->kad->bootstrap({{"127.0.0.1", n1->info.tcp_port}});
    std::this_thread::sleep_for(milliseconds(500));
    n3->kad->bootstrap({{"127.0.0.1", n1->info.tcp_port}});
    std::this_thread::sleep_for(milliseconds(500));
    n2->kad->bootstrap({{"127.0.0.1", n1->info.tcp_port}});
    std::this_thread::sleep_for(milliseconds(500));

    std::vector<double> samples;
    samples.reserve(n);

    for (int i = 0; i < n; ++i) {
        crypto::Hash key{};
        key.fill(0);
        key[0] = static_cast<uint8_t>(i & 0xFF);
        key[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        std::vector<uint8_t> val(256, static_cast<uint8_t>(i));

        auto t0 = Clock::now();
        n1->kad->store(key, 0x00, val);

        // Wait for all ACKs
        bool done = false;
        for (int j = 0; j < 200; ++j) {
            std::this_thread::sleep_for(microseconds(500));
            if (!n1->kad->pending_store_status(key).has_value()) {
                done = true;
                break;
            }
        }

        auto t1 = Clock::now();
        if (done) {
            samples.push_back(duration<double, std::milli>(t1 - t0).count());
        }
    }

    n1->stop();
    n2->stop();
    n3->stop();
    std::filesystem::remove_all(n1->db_path);
    std::filesystem::remove_all(n2->db_path);
    std::filesystem::remove_all(n3->db_path);

    return compute_stats("STORE + quorum (R=3,W=2,256B)", samples);
}

// Bootstrap / discovery time (how long to discover N nodes)
static BenchResult bench_discovery(int n) {
    std::vector<double> samples;
    samples.reserve(n);

    for (int i = 0; i < n; ++i) {
        auto n1 = make_node();
        auto n2 = make_node();
        auto n3 = make_node();
        n1->start_recv();
        n2->start_recv();
        n3->start_recv();
        std::this_thread::sleep_for(milliseconds(50));

        // n2 bootstraps from n1
        n2->kad->bootstrap({{"127.0.0.1", n1->info.tcp_port}});
        std::this_thread::sleep_for(milliseconds(300));

        // Time: n3 bootstraps and discovers both n1 and n2
        auto t0 = Clock::now();
        n3->kad->bootstrap({{"127.0.0.1", n1->info.tcp_port}});

        bool found_both = false;
        for (int j = 0; j < 200; ++j) {
            std::this_thread::sleep_for(microseconds(500));
            if (n3->table->size() >= 2) {
                found_both = true;
                break;
            }
        }

        auto t1 = Clock::now();
        if (found_both) {
            samples.push_back(duration<double, std::milli>(t1 - t0).count());
        }

        n1->stop();
        n2->stop();
        n3->stop();
        std::filesystem::remove_all(n1->db_path);
        std::filesystem::remove_all(n2->db_path);
        std::filesystem::remove_all(n3->db_path);
    }

    return compute_stats("Discovery (3 nodes, full mesh)", samples);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    spdlog::set_level(spdlog::level::warn);

    std::cout << "Chromatin Performance Benchmark\n";
    std::cout << "===========================\n";
    std::cout << "Real TCP, real PQ crypto (ML-DSA-87), real mdbx storage\n";
    std::cout << "All network tests on localhost (127.0.0.1)\n\n";

    std::vector<BenchResult> results;

    std::cout << "Running: keygen..." << std::flush;
    results.push_back(bench_keygen(50));
    std::cout << " done\n";

    std::cout << "Running: sign..." << std::flush;
    results.push_back(bench_sign(100));
    std::cout << " done\n";

    std::cout << "Running: verify..." << std::flush;
    results.push_back(bench_verify(100));
    std::cout << " done\n";

    std::cout << "Running: SHA3-256..." << std::flush;
    results.push_back(bench_sha3(1000));
    std::cout << " done\n";

    std::cout << "Running: mdbx put+get..." << std::flush;
    results.push_back(bench_storage(500));
    std::cout << " done\n";

    std::cout << "Running: TCP serde..." << std::flush;
    results.push_back(bench_tcp_serde(200));
    std::cout << " done\n";

    std::cout << "Running: PING/PONG..." << std::flush;
    results.push_back(bench_ping_pong(50));
    std::cout << " done\n";

    std::cout << "Running: discovery..." << std::flush;
    results.push_back(bench_discovery(10));
    std::cout << " done\n";

    std::cout << "Running: STORE (2-node)..." << std::flush;
    results.push_back(bench_store_quorum(50));
    std::cout << " done\n";

    std::cout << "Running: STORE (3-node)..." << std::flush;
    results.push_back(bench_store_3node(50));
    std::cout << " done\n";

    print_results(results);

    return 0;
}
