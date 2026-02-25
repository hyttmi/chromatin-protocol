// Integration test binary — acts as the 11th node in a 10-node Docker cluster.
// Bootstraps into the network, then runs store/quorum/find/sync tests.
//
// Usage: chromatin-integration-test --config <path>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
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
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string hex(const crypto::Hash& h) {
    std::ostringstream oss;
    for (auto b : h) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

static crypto::Hash make_test_key(const std::string& label) {
    std::vector<uint8_t> data(label.begin(), label.end());
    return crypto::sha3_256(data);
}

// Build a valid signed profile binary for the given keypair.
// fingerprint(32) || pk_len(2 BE) || pubkey || kem_pk_len(2 BE=0) ||
// bio_len(2 BE) || bio || avatar_len(4 BE=0) || social_count(1=0) ||
// sequence(8 BE) || sig_len(2 BE) || signature
static std::vector<uint8_t> build_test_profile(
    const crypto::KeyPair& kp,
    const crypto::Hash& fingerprint,
    const std::string& bio,
    uint64_t sequence)
{
    std::vector<uint8_t> buf;

    // fingerprint (32)
    buf.insert(buf.end(), fingerprint.begin(), fingerprint.end());

    // pubkey_len (2 BE) + pubkey
    uint16_t pk_len = static_cast<uint16_t>(kp.public_key.size());
    buf.push_back(static_cast<uint8_t>(pk_len >> 8));
    buf.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    buf.insert(buf.end(), kp.public_key.begin(), kp.public_key.end());

    // kem_pubkey_len (2 BE = 0)
    buf.push_back(0x00); buf.push_back(0x00);

    // bio_len (2 BE) + bio
    uint16_t bio_len = static_cast<uint16_t>(bio.size());
    buf.push_back(static_cast<uint8_t>(bio_len >> 8));
    buf.push_back(static_cast<uint8_t>(bio_len & 0xFF));
    buf.insert(buf.end(), bio.begin(), bio.end());

    // avatar_len (4 BE = 0)
    buf.push_back(0x00); buf.push_back(0x00);
    buf.push_back(0x00); buf.push_back(0x00);

    // social_count (1 = 0)
    buf.push_back(0x00);

    // sequence (8 BE)
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));

    // Sign everything up to here
    auto sig = crypto::sign(buf, kp.secret_key);

    // sig_len (2 BE) + signature
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    buf.push_back(static_cast<uint8_t>(sig_len >> 8));
    buf.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    buf.insert(buf.end(), sig.begin(), sig.end());

    return buf;
}

// ---------------------------------------------------------------------------
// Intercepting handler: captures VALUE and SYNC_RESP for test assertions
// ---------------------------------------------------------------------------

struct TestContext {
    kademlia::Kademlia* kad = nullptr;

    std::mutex mu;

    // Captured VALUE responses: key -> {found, value}
    struct ValueResp {
        bool found = false;
        std::vector<uint8_t> value;
    };
    std::vector<ValueResp> value_responses;

    // Captured SYNC_RESP responses: key -> entry_count
    struct SyncResp {
        crypto::Hash key{};
        uint16_t entry_count = 0;
    };
    std::vector<SyncResp> sync_responses;

    void handle(const kademlia::Message& msg, const std::string& from, uint16_t port) {
        if (msg.type == kademlia::MessageType::VALUE) {
            parse_value(msg);
            return;
        }
        if (msg.type == kademlia::MessageType::SYNC_RESP) {
            parse_sync_resp(msg);
            return;
        }
        // Everything else goes to kademlia
        kad->handle_message(msg, from, port);
    }

private:
    void parse_value(const kademlia::Message& msg) {
        const auto& data = msg.payload;
        // VALUE: [32 key][1 found][4 BE vlen][vlen value]
        if (data.size() < 37) return;

        ValueResp vr;
        vr.found = data[32] == 0x01;

        uint32_t vlen = (static_cast<uint32_t>(data[33]) << 24)
                      | (static_cast<uint32_t>(data[34]) << 16)
                      | (static_cast<uint32_t>(data[35]) << 8)
                      | static_cast<uint32_t>(data[36]);

        if (37 + vlen <= data.size()) {
            vr.value.assign(data.begin() + 37, data.begin() + 37 + vlen);
        }

        std::lock_guard lock(mu);
        value_responses.push_back(std::move(vr));
    }

    void parse_sync_resp(const kademlia::Message& msg) {
        const auto& data = msg.payload;
        // SYNC_RESP: [32 key][2 BE entry_count][entries...]
        if (data.size() < 34) return;

        SyncResp sr;
        std::copy_n(data.data(), 32, sr.key.begin());
        sr.entry_count = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[32]) << 8) | data[33]);

        std::lock_guard lock(mu);
        sync_responses.push_back(sr);
    }
};

// ---------------------------------------------------------------------------
// Test result tracking
// ---------------------------------------------------------------------------

struct TestResult {
    std::string name;
    bool passed = false;
    std::string detail;
};

