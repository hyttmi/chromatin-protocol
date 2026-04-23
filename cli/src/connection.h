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

    /// Arrival-order counterpart of recv_for(rid). Returns the next reply —
    /// either a stashed one from pending_replies_ or the next one off the
    /// wire — and decrements in_flight_ by one. Used by chunked upload/download
    /// drain loops and by cmd::put / cmd::get batch drains where the caller
    /// routes replies by rid themselves (not by the Connection's correlation map).
    /// Single-sender / single-reader invariant (PIPE-02) is preserved: this
    /// method only invokes recv() under the hood.
    std::optional<DecodedTransport> recv_next();

    /// Close the connection.
    void close();

    bool is_connected() const { return connected_; }
    bool is_uds() const { return is_uds_; }

    /// Server's advertised max_blob_data_bytes, snapshotted on connect
    /// (Phase 130 CLI-01 / CONTEXT.md D-01). Returns 0 iff never connected
    /// successfully. For a live session the value is the node's Phase 128
    /// runtime cap at connect time; it does not change until reconnect.
    /// Consumers size chunked uploads, derive the manifest-validator ceiling,
    /// and diagnose cap violations against this cached value.
    uint64_t session_blob_cap() const { return session_blob_cap_; }

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

    // Phase 130 CLI-01 / CONTEXT.md D-01: session-scoped snapshot of the
    // server's advertised max_blob_data_bytes, seeded once by seed_session_cap()
    // inside connect() right after the handshake. Remains 0 until that call
    // succeeds; a 0 here means "do not chunk yet" — every cap-aware code path
    // MUST go through a live, seeded connection.
    uint64_t session_blob_cap_ = 0;

    // Internal: send NodeInfoRequest, decode max_blob_data_bytes, cache it.
    // Called from connect() as the first protocol round-trip after handshake.
    // Returns false on transport failure, malformed response, or a short
    // payload (pre-v4.2.0 node — Phase 130 CONTEXT.md D-07 hard-fail).
    bool seed_session_cap();
};

} // namespace chromatindb::cli
