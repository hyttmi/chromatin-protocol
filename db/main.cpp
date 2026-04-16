#include "db/version.h"
#include "db/acl/access_control.h"
#include "db/config/config.h"
#include "db/crypto/hash.h"
#include "db/crypto/thread_pool.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/logging/logging.h"
#include "db/net/auth_helpers.h"
#include "db/net/protocol.h"
#include "db/peer/peer_manager.h"
#include "db/storage/storage.h"
#include "db/util/endian.h"
#include "db/util/hex.h"

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>
#include <nlohmann/json.hpp>
#include <oqs/oqs.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <signal.h>
#include <span>
#include <string>
#include <thread>


namespace {

using chromatindb::util::to_hex;

void print_usage(const char* prog) {
    std::cerr << "chromatindb " << VERSION << "\n\n"
              << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  run          Start the daemon\n"
              << "  keygen       Generate identity keypair\n"
              << "  show-key     Print namespace (public key hash)\n"
              << "  backup       Create a live database backup\n"
              << "  add-peer     Add a bootstrap peer to config\n"
              << "  remove-peer  Remove a bootstrap peer from config\n"
              << "  list-peers   Show configured and connected peers\n"
              << "  version      Print version\n\n"
              << "Run options:\n"
              << "  --config <path>     JSON config file\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n"
              << "  --log-level <lvl>   Log level: trace|debug|info|warn|error (default: info)\n\n"
              << "Keygen options:\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n"
              << "  --force             Overwrite existing identity\n\n"
              << "Show-key options:\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n\n"
              << "Peer management options:\n"
              << "  --config <path>     Config file to edit (default: <data-dir>/config.json)\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n";
}

int cmd_version() {
    std::cout << "chromatindb " << VERSION << std::endl;
    return 0;
}

int cmd_keygen(int argc, char* argv[]) {
    std::string data_dir = "./data";
    bool force = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--force") {
            force = true;
        }
    }

    namespace fs = std::filesystem;
    auto key_path = fs::path(data_dir) / "node.key";
    auto pub_path = fs::path(data_dir) / "node.pub";

    if (fs::exists(key_path) && !force) {
        std::cerr << "Identity already exists at " << data_dir
                  << "/. Use --force to overwrite." << std::endl;
        return 1;
    }

    fs::create_directories(data_dir);

    auto identity = chromatindb::identity::NodeIdentity::generate();
    identity.save_to(data_dir);

    std::cout << "Generated identity at " << data_dir << "/" << std::endl;
    std::cout << "Namespace: " << to_hex(identity.namespace_id()) << std::endl;

    return 0;
}

