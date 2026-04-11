// relay_feature_test -- operational behavior tests for chromatindb relay
//
// Tests pub/sub notification fan-out (E2E-02), rate limiting (E2E-03),
// SIGHUP config reload (E2E-04), and SIGTERM graceful shutdown (E2E-05).
//
// Usage:
//   relay_feature_test --identity <key_path>
//                      --relay-pid <PID>
//                      --config <relay_config_path>
//                      [--host <addr>] [--port <port>]
//
// Requires a running relay+node. The relay PID is needed for signal delivery.
// The config path is needed for SIGHUP test (modifies rate_limit_messages_per_sec).
// SIGTERM test runs LAST since it kills the relay process.

#include "relay_test_helpers.h"

#include <cerrno>     // errno, ESRCH
#include <csignal>    // kill()
#include <sys/wait.h> // waitpid (D-12)
#include <fstream>    // config rewrite

using namespace relay_test;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void test_pubsub(const std::string& host, int port,
                        const identity::RelayIdentity& id);
static void test_rate_limit_standalone(const std::string& host, int port,
                                       const identity::RelayIdentity& id,
                                       pid_t relay_pid,
                                       const std::string& config_path);
static void test_sighup_rate_limit(const std::string& host, int port,
                                    const identity::RelayIdentity& id,
                                    pid_t relay_pid,
                                    const std::string& config_path);
static void test_sigterm(const std::string& host, int port,
                         const identity::RelayIdentity& id,
                         pid_t relay_pid);

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --identity <key_path>"
              << " --relay-pid <PID>"
              << " --config <relay_config_path>"
              << " [--host <addr>] [--port <port>]\n"
              << "\n"
              << "  --identity   Path to ML-DSA-87 client key file (required)\n"
              << "  --relay-pid  Relay process PID for signal delivery (required)\n"
              << "  --config     Path to relay JSON config file (required)\n"
              << "  --host       Relay host address (default: 127.0.0.1)\n"
              << "  --port       Relay port (default: 4201)\n";
}

// ---------------------------------------------------------------------------
// E2E-02: Pub/Sub notification fan-out (D-03, D-04)
//
// Two TCP connections in the same process, sequential blocking, no threads.
// Client A subscribes, Client B writes, Client A receives notification.
// ---------------------------------------------------------------------------

