#include "net/framing.h"

#include <cstring>
#include <stdexcept>

namespace chromatin::net {

std::array<uint8_t, crypto::AEAD::NONCE_SIZE> make_nonce(uint64_t counter) {
    std::array<uint8_t, crypto::AEAD::NONCE_SIZE> nonce{};
    // First 4 bytes are zero (already initialized)
    // Last 8 bytes are big-endian counter
    for (int i = 7; i >= 0; --i) {
        nonce[4 + i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    return nonce;
}

std::vector<uint8_t> write_frame(
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> key,
    uint64_t counter) {

    auto nonce = make_nonce(counter);
    std::span<const uint8_t> empty_ad{};

    auto ciphertext = crypto::AEAD::encrypt(plaintext, empty_ad, nonce, key);
    uint32_t ct_len = static_cast<uint32_t>(ciphertext.size());

    // Build frame: [4-byte BE length][ciphertext]
    std::vector<uint8_t> frame(FRAME_HEADER_SIZE + ciphertext.size());
    frame[0] = static_cast<uint8_t>((ct_len >> 24) & 0xFF);
    frame[1] = static_cast<uint8_t>((ct_len >> 16) & 0xFF);
    frame[2] = static_cast<uint8_t>((ct_len >> 8) & 0xFF);
    frame[3] = static_cast<uint8_t>(ct_len & 0xFF);
    std::memcpy(frame.data() + FRAME_HEADER_SIZE, ciphertext.data(), ciphertext.size());

    return frame;
}

std::optional<FrameResult> read_frame(
    std::span<const uint8_t> buffer,
    std::span<const uint8_t> key,
    uint64_t counter) {

    if (buffer.size() < FRAME_HEADER_SIZE) {
        throw std::runtime_error("buffer too short for frame header");
    }

    // Parse big-endian length prefix
    uint32_t ct_len = (static_cast<uint32_t>(buffer[0]) << 24) |
                      (static_cast<uint32_t>(buffer[1]) << 16) |
                      (static_cast<uint32_t>(buffer[2]) << 8) |
                      static_cast<uint32_t>(buffer[3]);

    if (ct_len > MAX_FRAME_SIZE) {
        throw std::runtime_error("frame exceeds maximum size");
    }

    if (buffer.size() < FRAME_HEADER_SIZE + ct_len) {
        throw std::runtime_error("buffer too short for declared frame");
    }

    auto ciphertext = buffer.subspan(FRAME_HEADER_SIZE, ct_len);
    auto nonce = make_nonce(counter);
    std::span<const uint8_t> empty_ad{};

    auto plaintext = crypto::AEAD::decrypt(ciphertext, empty_ad, nonce, key);
    if (!plaintext) {
        return std::nullopt;
    }

    return FrameResult{
        .plaintext = std::move(*plaintext),
        .bytes_consumed = FRAME_HEADER_SIZE + ct_len
    };
}

} // namespace chromatin::net
