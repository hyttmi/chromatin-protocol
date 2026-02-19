#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

struct WsFrame {
    uint8_t opcode;  // 0x01 = text, 0x02 = binary
    std::vector<uint8_t> data;
};

// Minimal WebSocket client for testing. Sends/receives JSON text frames
// and binary frames for chunked uploads/downloads.
class TestWsClient {
public:
    TestWsClient() = default;
    TestWsClient(const TestWsClient&) = delete;
    TestWsClient& operator=(const TestWsClient&) = delete;

    bool connect(const std::string& host, uint16_t port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // WebSocket HTTP upgrade handshake
        std::string req =
            "GET / HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";

        if (write_all(req.data(), req.size()) < 0) return false;

        // Read HTTP response (look for 101)
        char buf[1024];
        ssize_t n = recv(fd_, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return false;
        buf[n] = '\0';
        return std::string(buf).find("101") != std::string::npos;
    }

    bool send_text(const std::string& msg) {
        return send_frame(0x81, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    }

    bool send_binary(const std::vector<uint8_t>& data) {
        return send_frame(0x82, data.data(), data.size());
    }

    std::optional<WsFrame> recv_frame(int timeout_ms = 2000) {
        if (fd_ < 0) return std::nullopt;

        // Set receive timeout
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t header[2];
        if (recv_all(header, 2) < 0) return std::nullopt;

        uint8_t opcode = header[0] & 0x0F;

        uint64_t payload_len = header[1] & 0x7F;
        if (payload_len == 126) {
            uint8_t ext[2];
            if (recv_all(ext, 2) < 0) return std::nullopt;
            payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8];
            if (recv_all(ext, 8) < 0) return std::nullopt;
            payload_len = 0;
            for (int i = 0; i < 8; ++i)
                payload_len = (payload_len << 8) | ext[i];
        }

        std::vector<uint8_t> data(payload_len);
        if (payload_len > 0) {
            if (recv_all(data.data(), payload_len) < 0)
                return std::nullopt;
        }

        return WsFrame{opcode, std::move(data)};
    }

    std::optional<std::string> recv_text(int timeout_ms = 2000) {
        auto frame = recv_frame(timeout_ms);
        if (!frame || frame->opcode != 0x01) return std::nullopt;
        return std::string(frame->data.begin(), frame->data.end());
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~TestWsClient() { close(); }

private:
    int fd_ = -1;

    bool send_frame(uint8_t opcode_byte, const uint8_t* payload, size_t len) {
        if (fd_ < 0) return false;

        std::vector<uint8_t> frame;
        frame.push_back(opcode_byte);  // FIN + opcode

        // Mask bit set (client must mask), payload length
        if (len < 126) {
            frame.push_back(0x80 | static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }

        // Mask key (fixed for simplicity)
        uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.insert(frame.end(), mask, mask + 4);

        // Masked payload
        for (size_t i = 0; i < len; ++i) {
            frame.push_back(payload[i] ^ mask[i % 4]);
        }

        return write_all(reinterpret_cast<const char*>(frame.data()), frame.size()) >= 0;
    }

    ssize_t write_all(const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return -1;
            sent += n;
        }
        return static_cast<ssize_t>(sent);
    }

    ssize_t recv_all(uint8_t* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::recv(fd_, buf + got, len - got, 0);
            if (n <= 0) return -1;
            got += n;
        }
        return static_cast<ssize_t>(got);
    }
};
