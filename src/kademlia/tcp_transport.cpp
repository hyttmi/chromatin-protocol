#include "kademlia/tcp_transport.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "crypto/crypto.h"

namespace chromatin::kademlia {

// ---------------------------------------------------------------------------
// Wire format helpers
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_signed_data(const Message& msg) {
    // signed_data = magic || version || type || sender_port || sender_id || payload_length || payload
    size_t total = 4 + 1 + 1 + 2 + 32 + 4 + msg.payload.size();
    std::vector<uint8_t> buf;
    buf.reserve(total);

    // Magic (4 bytes)
    buf.insert(buf.end(), std::begin(CHRM_MAGIC), std::end(CHRM_MAGIC));

    // Version (1 byte)
    buf.push_back(PROTOCOL_VERSION);

    // Type (1 byte)
    buf.push_back(static_cast<uint8_t>(msg.type));

    // Sender port (2 bytes big-endian)
    buf.push_back(static_cast<uint8_t>((msg.sender_port >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(msg.sender_port & 0xFF));

    // Sender node ID (32 bytes)
    buf.insert(buf.end(), msg.sender.id.begin(), msg.sender.id.end());

    // Payload length (4 bytes big-endian)
    uint32_t payload_len = static_cast<uint32_t>(msg.payload.size());
    buf.push_back(static_cast<uint8_t>((payload_len >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((payload_len >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(payload_len & 0xFF));

    // Payload
    buf.insert(buf.end(), msg.payload.begin(), msg.payload.end());

    return buf;
}

std::vector<uint8_t> serialize_message(const Message& msg) {
    // Full wire format: signed_data + signature_length(2 BE) + signature
    auto buf = build_signed_data(msg);

    // Signature length (2 bytes big-endian)
    uint16_t sig_len = static_cast<uint16_t>(msg.signature.size());
    buf.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(sig_len & 0xFF));

    // Signature
    buf.insert(buf.end(), msg.signature.begin(), msg.signature.end());

    return buf;
}

std::optional<Message> deserialize_message(std::span<const uint8_t> data) {
    // Minimum size: header(44) + sig_length(2) = 46 bytes
    if (data.size() < HEADER_SIZE + 2) {
        return std::nullopt;
    }

    size_t offset = 0;

    // Verify magic
    if (std::memcmp(data.data() + offset, CHRM_MAGIC, 4) != 0) {
        return std::nullopt;
    }
    offset += 4;

    // Verify version
    if (data[offset] != PROTOCOL_VERSION) {
        return std::nullopt;
    }
    offset += 1;

    // Type
    Message msg;
    msg.type = static_cast<MessageType>(data[offset]);
    offset += 1;

    // Sender port (2 bytes big-endian)
    msg.sender_port = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    offset += 2;

    // Sender node ID (32 bytes)
    std::copy_n(data.data() + offset, 32, msg.sender.id.begin());
    offset += 32;

    // Payload length (4 bytes big-endian)
    uint32_t payload_len = (static_cast<uint32_t>(data[offset]) << 24)
                         | (static_cast<uint32_t>(data[offset + 1]) << 16)
                         | (static_cast<uint32_t>(data[offset + 2]) << 8)
                         | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    // Check that we have enough data for payload + signature length
    if (data.size() < offset + payload_len + 2) {
        return std::nullopt;
    }

    // Payload
    msg.payload.assign(data.data() + offset, data.data() + offset + payload_len);
    offset += payload_len;

    // Signature length (2 bytes big-endian)
    uint16_t sig_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    offset += 2;

    // Check that we have enough data for signature
    if (data.size() < offset + sig_len) {
        return std::nullopt;
    }

    // Signature
    msg.signature.assign(data.data() + offset, data.data() + offset + sig_len);

    return msg;
}

// ---------------------------------------------------------------------------
// Signing / verification
// ---------------------------------------------------------------------------

void sign_message(Message& msg, std::span<const uint8_t> secret_key) {
    auto signed_data = build_signed_data(msg);
    msg.signature = crypto::sign(signed_data, secret_key);
}

bool verify_message(const Message& msg, std::span<const uint8_t> public_key) {
    auto signed_data = build_signed_data(msg);
    return crypto::verify(signed_data, msg.signature, public_key);
}

// ---------------------------------------------------------------------------
// TCP helpers
// ---------------------------------------------------------------------------

// Read exactly n bytes from a socket. Returns false on error/disconnect.
static bool read_exact(int fd, uint8_t* buf, size_t n) {
    size_t total_read = 0;
    while (total_read < n) {
        ssize_t r = recv(fd, buf + total_read, n - total_read, 0);
        if (r <= 0) return false;
        total_read += static_cast<size_t>(r);
    }
    return true;
}

// Read one complete framed CHRM message from a TCP connection.
static std::optional<Message> read_framed_message(int fd) {
    // 1. Read header (44 bytes)
    std::vector<uint8_t> header(HEADER_SIZE);
    if (!read_exact(fd, header.data(), HEADER_SIZE)) return std::nullopt;

    // 2. Extract payload_length from header
    //    Offset: magic(4) + version(1) + type(1) + sender_port(2) + sender_id(32) = 40
    size_t pl_offset = 40;
    uint32_t payload_len = (static_cast<uint32_t>(header[pl_offset]) << 24)
                         | (static_cast<uint32_t>(header[pl_offset + 1]) << 16)
                         | (static_cast<uint32_t>(header[pl_offset + 2]) << 8)
                         | static_cast<uint32_t>(header[pl_offset + 3]);

    // Sanity check: reject absurdly large payloads
    // Note: uses a generous upper bound; real enforcement is at the application layer
    if (payload_len > 100u * 1024u * 1024u) return std::nullopt;

    // 3. Read payload
    std::vector<uint8_t> payload(payload_len);
    if (payload_len > 0 && !read_exact(fd, payload.data(), payload_len)) return std::nullopt;

    // 4. Read sig_length (2 bytes)
    uint8_t sig_len_buf[2];
    if (!read_exact(fd, sig_len_buf, 2)) return std::nullopt;
    uint16_t sig_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(sig_len_buf[0]) << 8) | sig_len_buf[1]);

    // 5. Read signature
    std::vector<uint8_t> sig(sig_len);
    if (sig_len > 0 && !read_exact(fd, sig.data(), sig_len)) return std::nullopt;

    // 6. Assemble full buffer and deserialize
    std::vector<uint8_t> full;
    full.reserve(HEADER_SIZE + payload_len + 2 + sig_len);
    full.insert(full.end(), header.begin(), header.end());
    full.insert(full.end(), payload.begin(), payload.end());
    full.push_back(sig_len_buf[0]);
    full.push_back(sig_len_buf[1]);
    full.insert(full.end(), sig.begin(), sig.end());

    return deserialize_message(full);
}

// ---------------------------------------------------------------------------
// TcpTransport
// ---------------------------------------------------------------------------

TcpTransport::TcpTransport(const std::string& bind_addr, uint16_t port,
                           uint16_t connect_timeout, uint16_t read_timeout,
                           size_t max_pool_size, uint16_t conn_idle_seconds,
                           uint64_t max_message_size, size_t max_tcp_clients)
    : port_(port)
    , connect_timeout_(connect_timeout)
    , read_timeout_(read_timeout)
    , max_pool_size_(max_pool_size)
    , conn_max_idle_(conn_idle_seconds)
    , max_message_size_(max_message_size)
    , max_tcp_clients_(max_tcp_clients) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));
    }

