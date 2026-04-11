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

#include <csignal>    // kill()
#include <sys/wait.h> // waitpid (D-12)
#include <fstream>    // config rewrite

using namespace relay_test;

// ---------------------------------------------------------------------------
// Forward declarations -- Plan 02 implements the bodies
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
// Test implementations (skeletons -- Plan 02 fills these in)
// ---------------------------------------------------------------------------

static void test_pubsub(const std::string& host, int port,
                        const identity::RelayIdentity& id) {
    record("pubsub_notification", false, "NOT IMPLEMENTED");
}

static void test_rate_limit_standalone(const std::string& host, int port,
                                       const identity::RelayIdentity& id,
                                       pid_t relay_pid,
                                       const std::string& config_path) {
    record("rate_limit_standalone", false, "NOT IMPLEMENTED");
}

static void test_sighup_rate_limit(const std::string& host, int port,
                                    const identity::RelayIdentity& id,
                                    pid_t relay_pid,
                                    const std::string& config_path) {
    record("sighup_rate_limit", false, "NOT IMPLEMENTED");
}

static void test_sigterm(const std::string& host, int port,
                         const identity::RelayIdentity& id,
                         pid_t relay_pid) {
    record("sigterm_close_frame", false, "NOT IMPLEMENTED");
    record("sigterm_process_exit", false, "NOT IMPLEMENTED");
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
