#pragma once

#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <asio.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::cli {

/// Synchronous (blocking) connection to a chromatindb node.
/// Tries UDS first, falls back to TCP with full PQ handshake.
class Connection {
public:
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
};

} // namespace chromatindb::cli