static void print_results(const std::vector<TestResult>& results) {
    std::cout << "\n========================================\n";
    std::cout << "  INTEGRATION TEST RESULTS\n";
    std::cout << "========================================\n";
    for (const auto& r : results) {
        std::cout << (r.passed ? "  PASS" : "  FAIL") << "  " << r.name;
        if (!r.detail.empty()) {
            std::cout << " — " << r.detail;
        }
        std::cout << "\n";
    }
    std::cout << "========================================\n";

    int passed = 0;
    for (const auto& r : results) {
        if (r.passed) ++passed;
    }
    std::cout << "  " << passed << "/" << results.size() << " passed\n";
    std::cout << "========================================\n\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("chromatin integration test starting");

    // --- Parse --config ---
    std::filesystem::path config_path;
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config_path = argv[++i];
        }
    }
    if (config_path.empty()) {
        spdlog::error("usage: {} --config <path>", argv[0]);
        return 1;
    }

    // --- Load config ---
    config::Config cfg;
    try {
        cfg = config::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("config error: {}", e.what());
        return 1;
    }

    // --- Setup node stack ---
    std::filesystem::create_directories(cfg.data_dir);

    crypto::KeyPair keypair;
    try {
        keypair = config::load_or_generate_keypair(cfg.data_dir);
    } catch (const std::exception& e) {
        spdlog::error("keypair error: {}", e.what());
        return 1;
    }

    auto node_id = kademlia::NodeId::from_pubkey(keypair.public_key);
    spdlog::info("test node id: {}", hex(node_id.id));

    kademlia::NodeInfo self;
    self.id = node_id;
    self.address = cfg.bind;
    self.tcp_port = cfg.tcp_port;
    self.ws_port = cfg.ws_port;
    self.pubkey = keypair.public_key;
    self.last_seen = std::chrono::steady_clock::now();

    auto db_path = cfg.data_dir / "chromatin.mdbx";
    storage::Storage storage(db_path);
    replication::ReplLog repl_log(storage);
    kademlia::RoutingTable routing_table;
    kademlia::TcpTransport transport(cfg.bind, cfg.tcp_port);

    kademlia::Kademlia kademlia(cfg, self, transport, routing_table, storage, repl_log, keypair);
    kademlia.set_name_pow_difficulty(8);
    kademlia.set_contact_pow_difficulty(8);

    // --- Setup intercepting handler ---
    TestContext ctx;
    ctx.kad = &kademlia;

    std::thread recv_thread([&]() {
        transport.run([&](const kademlia::Message& msg,
                         const std::string& from_addr, uint16_t from_port) {
            ctx.handle(msg, from_addr, from_port);
        });
    });

    // --- Bootstrap ---
    spdlog::info("bootstrapping from {} peer(s)", cfg.bootstrap.size());
    kademlia.set_bootstrap_addrs(cfg.bootstrap);
    kademlia.bootstrap(cfg.bootstrap);

    // --- Wait for routing table to discover nodes ---
    // tick() handles iterative re-bootstrap and peer discovery automatically.
    constexpr size_t MIN_NODES = 9;
    constexpr auto DISCOVERY_TIMEOUT = 30s;

    auto start = std::chrono::steady_clock::now();
    bool discovered = false;

    while (std::chrono::steady_clock::now() - start < DISCOVERY_TIMEOUT) {
        size_t n = routing_table.size();
        if (n >= MIN_NODES) {
            spdlog::info("discovered {} nodes in routing table", n);
            discovered = true;
            break;
        }

        kademlia.tick();
        std::this_thread::sleep_for(200ms);
    }

    std::vector<TestResult> results;

    // === TEST 0: Discovery ===
    {
        TestResult r;
        r.name = "Discovery";
        size_t n = routing_table.size();
        r.passed = discovered;
        r.detail = std::to_string(n) + " nodes discovered (need " + std::to_string(MIN_NODES) + ")";
        results.push_back(r);

        if (!discovered) {
            spdlog::error("discovery failed — aborting remaining tests");
            print_results(results);
            transport.stop();
            recv_thread.join();
            return 1;
        }
    }

    // Give a moment for the network to settle
    std::this_thread::sleep_for(1s);

    // === TEST 1: Store ===
    // Store 5 valid signed profiles. Each uses a freshly-generated keypair
    // so the fingerprint and storage key are correct.
    constexpr int NUM_KEYS = 5;
    std::vector<crypto::Hash> test_keys;
    std::vector<std::vector<uint8_t>> test_values;

    {
        TestResult r;
        r.name = "Store";
        int stored = 0;

        for (int i = 0; i < NUM_KEYS; ++i) {
            // Generate a unique keypair for each test profile
            crypto::KeyPair test_kp = crypto::generate_keypair();
            auto fp = crypto::sha3_256(test_kp.public_key);

            // Storage key = SHA3-256("chromatin:profile:" || fingerprint)
            auto key = crypto::sha3_256_prefixed("chromatin:profile:", fp);

            // Build a valid signed profile
            std::string bio = "integration-test-" + std::to_string(i);
            auto val = build_test_profile(test_kp, fp, bio, 1);

            test_keys.push_back(key);
            test_values.push_back(val);

            bool ok = kademlia.store(key, 0x00, val);
            if (ok) ++stored;
            spdlog::info("store key {} => {}", hex(key).substr(0, 16), ok ? "ok" : "FAIL");
        }

        r.passed = (stored == NUM_KEYS);
        r.detail = std::to_string(stored) + "/" + std::to_string(NUM_KEYS) + " stored";
        results.push_back(r);
    }

    // === TEST 2: Quorum ===
    // Wait for pending_store_status to show quorum met (all ACKs received).
    // When all ACKs arrive, the pending entry is erased — so nullopt = completed.
    {
        TestResult r;
        r.name = "Quorum";
        constexpr auto QUORUM_TIMEOUT = 10s;
        auto qstart = std::chrono::steady_clock::now();
        int quorum_met = 0;

        while (std::chrono::steady_clock::now() - qstart < QUORUM_TIMEOUT) {
            quorum_met = 0;
            for (const auto& key : test_keys) {
                auto status = kademlia.pending_store_status(key);
                if (!status.has_value()) {
                    // No pending entry => all ACKs received => quorum met
                    ++quorum_met;
                } else {
                    size_t confirmed = status->acked + (status->local_stored ? 1 : 0);
                    if (confirmed >= kademlia.write_quorum()) {
                        ++quorum_met;
                    }
                }
            }
            if (quorum_met == NUM_KEYS) break;
            std::this_thread::sleep_for(200ms);
        }

        r.passed = (quorum_met == NUM_KEYS);
        r.detail = std::to_string(quorum_met) + "/" + std::to_string(NUM_KEYS) + " quorum met (W=" + std::to_string(kademlia.write_quorum()) + ")";
        results.push_back(r);
    }

    // === TEST 3: Find ===
    // Send FIND_VALUE to responsible nodes for each key. The intercepting
    // handler captures VALUE responses. We check that at least one response
    // per key has found=true.
    {
        TestResult r;
        r.name = "Find";

        // Clear previous responses
        {
            std::lock_guard lock(ctx.mu);
            ctx.value_responses.clear();
        }

        // Send FIND_VALUE for each key to responsible nodes
        for (const auto& key : test_keys) {
            auto resp_nodes = kademlia.responsible_nodes(key);
            std::vector<uint8_t> payload(key.begin(), key.end());

            for (const auto& node : resp_nodes) {
                if (node.id == self.id) continue;
                kademlia::Message msg;
                msg.type = kademlia::MessageType::FIND_VALUE;
                msg.sender = self.id;
                msg.sender_port = self.tcp_port;
                msg.payload = payload;
                sign_message(msg, keypair.secret_key);
                transport.send(node.address, node.tcp_port, msg);
            }
        }

        // Wait for VALUE responses
        std::this_thread::sleep_for(3s);

        int found_count = 0;
        {
            std::lock_guard lock(ctx.mu);
            for (const auto& vr : ctx.value_responses) {
                if (vr.found) ++found_count;
            }
        }

        r.passed = (found_count >= NUM_KEYS);
        r.detail = std::to_string(found_count) + " VALUE responses with found=true (need " + std::to_string(NUM_KEYS) + ")";
        results.push_back(r);
    }

    // === TEST 4: Sync ===
    // Send SYNC_REQ (after_seq=0) for each key to responsible nodes.
    // Verify we get SYNC_RESP with entry_count > 0 for at least some keys.
    {
        TestResult r;
        r.name = "Sync";

        // Clear previous responses
        {
            std::lock_guard lock(ctx.mu);
            ctx.sync_responses.clear();
        }

        for (const auto& key : test_keys) {
            auto resp_nodes = kademlia.responsible_nodes(key);

            // Build SYNC_REQ payload: [32 key][8 BE after_seq=0]
            std::vector<uint8_t> payload;
            payload.insert(payload.end(), key.begin(), key.end());
            for (int i = 0; i < 8; ++i) payload.push_back(0x00); // after_seq = 0

            for (const auto& node : resp_nodes) {
                if (node.id == self.id) continue;
                kademlia::Message msg;
                msg.type = kademlia::MessageType::SYNC_REQ;
                msg.sender = self.id;
                msg.sender_port = self.tcp_port;
                msg.payload = payload;
                sign_message(msg, keypair.secret_key);
                transport.send(node.address, node.tcp_port, msg);
            }
        }

        // Wait for SYNC_RESP responses
        std::this_thread::sleep_for(3s);

        int sync_with_entries = 0;
        {
            std::lock_guard lock(ctx.mu);
            for (const auto& sr : ctx.sync_responses) {
                if (sr.entry_count > 0) ++sync_with_entries;
            }
        }

        r.passed = (sync_with_entries >= NUM_KEYS);
        r.detail = std::to_string(sync_with_entries) + " SYNC_RESP with entries (need " + std::to_string(NUM_KEYS) + ")";
        results.push_back(r);
    }

    // --- Results ---
    print_results(results);

    // --- Shutdown ---
    transport.stop();
    recv_thread.join();

    int total_passed = 0;
    for (const auto& r : results) {
        if (r.passed) ++total_passed;
    }

    return (total_passed == static_cast<int>(results.size())) ? 0 : 1;
}