static void test_pubsub(const std::string& host, int port,
                        const identity::RelayIdentity& id) {
    // 1. Client A: connect and authenticate
    int fd_a = connect_and_auth(host, port, id);
    if (fd_a < 0) {
        record("pubsub_connect_a", false, "connect_and_auth failed");
        return;
    }
    record("pubsub_connect_a", true);

    // 2. Client A subscribes to own namespace
    auto ns_hex = util::to_hex(id.public_key_hash());
    json subscribe_msg = {
        {"type", "subscribe"},
        {"request_id", 1},
        {"namespaces", json::array({ns_hex})}
    };
    ws_send_text(fd_a, subscribe_msg.dump());
    // No response expected -- relay intercepts subscribe
    record("pubsub_subscribe", true);

    // 3. Client B: connect and authenticate
    int fd_b = connect_and_auth(host, port, id);
    if (fd_b < 0) {
        record("pubsub_connect_b", false, "connect_and_auth failed");
        ::close(fd_a);
        return;
    }
    record("pubsub_connect_b", true);

    // 4. Client B writes a blob
    std::vector<uint8_t> test_data{'p','u','b','s','u','b','_','t','e','s','t'};
    auto data_msg = make_data_message(id, 500, test_data, 3600,
                                      static_cast<uint64_t>(time(nullptr)));
    ws_send_text(fd_b, data_msg.dump());

    // 5. Client B reads write_ack
    auto ack_text = ws_recv_text(fd_b);
    bool ack_ok = false;
    std::string ack_hash;
    if (ack_text) {
        try {
            auto ack_json = json::parse(*ack_text);
            if (ack_json.contains("type") && ack_json["type"] == "write_ack" &&
                ack_json.contains("hash")) {
                ack_ok = true;
                ack_hash = ack_json["hash"].get<std::string>();
            }
        } catch (...) {}
    }
    record("pubsub_write_ack", ack_ok,
           ack_ok ? "hash=" + ack_hash : "no write_ack received");

    // 6. Read notification from Client A (D-04: guaranteed after write_ack)
    auto notif_frame = ws_recv_frame(fd_a);
    bool notif_ok = false;
    std::string notif_detail;
    if (notif_frame) {
        try {
            auto notif_json = json::parse(notif_frame->payload);
            std::string ntype = notif_json.value("type", "");
            std::string nns = notif_json.value("namespace", "");
            std::string nhash = notif_json.value("hash", "");
            bool is_tombstone = notif_json.value("is_tombstone", true);

            bool type_ok = (ntype == "notification");
            bool ns_ok = (nns == ns_hex);
            bool hash_ok = (nhash.size() == 64);
            bool tomb_ok = !is_tombstone;

            notif_ok = type_ok && ns_ok && hash_ok && tomb_ok;
            notif_detail = "type=" + ntype +
                          " ns_match=" + (ns_ok ? "true" : "false") +
                          " hash=" + nhash.substr(0, 16) + "..." +
                          " tombstone=" + (is_tombstone ? "true" : "false");
        } catch (...) {
            notif_detail = "JSON parse error";
        }
    } else {
        notif_detail = "no frame received";
    }
    record("pubsub_notification", notif_ok, notif_detail);

    // 7. Unsubscribe Client A
    json unsub_msg = {
        {"type", "unsubscribe"},
        {"request_id", 2},
        {"namespaces", json::array({ns_hex})}
    };
    ws_send_text(fd_a, unsub_msg.dump());

    // 8. Cleanup
    ::close(fd_a);
    ::close(fd_b);
}

// ---------------------------------------------------------------------------
// E2E-03: Standalone rate limit disconnect (D-10)
//
// Sets rate=5 via SIGHUP, connects fresh, blasts messages to trigger
// disconnect with close code 4002.
// ---------------------------------------------------------------------------

static void test_rate_limit_standalone(const std::string& host, int port,
                                       const identity::RelayIdentity& id,
                                       pid_t relay_pid,
                                       const std::string& config_path) {
    // Config restoration guard -- ensures rate=0 on ALL exit paths
    auto restore_config = [&]() {
        rewrite_config(config_path, "rate_limit_messages_per_sec", 0);
        kill(relay_pid, SIGHUP);
        usleep(200000);
    };

    // 1. Set rate limit to 5 via config + SIGHUP
    rewrite_config(config_path, "rate_limit_messages_per_sec", 5);
    kill(relay_pid, SIGHUP);
    usleep(200000);  // 200ms for SIGHUP processing

    // 2. Connect fresh (after SIGHUP applied)
    int fd = connect_and_auth(host, port, id);
    if (fd < 0) {
        record("rate_limit_connect", false, "connect_and_auth failed");
        restore_config();
        return;
    }
    record("rate_limit_connect", true);

    // 3. Blast messages in tight loop -- expect disconnect after token depletion
    bool disconnected = false;
    uint16_t disconnect_code = 0;
    int successful_sends = 0;
    for (int i = 0; i < 30; ++i) {
        json msg = {{"type", "node_info_request"}, {"request_id", 3000 + i}};
        if (!ws_send_text(fd, msg.dump())) {
            disconnected = true;
            break;
        }
        auto frame = ws_recv_frame_raw(fd);
        if (!frame) {
            disconnected = true;
            break;
        }
        if (frame->opcode == 0x08) {
            disconnected = true;
            disconnect_code = frame->close_code;
            break;
        }
        ++successful_sends;
    }
    record("rate_limit_disconnect", disconnected,
           "sent=" + std::to_string(successful_sends) +
           " code=" + std::to_string(disconnect_code));
    record("rate_limit_code_4002", disconnect_code == 4002,
           "expected 4002, got " + std::to_string(disconnect_code));

    // 4. Restore config and cleanup
    ::close(fd);
    restore_config();
}

