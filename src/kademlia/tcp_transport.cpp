#include "kademlia/tcp_transport.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "crypto/aead.h"
#include "crypto/crypto.h"
#include "crypto/kem.h"

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

// Read one complete framed CHRM message from a plaintext TCP connection.
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

// Read one encrypted frame from a connection.
static std::optional<Message> read_encrypted_message(int fd, tcp_encryption::SessionKeys& session) {
    // Read 4-byte length header
    uint8_t len_buf[4];
    if (!read_exact(fd, len_buf, 4)) return std::nullopt;

    uint32_t ct_len = (static_cast<uint32_t>(len_buf[0]) << 24) |
                      (static_cast<uint32_t>(len_buf[1]) << 16) |
                      (static_cast<uint32_t>(len_buf[2]) << 8) |
                      static_cast<uint32_t>(len_buf[3]);

    // Sanity check
    if (ct_len > 100u * 1024u * 1024u + crypto::AEAD_TAG_SIZE) return std::nullopt;

    // Read ciphertext
    std::vector<uint8_t> ciphertext(ct_len);
    if (ct_len > 0 && !read_exact(fd, ciphertext.data(), ct_len)) return std::nullopt;

    // Decrypt with AAD = length header
    std::span<const uint8_t> aad(len_buf, 4);
    auto plaintext = tcp_encryption::decrypt_frame(session, ciphertext, aad);
    if (!plaintext) return std::nullopt;

    // Deserialize the decrypted CHRM message
    return deserialize_message(*plaintext);
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

void TcpTransport::set_signing_keypair(const crypto::KeyPair& kp) {
    signing_keypair_ = &kp;
}

void TcpTransport::set_node_id(const NodeId& id) {
    local_node_id_ = id;
}

void TcpTransport::set_pubkey_lookup(PubkeyLookup fn) {
    pubkey_lookup_ = std::move(fn);
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
static bool is_connection_alive(int fd) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 0);
    if (ret < 0) return false;
    if (ret == 0) return true;

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    if (pfd.revents & POLLIN) {
        char buf;
        ssize_t r = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) return false;
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    }
    return true;
}

// Helper: create a new TCP connection to addr:port. Returns fd or -1 on failure.
// Supports both IP addresses and DNS hostnames via getaddrinfo.
// Uses non-blocking connect with poll() so the caller can be interrupted
// by setting running to false (e.g. on SIGINT).
static int create_connection(const std::string& addr, uint16_t port,
                             uint16_t connect_timeout, const std::atomic<bool>& running) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int gai_err = getaddrinfo(addr.c_str(), port_str.c_str(), &hints, &result);
    if (gai_err != 0) {
        spdlog::error("DNS resolution failed for {}: {}", addr, gai_strerror(gai_err));
        return -1;
    }

    int sockfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sockfd < 0) {
        spdlog::error("send socket() failed: {}", strerror(errno));
        freeaddrinfo(result);
        return -1;
    }

    // Disable Nagle's algorithm for low latency
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set non-blocking for connect
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sockfd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (ret == 0) {
        // Connected immediately — restore blocking mode
        fcntl(sockfd, F_SETFL, flags);
        return sockfd;
    }

    if (errno != EINPROGRESS) {
        close(sockfd);
        spdlog::warn("TCP connect to {}:{} failed: {}", addr, port, strerror(errno));
        return -1;
    }

    // Wait for connect with poll(), checking running flag every 100ms
    int remaining_ms = connect_timeout * 1000;
    while (remaining_ms > 0 && running.load()) {
        struct pollfd pfd{};
        pfd.fd = sockfd;
        pfd.events = POLLOUT;
        int poll_timeout = std::min(remaining_ms, 100);
        int pr = poll(&pfd, 1, poll_timeout);

        if (pr > 0 && (pfd.revents & POLLOUT)) {
            // Check if connect succeeded
            int err = 0;
            socklen_t errlen = sizeof(err);
            getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen);
            if (err == 0) {
                // Success — restore blocking mode
                fcntl(sockfd, F_SETFL, flags);
                return sockfd;
            }
            close(sockfd);
            spdlog::warn("TCP connect to {}:{} failed: {}", addr, port, strerror(err));
            return -1;
        }
        if (pr < 0 && errno != EINTR) {
            close(sockfd);
            spdlog::warn("TCP connect poll to {}:{} failed: {}", addr, port, strerror(errno));
            return -1;
        }
        remaining_ms -= poll_timeout;
    }

    close(sockfd);
    if (!running.load()) {
        spdlog::debug("TCP connect to {}:{} aborted (shutdown)", addr, port);
    } else {
        spdlog::warn("TCP connect to {}:{} timed out", addr, port);
    }
    return -1;
}