    // Allow address reuse
    int optval = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind_addr.empty() || bind_addr == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
            close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error("Invalid bind address: " + bind_addr);
        }
    }

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("bind() failed: " + std::string(strerror(errno)));
    }

    // Listen with a reasonable backlog
    if (listen(listen_fd_, 64) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("listen() failed: " + std::string(strerror(errno)));
    }

    // Read back assigned port (needed when port == 0)
    struct sockaddr_in bound_addr{};
    socklen_t bound_len = sizeof(bound_addr);
    if (getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&bound_addr), &bound_len) == 0) {
        port_ = ntohs(bound_addr.sin_port);
    }

    spdlog::info("TCP transport listening on {}:{}", bind_addr.empty() ? "0.0.0.0" : bind_addr, port_);
}

TcpTransport::~TcpTransport() {
    stop();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    // Close all pooled connections
    std::lock_guard lock(pool_mutex_);
    for (auto& [key, conn] : conn_pool_) {
        close(conn.fd);
    }
    conn_pool_.clear();
}

void TcpTransport::cleanup_idle_connections() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(pool_mutex_);
    for (auto it = conn_pool_.begin(); it != conn_pool_.end(); ) {
        if (now - it->second.last_used > conn_max_idle_) {
            close(it->second.fd);
            it = conn_pool_.erase(it);
        } else {
            ++it;
        }
    }
}

// Check if a socket is still connected (no pending RST/FIN).
// Uses non-blocking recv with MSG_PEEK to detect if the remote end closed.
static bool is_connection_alive(int fd) {
    // First check for POLLERR/POLLHUP via poll
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 0);
    if (ret < 0) return false;
    if (ret == 0) return true;  // no events pending, connection is idle

    // poll says there's something to read — peek to see if it's EOF
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    if (pfd.revents & POLLIN) {
        // There's data or EOF. Peek to distinguish.
        char buf;
        ssize_t r = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) return false;   // EOF — remote closed
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        // r > 0 means unexpected data on the connection (shouldn't happen
        // for our protocol where the server never sends on the same connection)
        // or r < 0 with EAGAIN means no data available (spurious wakeup).
        // In either case, treat as alive.
    }
    return true;
}

