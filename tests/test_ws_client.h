#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Minimal WebSocket client for testing. Sends/receives JSON text frames.
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
        if (fd_ < 0) return false;

        std::vector<uint8_t> frame;
        frame.push_back(0x81);  // FIN + text opcode

        // Mask bit set (client must mask), payload length
        if (msg.size() < 126) {
            frame.push_back(0x80 | static_cast<uint8_t>(msg.size()));
        } else {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((msg.size() >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(msg.size() & 0xFF));
        }

        // Mask key (fixed for simplicity)
        uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.insert(frame.end(), mask, mask + 4);

        // Masked payload
        for (size_t i = 0; i < msg.size(); ++i) {
            frame.push_back(msg[i] ^ mask[i % 4]);
        }

        return write_all(reinterpret_cast<const char*>(frame.data()), frame.size()) >= 0;
    }

    std::optional<std::string> recv_text(int timeout_ms = 2000) {
        if (fd_ < 0) return std::nullopt;

        // Set receive timeout
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t header[2];
        if (recv_all(header, 2) < 0) return std::nullopt;

        // Check FIN + text opcode
        if ((header[0] & 0x0F) != 0x01) return std::nullopt;

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

        std::vector<char> buf(payload_len);
        if (recv_all(reinterpret_cast<uint8_t*>(buf.data()), payload_len) < 0)
            return std::nullopt;

        return std::string(buf.begin(), buf.end());
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