// Helper: try to send all data on fd. Returns true on success.
static bool send_all(int fd, const uint8_t* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = ::send(fd, data + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

static bool send_all(int fd, const std::vector<uint8_t>& data) {
    return send_all(fd, data.data(), data.size());
}

bool TcpTransport::perform_initiator_handshake(int fd, const std::string& pool_key) {
    if (!signing_keypair_) return false;

    tcp_encryption::HandshakeInitiator initiator(local_node_id_);

    // Send HELLO (with embedded signing pubkey for identity verification)
    auto hello = initiator.generate_hello(signing_keypair_->public_key);
    if (!send_all(fd, hello)) {
        spdlog::debug("Encryption handshake: HELLO send failed to {}", pool_key);
        return false;
    }

    // Set read timeout for handshake
    struct timeval tv{};
    tv.tv_sec = connect_timeout_;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Read ACCEPT progressively — we don't know total size upfront.
    // ACCEPT: [1B version][1B cipher][32B node_id][2B ct_len][ct][32B random]
    //         [2B sig_pk_len][signing_pk][2B sig_len][sig]

    // Read version(1) + cipher(1) + node_id(32) + ct_len(2) = 36 bytes
    uint8_t accept_header[36];
    if (!read_exact(fd, accept_header, 36)) {
        spdlog::debug("Encryption handshake: ACCEPT header read failed from {}", pool_key);
        return false;
    }

    uint16_t ct_len = (static_cast<uint16_t>(accept_header[34]) << 8) | accept_header[35];
    if (ct_len > 2048) {
        spdlog::debug("Encryption handshake: ct_len {} exceeds max from {}", ct_len, pool_key);
        return false;
    }

    // Read ct + accept_random(32)
    std::vector<uint8_t> ct_and_random(ct_len + 32);
    if (!read_exact(fd, ct_and_random.data(), ct_and_random.size())) {
        spdlog::debug("Encryption handshake: ACCEPT body read failed from {}", pool_key);
        return false;
    }

    // Read sig_pk_len(2)
    uint8_t sig_pk_len_buf[2];
    if (!read_exact(fd, sig_pk_len_buf, 2)) {
        spdlog::debug("Encryption handshake: ACCEPT sig_pk_len read failed from {}", pool_key);
        return false;
    }
    uint16_t sig_pk_len = (static_cast<uint16_t>(sig_pk_len_buf[0]) << 8) | sig_pk_len_buf[1];
    if (sig_pk_len > 4096) {
        spdlog::debug("Encryption handshake: sig_pk_len {} exceeds max from {}", sig_pk_len, pool_key);
        return false;
    }

    // Read signing_pk + sig_len(2)
    std::vector<uint8_t> sig_pk_and_sig_len(sig_pk_len + 2);
    if (!read_exact(fd, sig_pk_and_sig_len.data(), sig_pk_and_sig_len.size())) {
        spdlog::debug("Encryption handshake: ACCEPT signing_pk read failed from {}", pool_key);
        return false;
    }

    uint16_t sig_len = (static_cast<uint16_t>(sig_pk_and_sig_len[sig_pk_len]) << 8) |
                        sig_pk_and_sig_len[sig_pk_len + 1];
    if (sig_len > 8192) {
        spdlog::debug("Encryption handshake: sig_len {} exceeds max from {}", sig_len, pool_key);
        return false;
    }

    // Read signature
    std::vector<uint8_t> sig(sig_len);
    if (sig_len > 0 && !read_exact(fd, sig.data(), sig_len)) {
        spdlog::debug("Encryption handshake: ACCEPT signature read failed from {}", pool_key);
        return false;
    }

    // Assemble full ACCEPT bytes
    std::vector<uint8_t> accept_bytes;
    accept_bytes.reserve(36 + ct_and_random.size() + 2 + sig_pk_and_sig_len.size() + sig.size());
    accept_bytes.insert(accept_bytes.end(), accept_header, accept_header + 36);
    accept_bytes.insert(accept_bytes.end(), ct_and_random.begin(), ct_and_random.end());
    accept_bytes.push_back(sig_pk_len_buf[0]);
    accept_bytes.push_back(sig_pk_len_buf[1]);
    accept_bytes.insert(accept_bytes.end(), sig_pk_and_sig_len.begin(), sig_pk_and_sig_len.end());
    accept_bytes.insert(accept_bytes.end(), sig.begin(), sig.end());

    // Process ACCEPT — signing pubkey is extracted and verified internally
    auto confirm = initiator.process_accept(accept_bytes, signing_keypair_->secret_key);
    if (!confirm) {
        spdlog::debug("Encryption handshake: ACCEPT verification failed from {}", pool_key);
        return false;
    }

    // Send CONFIRM
    if (!send_all(fd, *confirm)) {
        spdlog::debug("Encryption handshake: CONFIRM send failed to {}", pool_key);
        return false;
    }

    // Store session keys in pool
    auto keys = initiator.session_keys();
    if (!keys) return false;

    {
        std::lock_guard lock(pool_mutex_);
        conn_pool_[pool_key] = {fd, std::chrono::steady_clock::now(), std::move(*keys)};
    }

    spdlog::info("TCP encryption established with {}", pool_key);
    return true;
}

bool TcpTransport::send_on_connection(int fd, const std::vector<uint8_t>& data,
                                       std::optional<tcp_encryption::SessionKeys>* session) {
    if (session && session->has_value()) {
        // Encrypt and send
        auto frame = tcp_encryption::encrypt_frame(**session, data);
        return send_all(fd, frame);
    }
    // Plaintext send
    return send_all(fd, data);
}

std::optional<Message> TcpTransport::read_message(int fd, tcp_encryption::SessionKeys* session) {
    if (session) {
        return read_encrypted_message(fd, *session);
    }
    return read_framed_message(fd);
}

void TcpTransport::send(const std::string& addr, uint16_t port, const Message& msg) {
    auto data = serialize_message(msg);
    auto pool_key = "[" + addr + "]:" + std::to_string(port);
    int fd = -1;
    bool from_pool = false;
    std::optional<tcp_encryption::SessionKeys> session;  // local copy, not a pointer into pool

    // Try to get a pooled connection — move session out under lock
    {
        std::lock_guard lock(pool_mutex_);
        auto it = conn_pool_.find(pool_key);
        if (it != conn_pool_.end()) {
            fd = it->second.fd;
            session = std::move(it->second.session);
            it->second.session.reset();
            from_pool = true;
        }
    }

    // Verify pooled connection is still alive
    if (from_pool && !is_connection_alive(fd)) {
        spdlog::debug("Pooled connection to {} is dead (fd={}), creating new", pool_key, fd);
        std::lock_guard lock(pool_mutex_);
        conn_pool_.erase(pool_key);
        close(fd);
        fd = -1;
        from_pool = false;
        session.reset();
    }

    if (from_pool) {
        spdlog::debug("Reusing pooled connection to {} (fd={})", pool_key, fd);
    }

    // If no pooled connection, create a new one
    if (fd < 0) {
        fd = create_connection(addr, port, connect_timeout_, running_);
        if (fd < 0) return;
        spdlog::debug("Created new connection to {} (fd={})", pool_key, fd);

        // Perform encryption handshake on new connections
        if (signing_keypair_) {
            if (perform_initiator_handshake(fd, pool_key)) {
                // Move session out of pool under lock
                std::lock_guard lock(pool_mutex_);
                session = std::move(conn_pool_[pool_key].session);
                conn_pool_[pool_key].session.reset();
                from_pool = true;
            } else {
                spdlog::warn("Encryption handshake failed for {}, dropping message", pool_key);
                close(fd);
                return;
            }
        }
    }

    // Send the message
    bool send_ok;
    if (session) {
        send_ok = send_on_connection(fd, data, &session);
    } else {
        send_ok = send_all(fd, data);
    }

    // If send failed on a pooled connection, retry with a fresh encrypted one
    if (!send_ok && from_pool) {
        spdlog::debug("Send on pooled fd={} failed, retrying with new connection", fd);
        {
            std::lock_guard lock(pool_mutex_);
            conn_pool_.erase(pool_key);
        }
        close(fd);
        session.reset();
        fd = create_connection(addr, port, connect_timeout_, running_);
        if (fd < 0) return;

        if (signing_keypair_) {
            if (perform_initiator_handshake(fd, pool_key)) {
                std::lock_guard lock(pool_mutex_);
                session = std::move(conn_pool_[pool_key].session);
                conn_pool_[pool_key].session.reset();
                send_ok = send_on_connection(fd, data, &session);
            } else {
                spdlog::warn("Encryption handshake failed on retry for {}", pool_key);
                close(fd);
                return;
            }
        } else {
            send_ok = send_all(fd, data);
        }
    }

    if (send_ok) {
        // Return connection and session to pool
        std::lock_guard lock(pool_mutex_);
        if (conn_pool_.find(pool_key) == conn_pool_.end()) {
            if (conn_pool_.size() < max_pool_size_) {
                conn_pool_[pool_key] = {fd, std::chrono::steady_clock::now(), std::move(session)};
                spdlog::debug("Returned fd={} to pool for {}", fd, pool_key);
            } else {
                close(fd);
            }
        } else {
            // Already in pool (from handshake), move session back and update last_used
            conn_pool_[pool_key].session = std::move(session);
            conn_pool_[pool_key].last_used = std::chrono::steady_clock::now();
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
                    close(client_fd);
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

            int client_fd = fds[i].fd;

            // Check if this client has an encrypted session
            auto session_it = client_sessions_.find(client_fd);
            if (session_it != client_sessions_.end()) {
                // Encrypted client — read encrypted message
                auto msg = read_encrypted_message(client_fd, session_it->second);
                if (!msg) {
                    client_sessions_.erase(session_it);
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                char addr_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addrs[i].sin_addr, addr_str, sizeof(addr_str));
                handler(*msg, std::string(addr_str), msg->sender_port);
                ++i;
                continue;
            }

            // Not yet encrypted — peek first byte to decide
            uint8_t first_byte;
            ssize_t peek_r = recv(client_fd, &first_byte, 1, MSG_PEEK);
            if (peek_r <= 0) {
                close(client_fd);
                fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }

            if (first_byte == tcp_encryption::PROBE_BYTE && signing_keypair_) {
                // Encrypted handshake — read full HELLO
                // HELLO: [1B probe][1B version][1B cipher][32B node_id][2B pk_len][pk]
                //        [32B random][2B sig_pk_len][signing_pk]
                // Read the fixed prefix first: 3 + 32 + 2 = 37
                uint8_t hello_prefix[37];
                if (!read_exact(client_fd, hello_prefix, 37)) {
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                uint16_t pk_len = (static_cast<uint16_t>(hello_prefix[35]) << 8) | hello_prefix[36];
                if (pk_len > 2048) {
                    spdlog::debug("TCP encryption: pk_len {} exceeds max from fd={}", pk_len, client_fd);
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Read pk + random(32)
                std::vector<uint8_t> hello_rest(pk_len + 32);
                if (!read_exact(client_fd, hello_rest.data(), hello_rest.size())) {
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Read sig_pk_len(2)
                uint8_t sig_pk_len_buf[2];
                if (!read_exact(client_fd, sig_pk_len_buf, 2)) {
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }
                uint16_t sig_pk_len = (static_cast<uint16_t>(sig_pk_len_buf[0]) << 8) |
                                       sig_pk_len_buf[1];
                if (sig_pk_len > 4096) {
                    spdlog::debug("TCP encryption: sig_pk_len {} exceeds max from fd={}", sig_pk_len, client_fd);
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Read signing_pk
                std::vector<uint8_t> signing_pk(sig_pk_len);
                if (sig_pk_len > 0 && !read_exact(client_fd, signing_pk.data(), sig_pk_len)) {
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Assemble full HELLO
                std::vector<uint8_t> hello_bytes;
                hello_bytes.reserve(37 + hello_rest.size() + 2 + signing_pk.size());
                hello_bytes.insert(hello_bytes.end(), hello_prefix, hello_prefix + 37);
                hello_bytes.insert(hello_bytes.end(), hello_rest.begin(), hello_rest.end());
                hello_bytes.push_back(sig_pk_len_buf[0]);
                hello_bytes.push_back(sig_pk_len_buf[1]);
                hello_bytes.insert(hello_bytes.end(), signing_pk.begin(), signing_pk.end());

                // Process handshake as responder
                tcp_encryption::HandshakeResponder responder(local_node_id_);
                auto init_id = responder.process_hello(hello_bytes);
                if (!init_id) {
                    spdlog::debug("TCP encryption: invalid HELLO from client fd={}", client_fd);
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Generate and send ACCEPT (with embedded signing pubkey)
                auto accept = responder.generate_accept(signing_keypair_->secret_key,
                                                         signing_keypair_->public_key);
                if (!accept || !send_all(client_fd, *accept)) {
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Read CONFIRM: [2B sig_len][sig]
                uint8_t confirm_header[2];
                if (!read_exact(client_fd, confirm_header, 2)) {
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }
                uint16_t sig_len = (static_cast<uint16_t>(confirm_header[0]) << 8) | confirm_header[1];
                if (sig_len > 8192) {
                    spdlog::debug("TCP encryption: confirm sig_len {} exceeds max from fd={}", sig_len, client_fd);
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }
                std::vector<uint8_t> confirm_bytes(2 + sig_len);
                confirm_bytes[0] = confirm_header[0];
                confirm_bytes[1] = confirm_header[1];
                if (sig_len > 0 && !read_exact(client_fd, confirm_bytes.data() + 2, sig_len)) {
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Verify CONFIRM — initiator's pubkey was extracted from HELLO
                bool confirm_ok = responder.process_confirm(confirm_bytes);

                if (!confirm_ok) {
                    spdlog::debug("TCP encryption: CONFIRM verification failed from fd={}", client_fd);
                    // Fall through to plaintext — the initiator can still send messages
                    // if they failed the handshake but kept the connection open.
                    // In practice, both sides should just close.
                    close(client_fd);
                    fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                    client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }

                // Store session keys for this client fd
                auto keys = responder.session_keys();
                if (keys) {
                    client_sessions_[client_fd] = std::move(*keys);
                    char addr_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addrs[i].sin_addr, addr_str, sizeof(addr_str));
                    spdlog::info("TCP encryption established with client fd={} ({})", client_fd, addr_str);
                }

                ++i;
                continue;
            }

            if (signing_keypair_) {
                // Encryption configured — reject plaintext connections
                spdlog::debug("Rejecting plaintext connection from fd={}", client_fd);
                close(client_fd);
                fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }

            // Plaintext CHRM message (only when encryption is not configured, e.g. tests)
            auto msg = read_framed_message(client_fd);
            if (!msg) {
                close(client_fd);
                fds.erase(fds.begin() + static_cast<ptrdiff_t>(i));
                client_addrs.erase(client_addrs.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }

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
    client_sessions_.clear();
}

void TcpTransport::stop() {
    running_.store(false);
}

uint16_t TcpTransport::local_port() const {
    return port_;
}

} // namespace chromatin::kademlia