// Helper: create a new TCP connection to addr:port. Returns fd or -1 on failure.
static int create_connection(const std::string& addr, uint16_t port, uint16_t connect_timeout = 5) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        spdlog::error("send socket() failed: {}", strerror(errno));
        return -1;
    }

    // Disable Nagle's algorithm for low latency
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Connect timeout via SO_SNDTIMEO
    struct timeval tv{};
    tv.tv_sec = connect_timeout;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, addr.c_str(), &dest.sin_addr) != 1) {
        close(sockfd);
        spdlog::error("Invalid destination address: {}", addr);
        return -1;
    }

    if (connect(sockfd, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        close(sockfd);
        spdlog::warn("TCP connect to {}:{} failed: {}", addr, port, strerror(errno));
        return -1;
    }

    return sockfd;
}

// Helper: try to send all data on fd. Returns true on success.
static bool send_all(int fd, const std::vector<uint8_t>& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = ::send(fd, data.data() + total_sent, data.size() - total_sent, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

void TcpTransport::send(const std::string& addr, uint16_t port, const Message& msg) {
    auto data = serialize_message(msg);
    // Use [addr]:port format for unambiguous IPv6 support
    auto pool_key = "[" + addr + "]:" + std::to_string(port);
    int fd = -1;
    bool from_pool = false;

    // Try to get a pooled connection
    {
        std::lock_guard lock(pool_mutex_);
        auto it = conn_pool_.find(pool_key);
        if (it != conn_pool_.end()) {
            fd = it->second.fd;
            conn_pool_.erase(it);  // take ownership
            from_pool = true;
        }
    }

    // Verify pooled connection is still alive (remote may have closed it)
    if (from_pool && !is_connection_alive(fd)) {
        spdlog::debug("Pooled connection to {} is dead (fd={}), creating new", pool_key, fd);
        close(fd);
        fd = -1;
        from_pool = false;
    }

    if (from_pool) {
        spdlog::debug("Reusing pooled connection to {} (fd={})", pool_key, fd);
    }

    // If no pooled connection, create a new one
    if (fd < 0) {
        fd = create_connection(addr, port, connect_timeout_);
        if (fd < 0) return;
        spdlog::debug("Created new connection to {} (fd={})", pool_key, fd);
    }

    // Try to send
    bool send_ok = send_all(fd, data);

    // If send failed on a pooled connection, the remote side may have closed it.
    // Retry with a fresh connection.
    if (!send_ok && from_pool) {
        spdlog::debug("Send on pooled fd={} failed, retrying with new connection", fd);
        close(fd);
        fd = create_connection(addr, port, connect_timeout_);
        if (fd < 0) return;
        send_ok = send_all(fd, data);
    }

    if (send_ok) {
        // Return connection to pool
        std::lock_guard lock(pool_mutex_);
        if (conn_pool_.size() < max_pool_size_) {
            conn_pool_[pool_key] = {fd, std::chrono::steady_clock::now()};
            spdlog::debug("Returned fd={} to pool for {}", fd, pool_key);
        } else {
            close(fd);
        }
    } else {
        close(fd);
        spdlog::error("TCP send to {}:{} failed: {}", addr, port, strerror(errno));
    }
}

void TcpTransport::run(Handler handler) {
    running_.store(true);

    // Track accepted client connections: pollfd + peer address.
    // Index 0 is always the listen socket.
    std::vector<struct pollfd> fds;
    std::vector<struct sockaddr_in> client_addrs;  // parallel to fds (index 0 unused)

    // Listen socket at index 0
    fds.push_back({listen_fd_, POLLIN, 0});
    client_addrs.push_back({});  // placeholder for listen socket

    while (running_.load()) {
        int ret = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            spdlog::error("poll() failed: {}", strerror(errno));
            break;
        }
        if (ret == 0) continue;  // timeout

        // Accept new incoming connections
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            if (client_fd >= 0) {
                // fds.size() - 1 because index 0 is the listen socket
                if (fds.size() - 1 < max_tcp_clients_) {
                    // Set read timeout on accepted connection
                    struct timeval read_tv{};
                    read_tv.tv_sec = read_timeout_;
                    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &read_tv, sizeof(read_tv));

                    fds.push_back({client_fd, POLLIN, 0});
                    client_addrs.push_back(client_addr);
                } else {
                    close(client_fd);  // at capacity
                    spdlog::warn("TCP client limit reached ({}), rejecting connection", max_tcp_clients_);
                }
            } else if (errno != EINTR) {
                spdlog::warn("accept() failed: {}", strerror(errno));
            }
        }

        // Read from ready client connections (skip index 0 = listen socket)
        for (size_t i = 1; i < fds.size(); ) {
            if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                ++i;
                continue;
            }

            auto msg = read_framed_message(fds[i].fd);
            if (!msg) {
                // Connection closed or error — remove this client
                close(fds[i].fd);
                fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }

            // from_addr = peer's IP, from_port = sender's listening port (from header)
            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addrs[i].sin_addr, addr_str, sizeof(addr_str));

            handler(*msg, std::string(addr_str), msg->sender_port);
            ++i;
        }
    }

    // Clean up any remaining client connections on shutdown
    for (size_t i = 1; i < fds.size(); ++i) {
        close(fds[i].fd);
    }
}

void TcpTransport::stop() {
    running_.store(false);
}

uint16_t TcpTransport::local_port() const {
    return port_;
}

} // namespace chromatin::kademlia