// ---------------------------------------------------------------------------
// E2E-03 + E2E-04: SIGHUP config reload with rate limit verification (D-06, D-08, D-09)
//
// Phase 1: blast with rate=0 (all succeed)
// Phase 2: modify config + SIGHUP
// Phase 3: blast again (disconnect with 4002)
// ---------------------------------------------------------------------------

static void test_sighup_rate_limit(const std::string& host, int port,
                                    const identity::RelayIdentity& id,
                                    pid_t relay_pid,
                                    const std::string& config_path) {
    // 1. Connect with rate=0 (disabled)
    int fd = connect_and_auth(host, port, id);
    if (fd < 0) {
        record("sighup_connect", false, "connect_and_auth failed");
        return;
    }

    // Phase 1: blast 10 messages with rate=0 -- all should succeed
    bool phase1_ok = true;
    for (int i = 0; i < 10; ++i) {
        json msg = {{"type", "node_info_request"}, {"request_id", 4000 + i}};
        ws_send_text(fd, msg.dump());
        auto resp = ws_recv_text(fd);
        if (!resp) {
            phase1_ok = false;
            break;
        }
    }
    record("sighup_phase1_no_limit", phase1_ok, "10 messages sent with rate=0");

    if (!phase1_ok) {
        ::close(fd);
        return;
    }

    // Config restoration guard
    auto restore_config = [&]() {
        rewrite_config(config_path, "rate_limit_messages_per_sec", 0);
        kill(relay_pid, SIGHUP);
        usleep(200000);
    };

    // Phase 2: modify config and send SIGHUP
    rewrite_config(config_path, "rate_limit_messages_per_sec", 5);
    kill(relay_pid, SIGHUP);
    usleep(200000);  // 200ms for SIGHUP processing

    // Phase 3: blast again -- expect disconnect with 4002
    // First message triggers set_rate(5) with full 5 tokens.
    // Messages 1-5 consume tokens (succeed).
    // Messages 6-15: 10 consecutive rejections -> disconnect with Close(4002).
    bool disconnected = false;
    uint16_t disconnect_code = 0;
    int sent_ok = 0;
    for (int i = 0; i < 30; ++i) {
        json msg = {{"type", "node_info_request"}, {"request_id", 5000 + i}};
        if (!ws_send_text(fd, msg.dump())) {
            disconnected = true;
            break;
        }
        auto frame = ws_recv_frame_raw(fd);
        if (!frame) {
            disconnected = true;
            break;
        }
        if (frame->opcode == 0x08) {
            disconnected = true;
            disconnect_code = frame->close_code;
            break;
        }
        ++sent_ok;
    }
    record("sighup_rate_limit_disconnect", disconnected,
           "sent_ok=" + std::to_string(sent_ok) +
           " code=" + std::to_string(disconnect_code));
    record("sighup_rate_limit_code_4002", disconnect_code == 4002,
           "expected 4002, got " + std::to_string(disconnect_code));

    // Restore config for SIGTERM test
    ::close(fd);
    restore_config();
}

// ---------------------------------------------------------------------------
// E2E-05: SIGTERM graceful shutdown (D-07, D-11, D-12)
//
// MUST be last test. Sends SIGTERM, verifies close frame (1001) and process exit.
// ---------------------------------------------------------------------------

