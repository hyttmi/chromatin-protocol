#include "kademlia/udp_transport.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "crypto/crypto.h"

namespace helix::kademlia {

// ---------------------------------------------------------------------------
// Wire format helpers
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_signed_data(const Message& msg) {
    // signed_data = magic || version || type || reserved || sender_id || payload_length || payload
    size_t total = 5 + 1 + 1 + 1 + 32 + 4 + msg.payload.size();
    std::vector<uint8_t> buf;
    buf.reserve(total);

    // Magic
    buf.insert(buf.end(), std::begin(HELIX_MAGIC), std::end(HELIX_MAGIC));

    // Version
    buf.push_back(PROTOCOL_VERSION);

    // Type
    buf.push_back(static_cast<uint8_t>(msg.type));

    // Reserved
    buf.push_back(0x00);

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
    // Minimum size: header(44) + sig_length(2) = 46 bytes (with 0-length payload and 0-length sig)
    if (data.size() < HEADER_SIZE + 2) {
        return std::nullopt;
    }

    size_t offset = 0;

    // Verify magic
    if (std::memcmp(data.data() + offset, HELIX_MAGIC, 5) != 0) {
        return std::nullopt;
    }
    offset += 5;

    // Verify version
    if (data[offset] != PROTOCOL_VERSION) {
        return std::nullopt;
    }
    offset += 1;

    // Type
    Message msg;
    msg.type = static_cast<MessageType>(data[offset]);
    offset += 1;

    // Reserved (skip)
    offset += 1;

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
// UdpTransport
// ---------------------------------------------------------------------------

UdpTransport::UdpTransport(const std::string& bind_addr, uint16_t port) : port_(port) {
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));
    }

    // Allow address reuse
    int optval = 1;
    setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind_addr.empty() || bind_addr == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
            close(sockfd_);
            sockfd_ = -1;
            throw std::runtime_error("Invalid bind address: " + bind_addr);
        }
    }

    if (bind(sockfd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sockfd_);
        sockfd_ = -1;
        throw std::runtime_error("bind() failed: " + std::string(strerror(errno)));
    }

    // Read back assigned port (needed when port == 0)
    struct sockaddr_in bound_addr{};
    socklen_t bound_len = sizeof(bound_addr);
    if (getsockname(sockfd_, reinterpret_cast<struct sockaddr*>(&bound_addr), &bound_len) == 0) {
        port_ = ntohs(bound_addr.sin_port);
    }

    spdlog::info("UDP transport bound to {}:{}", bind_addr.empty() ? "0.0.0.0" : bind_addr, port_);
}

UdpTransport::~UdpTransport() {
    stop();
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

void UdpTransport::send(const std::string& addr, uint16_t port, const Message& msg) {
    auto data = serialize_message(msg);

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, addr.c_str(), &dest.sin_addr) != 1) {
        spdlog::error("Invalid destination address: {}", addr);
        return;
    }

    ssize_t sent = sendto(sockfd_, data.data(), data.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        spdlog::error("sendto() failed: {}", strerror(errno));
    }
}

void UdpTransport::run(Handler handler) {
    running_.store(true);

    std::vector<uint8_t> buf(MAX_UDP_MESSAGE);

    while (running_.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd_, &readfds);

        // 100ms timeout so stop() can break the loop
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ret = select(sockfd_ + 1, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            spdlog::error("select() failed: {}", strerror(errno));
            break;
        }

        if (ret == 0) {
            // Timeout — check running_ and loop
            continue;
        }

        struct sockaddr_in from_addr{};
        socklen_t from_len = sizeof(from_addr);

        ssize_t received = recvfrom(sockfd_, buf.data(), buf.size(), 0,
                                    reinterpret_cast<struct sockaddr*>(&from_addr), &from_len);
        if (received <= 0) {
            continue;
        }

        auto msg = deserialize_message(std::span<const uint8_t>(buf.data(), static_cast<size_t>(received)));
        if (!msg) {
            spdlog::warn("Received invalid UDP message ({} bytes), dropping", received);
            continue;
        }

        // Convert sender address to string
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from_addr.sin_addr, addr_str, sizeof(addr_str));
        uint16_t from_port = ntohs(from_addr.sin_port);

        handler(*msg, std::string(addr_str), from_port);
    }
}

void UdpTransport::stop() {
    running_.store(false);
}

uint16_t UdpTransport::local_port() const {
    return port_;
}

} // namespace helix::kademlia
