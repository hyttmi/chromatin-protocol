// relay_uds_tap -- Standalone UDS tap tool for capturing binary responses
// from a live chromatindb node. Connects via UDS, performs TrustedHello
// handshake + AEAD auth, sends each compound request type, and saves raw
// binary response payloads to files.
//
// Usage:
//   relay_uds_tap --socket /path/to/node.sock --output /path/to/fixtures/
//                 [--identity /path/to/relay.key]
//
// If --identity is omitted, generates an ephemeral identity (requires the
// node to accept untrusted/any peer for TrustedHello).
//
// Reusable for Phase 107 E2E verification.

#include "relay/identity/relay_identity.h"
#include "relay/util/endian.h"
#include "relay/util/hex.h"
#include "relay/wire/aead.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"

#include <oqs/sha3.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace wire = chromatindb::relay::wire;
namespace util = chromatindb::relay::util;
namespace identity = chromatindb::relay::identity;

using chromatindb::wire::TransportMsgType;
using chromatindb::wire::TransportMsgType_TrustedHello;
using chromatindb::wire::TransportMsgType_AuthSignature;

// ---------------------------------------------------------------------------
// Raw socket I/O helpers (blocking, no Asio needed for a simple tool)
// ---------------------------------------------------------------------------

static bool send_all(int fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, data + sent, len - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static bool recv_all(int fd, uint8_t* buf, size_t len, int timeout_ms = 5000) {
    size_t got = 0;
    while (got < len) {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, timeout_ms);
        if (r <= 0) return false;  // timeout or error
        ssize_t n = ::read(fd, buf + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

// Send length-prefixed frame: [4B BE length][data]
static bool send_raw(int fd, std::span<const uint8_t> data) {
    uint8_t header[4];
    util::store_u32_be(header, static_cast<uint32_t>(data.size()));
    if (!send_all(fd, header, 4)) return false;
    return send_all(fd, data.data(), data.size());
}

// Receive length-prefixed frame: [4B BE length][data]
static std::optional<std::vector<uint8_t>> recv_raw(int fd) {
    uint8_t header[4];
    if (!recv_all(fd, header, 4)) return std::nullopt;
    uint32_t length = util::read_u32_be(header);
    if (length > 110 * 1024 * 1024) return std::nullopt;  // MAX_FRAME_SIZE
    std::vector<uint8_t> buf(length);
    if (!recv_all(fd, buf.data(), length)) return std::nullopt;
    return buf;
}

// ---------------------------------------------------------------------------
// AEAD send/recv with counter tracking
// ---------------------------------------------------------------------------

struct AeadState {
    std::vector<uint8_t> send_key;
    std::vector<uint8_t> recv_key;
    uint64_t send_counter = 0;
    uint64_t recv_counter = 0;
};

static bool send_encrypted(int fd, AeadState& state, std::span<const uint8_t> plaintext) {
    auto ct = wire::aead_encrypt(plaintext, state.send_key, state.send_counter++);
    return send_raw(fd, ct);
}

static std::optional<std::vector<uint8_t>> recv_encrypted(int fd, AeadState& state) {
    auto raw = recv_raw(fd);
    if (!raw) return std::nullopt;
    auto pt = wire::aead_decrypt(*raw, state.recv_key, state.recv_counter++);
    return pt;
}

// ---------------------------------------------------------------------------
// TrustedHello + Auth handshake (mirrors UdsMultiplexer::do_handshake)
// ---------------------------------------------------------------------------

static constexpr size_t SIGNING_PK_SIZE = 2592;

static bool do_handshake(int fd, const identity::RelayIdentity& id, AeadState& state) {
    // Step 1: Generate 32-byte nonce
    std::array<uint8_t, 32> nonce_i{};
    if (RAND_bytes(nonce_i.data(), 32) != 1) {
        std::cerr << "ERROR: RAND_bytes failed\n";
        return false;
    }

    // Step 2: Build TrustedHello payload: [nonce:32][signing_pubkey:2592]
    auto signing_pk = id.public_key();
    std::vector<uint8_t> hello_payload;
    hello_payload.reserve(32 + signing_pk.size());
    hello_payload.insert(hello_payload.end(), nonce_i.begin(), nonce_i.end());
    hello_payload.insert(hello_payload.end(), signing_pk.begin(), signing_pk.end());

    // Step 3: Encode with TransportCodec
    auto hello_msg = wire::TransportCodec::encode(TransportMsgType_TrustedHello, hello_payload);

    // Step 4: Send TrustedHello
    if (!send_raw(fd, hello_msg)) {
        std::cerr << "ERROR: failed to send TrustedHello\n";
        return false;
    }

    // Step 5: Receive TrustedHello response
    auto response = recv_raw(fd);
    if (!response) {
        std::cerr << "ERROR: failed to receive TrustedHello response\n";
        return false;
    }

    // Step 6: Decode response
    auto decoded = wire::TransportCodec::decode(*response);
    if (!decoded || decoded->type != TransportMsgType_TrustedHello) {
        std::cerr << "ERROR: invalid TrustedHello response\n";
        return false;
    }

    // Step 7: Parse response nonce + signing_pk
    constexpr size_t NONCE_SIZE = 32;
    size_t expected_size = NONCE_SIZE + SIGNING_PK_SIZE;
    if (decoded->payload.size() != expected_size) {
        std::cerr << "ERROR: TrustedHello payload size mismatch (got "
                  << decoded->payload.size() << ", expected " << expected_size << ")\n";
        return false;
    }

    auto nonce_r = std::span<const uint8_t>(decoded->payload.data(), NONCE_SIZE);
    auto resp_signing_pk = std::span<const uint8_t>(
        decoded->payload.data() + NONCE_SIZE, SIGNING_PK_SIZE);

    // Step 8: Derive session keys via HKDF-SHA256
    // IKM = nonce_i || nonce_r (64 bytes)
    std::vector<uint8_t> ikm;
    ikm.reserve(64);
    ikm.insert(ikm.end(), nonce_i.begin(), nonce_i.end());
    ikm.insert(ikm.end(), nonce_r.begin(), nonce_r.end());

    // Salt = signing_pk_i || signing_pk_r (we are initiator)
    std::vector<uint8_t> salt;
    salt.reserve(signing_pk.size() + SIGNING_PK_SIZE);
    salt.insert(salt.end(), signing_pk.begin(), signing_pk.end());
    salt.insert(salt.end(), resp_signing_pk.begin(), resp_signing_pk.end());

    auto prk = wire::hkdf_extract(salt, ikm);
    auto init_to_resp_key = wire::hkdf_expand(prk, "chromatin-init-to-resp-v1", 32);
    auto resp_to_init_key = wire::hkdf_expand(prk, "chromatin-resp-to-init-v1", 32);

    // Session fingerprint: SHA3-256(IKM || Salt)
    std::vector<uint8_t> fp_input;
    fp_input.reserve(ikm.size() + salt.size());
    fp_input.insert(fp_input.end(), ikm.begin(), ikm.end());
    fp_input.insert(fp_input.end(), salt.begin(), salt.end());

    std::array<uint8_t, 32> session_fingerprint{};
    OQS_SHA3_sha3_256(session_fingerprint.data(), fp_input.data(), fp_input.size());

    // Step 9: Set keys (we are initiator)
    state.send_key.assign(init_to_resp_key.begin(), init_to_resp_key.end());
    state.recv_key.assign(resp_to_init_key.begin(), resp_to_init_key.end());
    state.send_counter = 0;
    state.recv_counter = 0;

    // Step 10: Auth exchange (encrypted, consumes counter 0)
    auto sig = id.sign(session_fingerprint);

    // Build auth payload: [pubkey_size:4B LE][pubkey:2592][signature:4627]
    // CRITICAL: pubkey_size is LITTLE-endian (protocol exception)
    std::vector<uint8_t> auth_payload;
    auth_payload.reserve(4 + signing_pk.size() + sig.size());
    uint32_t pk_size = static_cast<uint32_t>(signing_pk.size());
    auth_payload.push_back(static_cast<uint8_t>(pk_size & 0xFF));
    auth_payload.push_back(static_cast<uint8_t>((pk_size >> 8) & 0xFF));
    auth_payload.push_back(static_cast<uint8_t>((pk_size >> 16) & 0xFF));
    auth_payload.push_back(static_cast<uint8_t>((pk_size >> 24) & 0xFF));
    auth_payload.insert(auth_payload.end(), signing_pk.begin(), signing_pk.end());
    auth_payload.insert(auth_payload.end(), sig.begin(), sig.end());

    // Encode and send encrypted auth (uses counter 0)
    auto auth_msg = wire::TransportCodec::encode(TransportMsgType_AuthSignature, auth_payload);
    if (!send_encrypted(fd, state, auth_msg)) {
        std::cerr << "ERROR: failed to send auth\n";
        return false;
    }

    // Receive peer's auth (uses counter 0)
    auto resp_auth_raw = recv_encrypted(fd, state);
    if (!resp_auth_raw) {
        std::cerr << "ERROR: failed to receive peer auth\n";
        return false;
    }

    auto resp_transport = wire::TransportCodec::decode(*resp_auth_raw);
    if (!resp_transport || resp_transport->type != TransportMsgType_AuthSignature) {
        std::cerr << "ERROR: invalid auth message from node\n";
        return false;
    }

    // Verify auth payload
    if (resp_transport->payload.size() < 4) {
        std::cerr << "ERROR: auth payload too short\n";
        return false;
    }
    uint32_t resp_pk_size =
        static_cast<uint32_t>(resp_transport->payload[0]) |
        (static_cast<uint32_t>(resp_transport->payload[1]) << 8) |
        (static_cast<uint32_t>(resp_transport->payload[2]) << 16) |
        (static_cast<uint32_t>(resp_transport->payload[3]) << 24);

    if (resp_pk_size != SIGNING_PK_SIZE) {
        std::cerr << "ERROR: unexpected pubkey size " << resp_pk_size << " in auth\n";
        return false;
    }

    std::cout << "Handshake complete (send_counter=" << state.send_counter
              << " recv_counter=" << state.recv_counter << ")\n";
    return true;
}

// ---------------------------------------------------------------------------
// Request type descriptors
// ---------------------------------------------------------------------------

struct RequestDesc {
    const char* name;
    uint8_t request_type;
    uint8_t response_type;
    std::vector<uint8_t> payload;
};

static std::vector<RequestDesc> build_requests() {
    // Zero namespace for queries that need one
    std::array<uint8_t, 32> zero_ns{};
    // Zero hash for queries that need one
    std::array<uint8_t, 32> zero_hash{};

    std::vector<RequestDesc> requests;

    // NodeInfoRequest(39) -> NodeInfoResponse(40): empty payload
    requests.push_back({"node_info_response", 39, 40, {}});

    // StatsRequest(35) -> StatsResponse(36): 32-byte namespace
    requests.push_back({"stats_response", 35, 36,
        std::vector<uint8_t>(zero_ns.begin(), zero_ns.end())});

    // StorageStatusRequest(43) -> StorageStatusResponse(44): empty payload
    requests.push_back({"storage_status_response", 43, 44, {}});

    // NamespaceStatsRequest(45) -> NamespaceStatsResponse(46): 32-byte namespace
    requests.push_back({"namespace_stats_response", 45, 46,
        std::vector<uint8_t>(zero_ns.begin(), zero_ns.end())});

    // ListRequest(33) -> ListResponse(34): 32-byte namespace + u64BE since_seq + u32BE limit
    {
        std::vector<uint8_t> list_payload;
        list_payload.insert(list_payload.end(), zero_ns.begin(), zero_ns.end());
        uint8_t since_buf[8];
        util::store_u64_be(since_buf, 0);  // since_seq = 0 (from start)
        list_payload.insert(list_payload.end(), since_buf, since_buf + 8);
        uint8_t limit_buf[4];
        util::store_u32_be(limit_buf, 10);  // limit = 10
        list_payload.insert(list_payload.end(), limit_buf, limit_buf + 4);
        requests.push_back({"list_response", 33, 34, std::move(list_payload)});
    }

    // NamespaceListRequest(41) -> NamespaceListResponse(42): 32-byte after_ns + u32BE limit
    {
        std::vector<uint8_t> nslist_payload;
        nslist_payload.insert(nslist_payload.end(), zero_ns.begin(), zero_ns.end());
        uint8_t limit_buf[4];
        util::store_u32_be(limit_buf, 100);  // limit = 100
        nslist_payload.insert(nslist_payload.end(), limit_buf, limit_buf + 4);
        requests.push_back({"namespace_list_response", 41, 42, std::move(nslist_payload)});
    }

    // MetadataRequest(47) -> MetadataResponse(48): 32+32 bytes (namespace + hash)
    {
        std::vector<uint8_t> meta_payload;
        meta_payload.insert(meta_payload.end(), zero_ns.begin(), zero_ns.end());
        meta_payload.insert(meta_payload.end(), zero_hash.begin(), zero_hash.end());
        requests.push_back({"metadata_response", 47, 48, std::move(meta_payload)});
    }

    // BatchExistsRequest(49) -> BatchExistsResponse(50): 32-byte ns + u32BE count(1) + hash
    {
        std::vector<uint8_t> batch_payload;
        batch_payload.insert(batch_payload.end(), zero_ns.begin(), zero_ns.end());
        uint8_t count_buf[4];
        util::store_u32_be(count_buf, 1);
        batch_payload.insert(batch_payload.end(), count_buf, count_buf + 4);
        batch_payload.insert(batch_payload.end(), zero_hash.begin(), zero_hash.end());
        requests.push_back({"batch_exists_response", 49, 50, std::move(batch_payload)});
    }

    // DelegationListRequest(51) -> DelegationListResponse(52): 32-byte namespace
    requests.push_back({"delegation_list_response", 51, 52,
        std::vector<uint8_t>(zero_ns.begin(), zero_ns.end())});

    // PeerInfoRequest(55) -> PeerInfoResponse(56): empty payload
    requests.push_back({"peer_info_response", 55, 56, {}});

    // TimeRangeRequest(57) -> TimeRangeResponse(58): 32-byte ns + u64BE start_ts + u64BE end_ts + u32BE limit
    {
        std::vector<uint8_t> tr_payload;
        tr_payload.insert(tr_payload.end(), zero_ns.begin(), zero_ns.end());
        uint8_t start_buf[8];
        util::store_u64_be(start_buf, 0);  // start_ts = 0 (all time)
        tr_payload.insert(tr_payload.end(), start_buf, start_buf + 8);
        uint8_t end_buf[8];
        util::store_u64_be(end_buf, UINT64_MAX);  // end_ts = max (all time)
        tr_payload.insert(tr_payload.end(), end_buf, end_buf + 8);
        uint8_t limit_buf[4];
        util::store_u32_be(limit_buf, 10);  // limit = 10
        tr_payload.insert(tr_payload.end(), limit_buf, limit_buf + 4);
        requests.push_back({"time_range_response", 57, 58, std::move(tr_payload)});
    }

    return requests;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --socket <path> --output <dir> [--identity <key_path>]\n"
              << "\n"
              << "  --socket    Path to chromatindb node UDS socket\n"
              << "  --output    Directory to save binary response fixtures\n"
              << "  --identity  Path to relay identity key file (generates ephemeral if omitted)\n";
}

int main(int argc, char* argv[]) {
    std::string socket_path;
    std::string output_dir;
    std::string identity_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--socket" || arg == "-s") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            output_dir = argv[++i];
        } else if ((arg == "--identity" || arg == "-i") && i + 1 < argc) {
            identity_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (socket_path.empty() || output_dir.empty()) {
        std::cerr << "ERROR: --socket and --output are required\n";
        usage(argv[0]);
        return 1;
    }

    // Create output directory if it doesn't exist
    std::filesystem::create_directories(output_dir);

    // Load or generate identity
    identity::RelayIdentity id = identity_path.empty()
        ? identity::RelayIdentity::generate()
        : identity::RelayIdentity::load_from(identity_path);

    std::cout << "Identity: " << util::to_hex(id.public_key_hash()) << "\n";

    // Connect to UDS socket
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "ERROR: socket() failed\n";
        return 1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "ERROR: socket path too long\n";
        ::close(fd);
        return 1;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "ERROR: connect() to '" << socket_path << "' failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }

    std::cout << "Connected to " << socket_path << "\n";

    // Perform TrustedHello + AEAD handshake
    AeadState aead;
    if (!do_handshake(fd, id, aead)) {
        std::cerr << "ERROR: handshake failed\n";
        ::close(fd);
        return 1;
    }

    // Drain unsolicited SyncNamespaceAnnounce (type 62) the node sends after handshake
    {
        auto unsolicited = recv_encrypted(fd, aead);
        if (unsolicited) {
            auto decoded = wire::TransportCodec::decode(*unsolicited);
            if (decoded) {
                std::cout << "Drained unsolicited message: type="
                          << static_cast<int>(decoded->type)
                          << " request_id=" << decoded->request_id
                          << " (" << decoded->payload.size() << " bytes)\n";
            }
        } else {
            std::cerr << "WARNING: no unsolicited message after handshake (expected SyncNamespaceAnnounce)\n";
        }
    }

    // Send each compound request type and capture responses
    auto requests = build_requests();
    int succeeded = 0;
    int failed = 0;

    for (const auto& req : requests) {
        std::cout << "\n--- " << req.name << " (type " << static_cast<int>(req.request_type)
                  << " -> " << static_cast<int>(req.response_type) << ") ---\n";

        // Use sequential request_ids starting at 1
        uint32_t rid = static_cast<uint32_t>(succeeded + failed + 1);

        // Encode transport message
        auto transport_msg = wire::TransportCodec::encode(
            static_cast<TransportMsgType>(req.request_type), req.payload, rid);

        // Send encrypted
        if (!send_encrypted(fd, aead, transport_msg)) {
            std::cerr << "  FAIL: send failed\n";
            ++failed;
            continue;
        }

        // Receive encrypted response
        auto response = recv_encrypted(fd, aead);
        if (!response) {
            std::cerr << "  FAIL: recv failed\n";
            ++failed;
            continue;
        }

        // Decode transport envelope
        auto decoded = wire::TransportCodec::decode(*response);
        if (!decoded) {
            std::cerr << "  FAIL: decode failed\n";
            ++failed;
            continue;
        }

        std::cout << "  Response type: " << static_cast<int>(decoded->type)
                  << " request_id: " << decoded->request_id
                  << " payload: " << decoded->payload.size() << " bytes\n";

        // Save raw payload to file
        std::string filename = output_dir + "/" + req.name + ".bin";
        std::ofstream out(filename, std::ios::binary);
        if (!out) {
            std::cerr << "  FAIL: cannot open " << filename << "\n";
            ++failed;
            continue;
        }
        out.write(reinterpret_cast<const char*>(decoded->payload.data()),
                  static_cast<std::streamsize>(decoded->payload.size()));
        out.close();

        std::cout << "  Saved: " << filename << " (" << decoded->payload.size() << " bytes)\n";
        ++succeeded;
    }

    ::close(fd);

    // Summary
    std::cout << "\n=== Summary ===\n"
              << "  Succeeded: " << succeeded << "/" << requests.size() << "\n"
              << "  Failed:    " << failed << "/" << requests.size() << "\n";

    if (failed > 0) {
        std::cout << "  Result:    PARTIAL (some captures failed)\n";
    } else {
        std::cout << "  Result:    ALL CAPTURED\n";
    }

    return failed > 0 ? 1 : 0;
}