int cmd_show_key(int argc, char* argv[]) {
    std::string data_dir = "./data";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        }
    }

    try {
        auto identity = chromatindb::identity::NodeIdentity::load_from(data_dir);
        std::cout << "Namespace: " << to_hex(identity.namespace_id()) << std::endl;
    } catch (const std::runtime_error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

int cmd_backup(int argc, char* argv[]) {
    std::string data_dir = "./data";
    std::string dest_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (dest_path.empty() && arg[0] != '-') {
            dest_path = arg;
        }
    }

    if (dest_path.empty()) {
        std::cerr << "Usage: chromatindb backup <dest-path> [--data-dir <path>]\n"
                  << "Creates a live compacted copy of the database at <dest-path>.\n";
        return 1;
    }

    try {
        chromatindb::storage::Storage storage(data_dir);
        if (storage.backup(dest_path)) {
            std::cout << "Backup written to " << dest_path << std::endl;
            return 0;
        } else {
            std::cerr << "Backup failed." << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int cmd_add_peer(int argc, char* argv[]) {
    std::string data_dir = "./data";
    std::string config_path;
    std::string address;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (address.empty() && arg[0] != '-') {
            address = arg;
        }
    }

    if (address.empty()) {
        std::cerr << "Usage: chromatindb add-peer <host:port> [--config <path>] [--data-dir <path>]\n";
        return 1;
    }

    // Resolve config path
    if (config_path.empty()) {
        config_path = (std::filesystem::path(data_dir) / "config.json").string();
    }

    try {
        // Read existing config as raw JSON (preserves all fields)
        nlohmann::json j;
        {
            std::ifstream f(config_path);
            if (f.is_open()) {
                j = nlohmann::json::parse(f);
            }
        }

        // Ensure bootstrap_peers is an array
        if (!j.contains("bootstrap_peers") || !j["bootstrap_peers"].is_array()) {
            j["bootstrap_peers"] = nlohmann::json::array();
        }

        // Check for duplicates
        for (const auto& peer : j["bootstrap_peers"]) {
            if (peer.get<std::string>() == address) {
                std::cerr << "Peer " << address << " already in bootstrap_peers" << std::endl;
                return 1;
            }
        }

        // Add the peer
        j["bootstrap_peers"].push_back(address);

        // Write back with indentation
        {
            std::ofstream f(config_path);
            if (!f.is_open()) {
                std::cerr << "Error: cannot write " << config_path << std::endl;
                return 1;
            }
            f << j.dump(4) << std::endl;
        }

        std::cout << "Added peer " << address << " to " << config_path << std::endl;

        // Send SIGHUP to running node if pidfile exists (D-10)
        auto pidfile = std::filesystem::path(data_dir) / "chromatindb.pid";
        if (std::filesystem::exists(pidfile)) {
            std::ifstream pf(pidfile);
            pid_t pid;
            if (pf >> pid && kill(pid, 0) == 0) {
                kill(pid, SIGHUP);
                std::cout << "Sent SIGHUP to node (PID " << pid << ")" << std::endl;
            }
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int cmd_remove_peer(int argc, char* argv[]) {
    std::string data_dir = "./data";
    std::string config_path;
    std::string address;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (address.empty() && arg[0] != '-') {
            address = arg;
        }
    }

    if (address.empty()) {
        std::cerr << "Usage: chromatindb remove-peer <host:port> [--config <path>] [--data-dir <path>]\n";
        return 1;
    }

    // Resolve config path
    if (config_path.empty()) {
        config_path = (std::filesystem::path(data_dir) / "config.json").string();
    }

    try {
        // Read existing config as raw JSON
        nlohmann::json j;
        {
            std::ifstream f(config_path);
            if (!f.is_open()) {
                std::cerr << "Error: cannot read " << config_path << std::endl;
                return 1;
            }
            j = nlohmann::json::parse(f);
        }

        if (!j.contains("bootstrap_peers") || !j["bootstrap_peers"].is_array()) {
            std::cerr << "No bootstrap_peers array in " << config_path << std::endl;
            return 1;
        }

        // Find and remove the peer
        auto& peers = j["bootstrap_peers"];
        auto it = std::find(peers.begin(), peers.end(), address);
        if (it == peers.end()) {
            std::cerr << "Peer " << address << " not found in bootstrap_peers" << std::endl;
            return 1;
        }
        peers.erase(it);

        // Write back
        {
            std::ofstream f(config_path);
            if (!f.is_open()) {
                std::cerr << "Error: cannot write " << config_path << std::endl;
                return 1;
            }
            f << j.dump(4) << std::endl;
        }

        std::cout << "Removed peer " << address << " from " << config_path << std::endl;

        // Send SIGHUP to running node if pidfile exists (D-10)
        auto pidfile = std::filesystem::path(data_dir) / "chromatindb.pid";
        if (std::filesystem::exists(pidfile)) {
            std::ifstream pf(pidfile);
            pid_t pid;
            if (pf >> pid && kill(pid, 0) == 0) {
                kill(pid, SIGHUP);
                std::cout << "Sent SIGHUP to node (PID " << pid << ")" << std::endl;
            }
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

// Forward declaration -- implemented in Task 2
int cmd_list_peers(int argc, char* argv[]);

int cmd_run(int argc, char* argv[]) {
    // Parse args
    std::vector<const char*> args;
    args.push_back("chromatindb");
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    chromatindb::config::Config config;
    try {
        config = chromatindb::config::parse_args(
            static_cast<int>(args.size()), args.data());
        chromatindb::config::validate_config(config);
    } catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    // Initialize logging
    chromatindb::logging::init(config.log_level,
                                config.log_file,
                                config.log_max_size_mb,
                                config.log_max_files,
                                config.log_format);

    spdlog::info("chromatindb {}", VERSION);
    spdlog::info("bind: {}", config.bind_address);
    spdlog::info("data: {}", config.data_dir);

    // Create/load identity
    auto identity = chromatindb::identity::NodeIdentity::load_or_generate(config.data_dir);
    spdlog::info("namespace: {}", to_hex(identity.namespace_id()));

    // Log bootstrap peers
    for (const auto& peer : config.bootstrap_peers) {
        spdlog::info("bootstrap: {}", peer);
    }
    if (config.bootstrap_peers.empty()) {
        spdlog::info("no bootstrap peers configured (accepting inbound only)");
    }

    spdlog::info("max peers: {}", config.max_peers);
    spdlog::info("safety-net interval: {}s", config.safety_net_interval_seconds);
    if (config.compaction_interval_hours > 0) {
        spdlog::info("compaction interval: {}h", config.compaction_interval_hours);
    } else {
        spdlog::info("compaction: disabled");
    }
    if (!config.uds_path.empty()) {
        spdlog::info("uds: {}", config.uds_path);
    } else {
        spdlog::info("uds: disabled");
    }
    spdlog::info("blob_transfer_timeout: {}s", config.blob_transfer_timeout);
    spdlog::info("sync_timeout: {}s", config.sync_timeout);
    spdlog::info("pex_interval: {}s", config.pex_interval);
    spdlog::info("strike_threshold: {}", config.strike_threshold);
    spdlog::info("strike_cooldown: {}s", config.strike_cooldown);

    // Resolve and create thread pool for crypto offload
    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2;
    if (config.worker_threads > hw) {
        spdlog::warn("worker_threads {} exceeds hardware_concurrency {}, clamped",
                     config.worker_threads, hw);
    }
    auto num_workers = chromatindb::crypto::resolve_worker_threads(config.worker_threads);
    spdlog::info("worker threads: {}{}",
                 num_workers,
                 config.worker_threads == 0 ? " (auto-detected)" : " (configured)");

    asio::thread_pool pool(num_workers);

    // Create components
    chromatindb::storage::Storage storage(config.data_dir);
    storage.integrity_scan();

    chromatindb::engine::BlobEngine engine(storage, pool, config.max_storage_bytes,
                                           config.namespace_quota_bytes,
                                           config.namespace_quota_count,
                                           config.max_ttl_seconds);
    if (!config.namespace_quotas.empty()) {
        engine.set_quota_config(config.namespace_quota_bytes,
                                config.namespace_quota_count,
                                config.namespace_quotas);
    }
    asio::io_context ioc;

    chromatindb::acl::AccessControl acl(config.allowed_client_keys, config.allowed_peer_keys, identity.namespace_id());
    chromatindb::peer::PeerManager pm(config, identity, engine, storage, ioc, pool, acl, config.config_path);
    pm.start();

    spdlog::info("daemon started");

    // Write pidfile for peer management subcommands (Phase 118)
    auto pidfile = std::filesystem::path(config.data_dir) / "chromatindb.pid";
    {
        std::ofstream pf(pidfile);
        pf << getpid();
    }

    // Run event loop (expiry scanning now lives in PeerManager)
    ioc.run();

    // Clean up pidfile
    std::filesystem::remove(pidfile);

    // Wait for in-flight crypto operations to complete
    pool.join();

    return pm.exit_code();
}

static std::string format_duration(uint64_t ms) {
    if (ms < 1000) return std::to_string(ms) + "ms";
    uint64_t secs = ms / 1000;
    if (secs < 60) return std::to_string(secs) + "s";
    uint64_t mins = secs / 60;
    if (mins < 60) return std::to_string(mins) + "m " + std::to_string(secs % 60) + "s";
    uint64_t hours = mins / 60;
    return std::to_string(hours) + "h " + std::to_string(mins % 60) + "m";
}

int cmd_list_peers(int argc, char* argv[]) {
    std::string data_dir = "./data";
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        config_path = (std::filesystem::path(data_dir) / "config.json").string();
    }

    try {
        auto config = chromatindb::config::load_config(config_path);
        auto bootstrap_set = std::set<std::string>(
            config.bootstrap_peers.begin(), config.bootstrap_peers.end());

        struct PeerEntry {
            std::string address;
            bool is_bootstrap;
            bool syncing;
            bool peer_is_full;
            uint64_t duration_ms;
        };
        std::vector<PeerEntry> connected_peers;
        std::set<std::string> connected_addresses;
        bool node_running = false;

        // --- UDS query block (D-11): full TrustedHello handshake ---
        if (!config.uds_path.empty() && std::filesystem::exists(config.uds_path)) {
            try {
                // B1. Load identity from data_dir
                auto identity = chromatindb::identity::NodeIdentity::load_from(data_dir);
                auto signing_pk = identity.public_key();

                // B2. Connect UDS socket synchronously
                asio::io_context ioc;
                asio::local::stream_protocol::socket sock(ioc);
                asio::local::stream_protocol::endpoint ep(config.uds_path);
                sock.connect(ep);

                // -- Helper lambdas for synchronous framed I/O --
                // Wire format: [4-byte BE length][data]
                auto write_frame = [&](std::span<const uint8_t> data) {
                    uint8_t hdr[4];
                    chromatindb::util::store_u32_be(hdr, static_cast<uint32_t>(data.size()));
                    asio::write(sock, asio::buffer(hdr, 4));
                    if (!data.empty()) asio::write(sock, asio::buffer(data.data(), data.size()));
                };

                auto read_frame = [&]() -> std::vector<uint8_t> {
                    uint8_t hdr[4];
                    asio::read(sock, asio::buffer(hdr, 4));
                    uint32_t len = chromatindb::util::read_u32_be(hdr);
                    std::vector<uint8_t> buf(len);
                    if (len > 0) asio::read(sock, asio::buffer(buf.data(), buf.size()));
                    return buf;
                };

                // AEAD state
                std::vector<uint8_t> send_key(32), recv_key(32);
                uint64_t send_counter = 0, recv_counter = 0;

                // AEAD nonce: [4 zero bytes][8-byte BE counter]
                auto make_nonce = [](uint64_t ctr) -> std::array<uint8_t, 12> {
                    std::array<uint8_t, 12> n{};
                    chromatindb::util::store_u64_be(n.data() + 4, ctr);
                    return n;
                };

                auto send_encrypted = [&](std::span<const uint8_t> plaintext) {
                    auto nonce = make_nonce(send_counter);
                    std::vector<uint8_t> ct(plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
                    unsigned long long ct_len = 0;
                    crypto_aead_chacha20poly1305_ietf_encrypt(
                        ct.data(), &ct_len,
                        plaintext.data(), plaintext.size(),
                        nullptr, 0,
                        nullptr, nonce.data(), send_key.data());
                    ct.resize(ct_len);
                    ++send_counter;
                    write_frame(ct);
                };

                auto recv_encrypted = [&]() -> std::vector<uint8_t> {
                    auto raw = read_frame();
                    auto nonce = make_nonce(recv_counter);
                    std::vector<uint8_t> pt(raw.size());
                    unsigned long long pt_len = 0;
                    if (crypto_aead_chacha20poly1305_ietf_decrypt(
                            pt.data(), &pt_len,
                            nullptr,
                            raw.data(), raw.size(),
                            nullptr, 0,
                            nonce.data(), recv_key.data()) != 0) {
                        throw std::runtime_error("AEAD decrypt failed");
                    }
                    pt.resize(pt_len);
                    ++recv_counter;
                    return pt;
                };

                // B3. TrustedHello exchange (type 23)
                // Generate 32-byte random nonce
                constexpr size_t NONCE_SIZE = 32;
                constexpr size_t SIGNING_PK_SIZE = 2592;

                std::array<uint8_t, NONCE_SIZE> nonce_i{};
                randombytes_buf(nonce_i.data(), nonce_i.size());

                // Build TrustedHello payload: [nonce:32][signing_pk:2592]
                std::vector<uint8_t> hello_payload;
                hello_payload.reserve(NONCE_SIZE + signing_pk.size());
                hello_payload.insert(hello_payload.end(), nonce_i.begin(), nonce_i.end());
                hello_payload.insert(hello_payload.end(), signing_pk.begin(), signing_pk.end());

                // Encode as TransportMessage type 23, send raw (pre-encryption)
                auto hello_msg = chromatindb::net::TransportCodec::encode(
                    chromatindb::wire::TransportMsgType_TrustedHello,
                    hello_payload, 0);
                write_frame(hello_msg);

                // Receive responder's TrustedHello
                auto resp_raw = read_frame();
                auto resp = chromatindb::net::TransportCodec::decode(resp_raw);
                if (!resp || resp->type != chromatindb::wire::TransportMsgType_TrustedHello) {
                    throw std::runtime_error("invalid TrustedHello response");
                }

                // Parse responder nonce + pubkey from payload
                if (resp->payload.size() != NONCE_SIZE + SIGNING_PK_SIZE) {
                    throw std::runtime_error("invalid TrustedHello payload size");
                }
                auto nonce_r = std::span<const uint8_t>(resp->payload.data(), NONCE_SIZE);
                auto resp_pk = std::span<const uint8_t>(resp->payload.data() + NONCE_SIZE, SIGNING_PK_SIZE);

                // B4. HKDF-SHA256 key derivation
                // IKM = initiator_nonce || responder_nonce (64 bytes)
                std::vector<uint8_t> ikm;
                ikm.reserve(64);
                ikm.insert(ikm.end(), nonce_i.begin(), nonce_i.end());
                ikm.insert(ikm.end(), nonce_r.begin(), nonce_r.end());

                // Salt = initiator_signing_pk || responder_signing_pk (5184 bytes)
                std::vector<uint8_t> salt;
                salt.reserve(signing_pk.size() + resp_pk.size());
                salt.insert(salt.end(), signing_pk.begin(), signing_pk.end());
                salt.insert(salt.end(), resp_pk.begin(), resp_pk.end());

                // Extract PRK
                uint8_t prk[32];
                crypto_kdf_hkdf_sha256_extract(prk, salt.data(), salt.size(),
                                                ikm.data(), ikm.size());

                // Expand: initiator->responder key (our send key)
                uint8_t init_to_resp[32];
                crypto_kdf_hkdf_sha256_expand(init_to_resp, 32,
                                               "chromatin-init-to-resp-v1", 25, prk);

                // Expand: responder->initiator key (our recv key)
                uint8_t resp_to_init[32];
                crypto_kdf_hkdf_sha256_expand(resp_to_init, 32,
                                               "chromatin-resp-to-init-v1", 25, prk);

                send_key.assign(init_to_resp, init_to_resp + 32);
                recv_key.assign(resp_to_init, resp_to_init + 32);

                // Securely erase PRK
                sodium_memzero(prk, sizeof(prk));

                // B5. Auth exchange (AEAD-encrypted, type 3)
                // Session fingerprint = SHA3-256(IKM || Salt)
                std::vector<uint8_t> fp_input;
                fp_input.reserve(ikm.size() + salt.size());
                fp_input.insert(fp_input.end(), ikm.begin(), ikm.end());
                fp_input.insert(fp_input.end(), salt.begin(), salt.end());
                auto fingerprint = chromatindb::crypto::sha3_256(fp_input);

                // Sign fingerprint
                auto sig = identity.sign(fingerprint);

                // Encode auth payload: [4B pubkey_size_be][pubkey][signature]
                auto auth_payload = chromatindb::net::encode_auth_payload(signing_pk, sig);

                // Encode as TransportMessage type 3, encrypt, send
                auto auth_msg = chromatindb::net::TransportCodec::encode(
                    chromatindb::wire::TransportMsgType_AuthSignature,
                    auth_payload, 0);
                send_encrypted(auth_msg);

                // Receive responder's AuthSignature (type 3) -- decrypt + decode
                auto resp_auth_pt = recv_encrypted();
                auto resp_auth = chromatindb::net::TransportCodec::decode(resp_auth_pt);
                if (!resp_auth || resp_auth->type != chromatindb::wire::TransportMsgType_AuthSignature) {
                    throw std::runtime_error("invalid auth response");
                }

                // Verify responder's auth (pubkey match + signature over fingerprint)
                auto auth_data = chromatindb::net::decode_auth_payload(resp_auth->payload);
                if (!auth_data) {
                    throw std::runtime_error("malformed auth payload");
                }
                if (auth_data->pubkey.size() != resp_pk.size() ||
                    std::memcmp(auth_data->pubkey.data(), resp_pk.data(), resp_pk.size()) != 0) {
                    throw std::runtime_error("auth pubkey mismatch");
                }
                // Verify signature
                OQS_SIG* oqs_sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
                if (!oqs_sig) throw std::runtime_error("OQS_SIG_new failed");
                auto verify_rc = OQS_SIG_verify(oqs_sig,
                    fingerprint.data(), fingerprint.size(),
                    auth_data->signature.data(), auth_data->signature.size(),
                    auth_data->pubkey.data());
                OQS_SIG_free(oqs_sig);
                if (verify_rc != OQS_SUCCESS) {
                    throw std::runtime_error("peer auth signature invalid");
                }

                // B6. Drain SyncNamespaceAnnounce (type 62)
                auto announce_pt = recv_encrypted();
                // Type 62 is expected; any other type is a warning but not fatal
                (void)chromatindb::net::TransportCodec::decode(announce_pt);

                // B7. Send PeerInfoRequest (type 55, empty payload)
                auto peer_req = chromatindb::net::TransportCodec::encode(
                    chromatindb::wire::TransportMsgType_PeerInfoRequest,
                    std::span<const uint8_t>{}, 1);
                send_encrypted(peer_req);

                // B8. Receive PeerInfoResponse (type 56), parse binary format
                auto resp_peer_pt = recv_encrypted();
                auto resp_peer = chromatindb::net::TransportCodec::decode(resp_peer_pt);
                if (!resp_peer || resp_peer->type != chromatindb::wire::TransportMsgType_PeerInfoResponse) {
                    throw std::runtime_error("invalid PeerInfoResponse");
                }

                // Parse trusted format:
                // [4B peer_count_be][4B bootstrap_count_be]
                // Per peer: [2B addr_len_be][addr_bytes][1B is_bootstrap][1B syncing][1B peer_is_full][8B duration_ms_be]
                auto& pd = resp_peer->payload;
                if (pd.size() < 8) throw std::runtime_error("PeerInfoResponse too short");

                uint32_t peer_count = chromatindb::util::read_u32_be(pd.data());
                // uint32_t bootstrap_count = chromatindb::util::read_u32_be(pd.data() + 4);
                size_t off = 8;
                for (uint32_t i = 0; i < peer_count && off + 2 <= pd.size(); ++i) {
                    uint16_t addr_len = chromatindb::util::read_u16_be(
                        std::span<const uint8_t>(pd.data() + off, 2));
                    off += 2;
                    if (off + addr_len + 3 + 8 > pd.size()) break;  // bounds check
                    std::string address(reinterpret_cast<const char*>(pd.data() + off), addr_len);
                    off += addr_len;
                    bool is_bootstrap = pd[off++] == 0x01;
                    bool syncing = pd[off++] == 0x01;
                    bool peer_is_full = pd[off++] == 0x01;
                    uint64_t duration_ms = chromatindb::util::read_u64_be(pd.data() + off);
                    off += 8;
                    connected_peers.push_back({address, is_bootstrap, syncing, peer_is_full, duration_ms});
                }

                node_running = true;
                sock.close();

            } catch (const std::exception&) {
                // D-14 fallback: UDS query failed, show config only
                node_running = false;
            }
        }

        // D-14 fallback header
        if (!node_running) {
            std::cout << "Node is not running (showing config only)\n\n";
        }

        // Print table header
        std::cout << std::left
                  << std::setw(30) << "ADDRESS"
                  << std::setw(12) << "STATUS"
                  << std::setw(12) << "BOOTSTRAP"
                  << std::setw(10) << "SYNCING"
                  << std::setw(10) << "FULL"
                  << "UPTIME" << std::endl;
        std::cout << std::string(84, '-') << std::endl;

        // Print connected peers from UDS response (D-12)
        for (const auto& p : connected_peers) {
            connected_addresses.insert(p.address);
            auto uptime = format_duration(p.duration_ms);
            std::cout << std::left
                      << std::setw(30) << p.address
                      << std::setw(12) << "connected"
                      << std::setw(12) << (p.is_bootstrap ? "yes" : "no")
                      << std::setw(10) << (p.syncing ? "yes" : "no")
                      << std::setw(10) << (p.peer_is_full ? "yes" : "no")
                      << uptime << std::endl;
        }

        // Print disconnected bootstrap peers (in config but not connected -- D-12)
        for (const auto& addr : bootstrap_set) {
            if (connected_addresses.count(addr) == 0) {
                std::cout << std::left
                          << std::setw(30) << addr
                          << std::setw(12) << "disconnected"
                          << std::setw(12) << "yes"
                          << std::setw(10) << "-"
                          << std::setw(10) << "-"
                          << "-" << std::endl;
            }
        }

        if (connected_peers.empty() && bootstrap_set.empty()) {
            std::cout << "(no peers configured)" << std::endl;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "run") return cmd_run(argc - 1, argv + 1);
    if (cmd == "keygen") return cmd_keygen(argc - 1, argv + 1);
    if (cmd == "show-key") return cmd_show_key(argc - 1, argv + 1);
    if (cmd == "backup") return cmd_backup(argc - 1, argv + 1);
    if (cmd == "add-peer") return cmd_add_peer(argc - 1, argv + 1);
    if (cmd == "remove-peer") return cmd_remove_peer(argc - 1, argv + 1);
    if (cmd == "list-peers") return cmd_list_peers(argc - 1, argv + 1);
    if (cmd == "version" || cmd == "--version" || cmd == "-v") return cmd_version();
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { print_usage(argv[0]); return 0; }

    std::cerr << "Unknown command: " << cmd << std::endl;
    print_usage(argv[0]);
    return 1;
}