static void test_sigterm(const std::string& host, int port,
                         const identity::RelayIdentity& id,
                         pid_t relay_pid) {
    // 1. Connect
    int fd = connect_and_auth(host, port, id);
    if (fd < 0) {
        record("sigterm_connect", false, "connect_and_auth failed");
        return;
    }

    // 2. Send SIGTERM
    kill(relay_pid, SIGTERM);

    // 3. Set longer read timeout (relay has 5s drain + 2s close handshake)
    struct timeval tv{};
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 4. Read close frame using ws_recv_frame_raw
    auto frame = ws_recv_frame_raw(fd);
    bool got_close = frame && frame->opcode == 0x08;
    uint16_t close_code = frame ? frame->close_code : 0;
    record("sigterm_close_frame", got_close,
           got_close ? "opcode=0x08" : "no close frame received");
    record("sigterm_close_code_1001", close_code == 1001,
           "expected 1001, got " + std::to_string(close_code));

    // 5. Verify relay process exits (D-12)
    bool exited = false;
    for (int i = 0; i < 20; ++i) {
        if (kill(relay_pid, 0) != 0 && errno == ESRCH) {
            exited = true;
            break;
        }
        usleep(500000);  // 500ms, up to 10s total
    }
    record("sigterm_process_exit", exited,
           exited ? "relay process exited" : "relay still running after 10s");

    // 6. Cleanup
    ::close(fd);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string identity_path;
    std::string config_path;
    std::string host = "127.0.0.1";
    int port = 4201;
    pid_t relay_pid = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--identity" || arg == "-i") && i + 1 < argc) {
            identity_path = argv[++i];
        } else if (arg == "--relay-pid" && i + 1 < argc) {
            relay_pid = static_cast<pid_t>(std::stoi(argv[++i]));
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "--host" || arg == "-h") && i + 1 < argc) {
            host = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    // Validate required arguments
    if (identity_path.empty()) {
        std::cerr << "ERROR: --identity is required\n";
        usage(argv[0]);
        return 1;
    }
    if (config_path.empty()) {
        std::cerr << "ERROR: --config is required\n";
        usage(argv[0]);
        return 1;
    }
    if (relay_pid <= 0) {
        std::cerr << "ERROR: --relay-pid is required (must be > 0)\n";
        usage(argv[0]);
        return 1;
    }

    // Validate relay PID is actually running (Pitfall 7)
    if (kill(relay_pid, 0) != 0) {
        std::cerr << "ERROR: relay PID " << relay_pid << " is not running"
                  << " (errno=" << errno << ": " << std::strerror(errno) << ")\n";
        return 1;
    }

    // Load identity
    auto id = identity::RelayIdentity::load_from(identity_path);
    auto ns_hex = util::to_hex(id.public_key_hash());
    std::cout << "Client identity: " << ns_hex << "\n";
    std::cout << "Relay PID: " << relay_pid << "\n";
    std::cout << "Config: " << config_path << "\n";

    // Verify connectivity with connect_and_auth before running tests
    {
        int probe_fd = connect_and_auth(host, port, id);
        if (probe_fd < 0) {
            std::cerr << "ERROR: cannot connect+auth to relay at "
                      << host << ":" << port << "\n";
            return 1;
        }
        ::close(probe_fd);
        std::cout << "Connectivity check: OK\n";
    }

    // =====================================================================
    // Run tests -- SIGTERM MUST be last since it kills the relay (D-07, D-11)
    // =====================================================================

    std::cout << "\n=== Pub/Sub (E2E-02) ===\n";
    test_pubsub(host, port, id);

    std::cout << "\n=== Rate Limit Standalone (E2E-03) ===\n";
    test_rate_limit_standalone(host, port, id, relay_pid, config_path);

    std::cout << "\n=== SIGHUP + Rate Limit (E2E-03 + E2E-04) ===\n";
    test_sighup_rate_limit(host, port, id, relay_pid, config_path);

    // SIGTERM MUST be last -- it kills the relay process
    std::cout << "\n=== SIGTERM Graceful Shutdown (E2E-05) ===\n";
    test_sigterm(host, port, id, relay_pid);

    // =====================================================================
    // Summary
    // =====================================================================
    int passed = 0, failed = 0;
    for (const auto& r : results) {
        if (r.passed) ++passed;
        else ++failed;
    }
    std::cout << "\n=== Feature Test Results ===\n";
    std::cout << "  " << passed << " passed, " << failed << " failed, "
              << results.size() << " total\n";

    if (failed > 0) {
        std::cout << "\n  Failed tests:\n";
        for (const auto& r : results) {
            if (!r.passed) {
                std::cout << "    - " << r.name;
                if (!r.detail.empty()) std::cout << ": " << r.detail;
                std::cout << "\n";
            }
        }
    }

    return failed > 0 ? 1 : 0;
}
