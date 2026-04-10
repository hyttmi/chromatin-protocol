#include "relay/util/base64.h"

#include <openssl/evp.h>

namespace chromatindb::relay::util {

std::string base64_encode(std::span<const uint8_t> data) {
    if (data.empty()) return {};

    // EVP_EncodeBlock output size: 4 * ceil(n/3) + 1 (null terminator)
    auto out_len = 4 * ((static_cast<int>(data.size()) + 2) / 3);
    std::string result(static_cast<size_t>(out_len), '\0');

    int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()),
        data.data(),
        static_cast<int>(data.size()));

    result.resize(static_cast<size_t>(written));
    return result;
}

std::optional<std::vector<uint8_t>> base64_decode(std::string_view encoded) {
    if (encoded.empty()) return std::vector<uint8_t>{};

    // Reject invalid characters before calling EVP_DecodeBlock
    for (auto c : encoded) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
            return std::nullopt;
        }
    }

    // Length must be a multiple of 4
    if (encoded.size() % 4 != 0) {
        return std::nullopt;
    }

    // EVP_DecodeBlock output size: 3 * (n/4) -- includes padding bytes
    auto max_out = 3 * (static_cast<int>(encoded.size()) / 4);
    std::vector<uint8_t> result(static_cast<size_t>(max_out));

    int written = EVP_DecodeBlock(
        result.data(),
        reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size()));

    if (written < 0) {
        return std::nullopt;
    }

    // EVP_DecodeBlock includes padding bytes in output -- subtract trailing '='
    size_t actual = static_cast<size_t>(written);
    if (encoded.size() >= 1 && encoded[encoded.size() - 1] == '=') actual--;
    if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=') actual--;

    result.resize(actual);
    return result;
}

} // namespace chromatindb::relay::util
