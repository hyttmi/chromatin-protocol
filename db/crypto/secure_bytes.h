#pragma once

#include <cstdint>
#include <cstring>
#include <sodium.h>
#include <stdexcept>
#include <utility>
#include <span>

namespace chromatindb::crypto {

/// Ensures sodium_init() is called exactly once.
inline void ensure_sodium_init() {
    static const bool initialized = [] {
        if (sodium_init() < 0) {
            throw std::runtime_error("Failed to initialize libsodium");
        }
        return true;
    }();
    (void)initialized;
}

/// A secure byte container that zeroes memory on destruction.
/// Prevents sensitive key material from lingering in memory.
/// Move-only: copying secret keys is a security anti-pattern.
class SecureBytes {
public:
    SecureBytes() = default;

    explicit SecureBytes(size_t size)
        : data_(new uint8_t[size]()), size_(size) {
        ensure_sodium_init();
    }

    SecureBytes(const uint8_t* src, size_t size)
        : data_(new uint8_t[size]), size_(size) {
        ensure_sodium_init();
        std::memcpy(data_, src, size);
    }

    ~SecureBytes() {
        if (data_) {
            sodium_memzero(data_, size_);
            delete[] data_;
        }
    }

    // Move only
    SecureBytes(SecureBytes&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    SecureBytes& operator=(SecureBytes&& other) noexcept {
        if (this != &other) {
            if (data_) {
                sodium_memzero(data_, size_);
                delete[] data_;
            }
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // Delete copy
    SecureBytes(const SecureBytes&) = delete;
    SecureBytes& operator=(const SecureBytes&) = delete;

    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    std::span<uint8_t> span() { return {data_, size_}; }
    std::span<const uint8_t> span() const { return {data_, size_}; }

    uint8_t& operator[](size_t i) { return data_[i]; }
    const uint8_t& operator[](size_t i) const { return data_[i]; }

    bool operator==(const SecureBytes& other) const {
        if (size_ != other.size_) return false;
        // Constant-time comparison for security
        return sodium_memcmp(data_, other.data_, size_) == 0;
    }

private:
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace chromatindb::crypto
