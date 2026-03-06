#include "crypto/hash.h"
#include <oqs/oqs.h>
#include <oqs/sha3.h>

namespace chromatin::crypto {

std::array<uint8_t, SHA3_256_SIZE> sha3_256(std::span<const uint8_t> input) {
    std::array<uint8_t, SHA3_256_SIZE> output{};
    OQS_SHA3_sha3_256(output.data(), input.data(), input.size());
    return output;
}

std::array<uint8_t, SHA3_256_SIZE> sha3_256(const void* data, size_t len) {
    std::array<uint8_t, SHA3_256_SIZE> output{};
    OQS_SHA3_sha3_256(output.data(), static_cast<const uint8_t*>(data), len);
    return output;
}

} // namespace chromatin::crypto
