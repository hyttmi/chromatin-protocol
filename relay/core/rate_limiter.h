#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace chromatindb::relay::core {

/// Header-only token bucket rate limiter.
///
/// rate=0 means disabled (try_consume always returns true).
/// burst equals rate -- the full bucket is available immediately after set_rate().
/// Uses steady_clock (never system_clock).
class RateLimiter {
public:
    /// Set the rate (messages per second). Resets tokens to full bucket.
    void set_rate(uint32_t rate) {
        rate_ = rate;
        burst_ = rate;
        tokens_ = static_cast<double>(rate);
        consecutive_rejects_ = 0;
        last_refill_ = std::chrono::steady_clock::now();
    }

    /// Try to consume one token. Returns true if allowed, false if rate-limited.
    bool try_consume() {
        if (rate_ == 0) return true;  // Disabled mode
        refill();
        if (tokens_ < 1.0) {
            ++consecutive_rejects_;
            return false;
        }
        tokens_ -= 1.0;
        consecutive_rejects_ = 0;
        return true;
    }

    /// Returns true if consecutive rejections meet or exceed the threshold.
    bool should_disconnect(uint32_t threshold) const {
        return consecutive_rejects_ >= threshold;
    }

    /// Returns the current consecutive rejection count (for testing).
    uint32_t consecutive_rejects() const { return consecutive_rejects_; }

    /// Returns the current rate setting (for comparing against shared atomic).
    uint32_t current_rate() const { return rate_; }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
        tokens_ = std::min(tokens_ + elapsed * rate_, static_cast<double>(burst_));
        last_refill_ = now;
    }

    uint32_t rate_ = 0;
    uint32_t burst_ = 0;
    double tokens_ = 0.0;
    uint32_t consecutive_rejects_ = 0;
    std::chrono::steady_clock::time_point last_refill_ = std::chrono::steady_clock::now();
};

} // namespace chromatindb::relay::core
