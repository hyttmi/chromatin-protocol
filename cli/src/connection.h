#pragma once

#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <asio.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace chromatindb::cli {

/// Synchronous (blocking) connection to a chromatindb node.
/// Tries UDS first, falls back to TCP with full PQ handshake.
class Connection {
public:
    /// Maximum in-flight requests for send_async (PIPE-03 / D-07).
    /// Exposed publicly so pipelined callers in commands.cpp can size their
    /// per-batch correlation maps without duplicating the constant.
    static constexpr size_t kPipelineDepth = 8;

    explicit Connection(Identity& identity);

    /// Connect to node. Tries UDS path first, falls back to TCP.
    bool connect(const std::string& host, uint16_t port,
                 const std::string& uds_path);

    /// Send a transport message (AEAD-encrypted).
    /// Automatically uses chunked framing for payloads >= 1 MiB.
    bool send(MsgType type, std::span<const uint8_t> payload,
              uint32_t request_id = 1);

    /// Receive one transport message (AEAD-decrypt + decode).
    std::optional<DecodedTransport> recv();

    /// Pipelined send. Equivalent to send(), except: if PIPELINE_DEPTH requests
    /// are already in flight, blocks by pumping recv() until at least one slot
    /// frees. Off-target replies drained during backpressure are stashed for
    /// the next recv_for() call. Returns false on transport error.
    bool send_async(MsgType type, std::span<const uint8_t> payload,
                    uint32_t request_id);

    /// Wait for the reply matching `request_id`. Returns immediately if it was
    /// already stashed by a prior recv_for() / send_async() pump cycle, otherwise
    /// loops over recv() and stashes off-target replies until the target arrives.
    /// Returns std::nullopt on transport error (matches recv()).
    ///
    /// Precondition: each reply produced by recv() has already been
    /// AEAD-decrypted and integrity-verified in recv_encrypted(); this pump
    /// only routes already-authenticated replies (T-120-01).
    std::optional<DecodedTransport> recv_for(uint32_t request_id);

    /// Close the connection.
    void close();

    bool is_connected() const { return connected_; }
    bool is_uds() const { return is_uds_; }

private:
    // Connection establishment
    bool connect_uds(const std::string& uds_path);
    bool connect_tcp(const std::string& host, uint16_t port);

    // Handshake protocols
    bool handshake_pq();
    bool handshake_trusted();

    // Post-handshake: drain the SyncNamespaceAnnounce the node sends
    bool drain_announce();

    // Raw (unencrypted) framed I/O: [4BE length][data]
    bool send_raw(std::span<const uint8_t> data);
    std::optional<std::vector<uint8_t>> recv_raw();

    // Encrypted framed I/O (AEAD + counter nonce)
    bool send_encrypted(std::span<const uint8_t> plaintext);
    std::optional<std::vector<uint8_t>> recv_encrypted();

    // Chunked send for large payloads (>= STREAMING_THRESHOLD)
    bool send_chunked(MsgType type, std::span<const uint8_t> payload,
                      uint32_t request_id);

    // Write/read helpers that dispatch to the active socket
    bool write_bytes(const uint8_t* data, size_t len);
    bool read_bytes(uint8_t* buf, size_t len);

    Identity& identity_;
    asio::io_context ioc_;
    std::optional<asio::ip::tcp::socket> tcp_socket_;
    std::optional<asio::local::stream_protocol::socket> uds_socket_;

    std::vector<uint8_t> send_key_;   // 32 bytes
    std::vector<uint8_t> recv_key_;   // 32 bytes
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;
    bool connected_ = false;
    bool is_uds_ = false;

    // Pipelining: replies that arrived for a different rid than the caller is
    // currently waiting on. Bounded by PIPELINE_DEPTH; no eviction needed (D-04).
    std::unordered_map<uint32_t, DecodedTransport> pending_replies_;
    size_t in_flight_ = 0;
};

} // namespace chromatindb::cli
