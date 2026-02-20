#include "replication/repl_log.h"

#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

namespace chromatin::replication {

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> serialize_entry(const LogEntry& entry) {
    std::vector<uint8_t> buf;
    buf.reserve(8 + 1 + 8 + 1 + 4 + entry.data.size());

    // seq (8 bytes BE)
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((entry.seq >> (i * 8)) & 0xFF));
    }

    // op (1 byte)
    buf.push_back(static_cast<uint8_t>(entry.op));

    // timestamp (8 bytes BE)
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((entry.timestamp >> (i * 8)) & 0xFF));
    }

    // data_type (1 byte)
    buf.push_back(entry.data_type);

    // data_length (4 bytes BE)
    uint32_t data_len = static_cast<uint32_t>(entry.data.size());
    buf.push_back(static_cast<uint8_t>((data_len >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((data_len >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((data_len >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(data_len & 0xFF));

    // data
    buf.insert(buf.end(), entry.data.begin(), entry.data.end());

    return buf;
}

LogEntry deserialize_entry(std::span<const uint8_t> data) {
    // Minimum: 8 (seq) + 1 (op) + 8 (timestamp) + 1 (data_type) + 4 (data_length) = 22 bytes
    if (data.size() < 22) {
        throw std::runtime_error("LogEntry data too short to deserialize");
    }

    LogEntry entry;
    size_t offset = 0;

    // seq (8 bytes BE)
    entry.seq = 0;
    for (int i = 0; i < 8; ++i) {
        entry.seq = (entry.seq << 8) | data[offset + i];
    }
    offset += 8;

    // op (1 byte)
    entry.op = static_cast<Op>(data[offset]);
    offset += 1;

    // timestamp (8 bytes BE)
    entry.timestamp = 0;
    for (int i = 0; i < 8; ++i) {
        entry.timestamp = (entry.timestamp << 8) | data[offset + i];
    }
    offset += 8;

    // data_type (1 byte)
    entry.data_type = data[offset];
    offset += 1;

    // data_length (4 bytes BE)
    uint32_t data_len = (static_cast<uint32_t>(data[offset]) << 24)
                      | (static_cast<uint32_t>(data[offset + 1]) << 16)
                      | (static_cast<uint32_t>(data[offset + 2]) << 8)
                      | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    if (offset + data_len > data.size()) {
        throw std::runtime_error("LogEntry data truncated");
    }

    entry.data.assign(data.begin() + offset, data.begin() + offset + data_len);

    return entry;
}

// ---------------------------------------------------------------------------
// ReplLog
// ---------------------------------------------------------------------------

ReplLog::ReplLog(storage::Storage& storage)
    : storage_(storage) {}

std::vector<uint8_t> ReplLog::make_composite_key(const crypto::Hash& key, uint64_t seq) {
    std::vector<uint8_t> composite;
    composite.reserve(32 + 8);

    // data_key_hash (32 bytes)
    composite.insert(composite.end(), key.begin(), key.end());

    // seq_number (8 bytes BE)
    for (int i = 7; i >= 0; --i) {
        composite.push_back(static_cast<uint8_t>((seq >> (i * 8)) & 0xFF));
    }

    return composite;
}

uint64_t ReplLog::append(const crypto::Hash& key, Op op, uint8_t data_type, std::span<const uint8_t> data) {
    std::lock_guard lock(append_mutex_);
    uint64_t seq = current_seq(key) + 1;

    auto now = std::chrono::system_clock::now();
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

    LogEntry entry;
    entry.seq = seq;
    entry.op = op;
    entry.timestamp = timestamp;
    entry.data_type = data_type;
    entry.data.assign(data.begin(), data.end());

    auto composite_key = make_composite_key(key, seq);
    auto serialized = serialize_entry(entry);

    storage_.put(storage::TABLE_REPL_LOG, composite_key, serialized);

    spdlog::debug("ReplLog: appended seq {} for key (op=0x{:02X})", seq, static_cast<uint8_t>(op));
    return seq;
}

std::vector<LogEntry> ReplLog::entries_after(const crypto::Hash& key, uint64_t after_seq) const {
    std::vector<LogEntry> result;
    std::vector<uint8_t> prefix(key.begin(), key.end());

    storage_.scan(storage::TABLE_REPL_LOG, prefix, [&](std::span<const uint8_t> k, std::span<const uint8_t> v) -> bool {
        if (k.size() < 40) return true; // skip malformed entries

        // Extract seq from composite key (bytes 32..39, BE)
        uint64_t entry_seq = 0;
        for (int i = 0; i < 8; ++i) {
            entry_seq = (entry_seq << 8) | k[32 + i];
        }

        if (entry_seq > after_seq) {
            try {
                result.push_back(deserialize_entry(v));
            } catch (const std::exception& e) {
                spdlog::warn("ReplLog: failed to deserialize entry seq {}: {}", entry_seq, e.what());
            }
        }

        return true; // continue scanning within prefix
    });

    return result;
}

void ReplLog::apply(const crypto::Hash& key, const std::vector<LogEntry>& entries) {
    for (const auto& entry : entries) {
        auto composite_key = make_composite_key(key, entry.seq);

        // Idempotent: skip if this seq already exists
        auto existing = storage_.get(storage::TABLE_REPL_LOG, composite_key);
        if (existing) {
            spdlog::debug("ReplLog: skipping existing seq {} during apply", entry.seq);
            continue;
        }

        auto serialized = serialize_entry(entry);
        storage_.put(storage::TABLE_REPL_LOG, composite_key, serialized);

        spdlog::debug("ReplLog: applied remote seq {} (op=0x{:02X})", entry.seq, static_cast<uint8_t>(entry.op));
    }
}

uint64_t ReplLog::current_seq(const crypto::Hash& key) const {
    // Build upper bound: key || 0xFF...FF (8 bytes) — past all possible seqs
    std::vector<uint8_t> prefix(key.begin(), key.end());
    std::vector<uint8_t> upper(40);
    std::copy(key.begin(), key.end(), upper.begin());
    std::fill(upper.begin() + 32, upper.end(), 0xFF);

    // Seek to last entry with this prefix — O(1) instead of O(N)
    uint64_t max_seq = 0;
    storage_.reverse_scan_one(storage::TABLE_REPL_LOG, prefix, upper,
        [&](std::span<const uint8_t> k, std::span<const uint8_t> /*v*/) -> bool {
            if (k.size() >= 40) {
                for (int i = 0; i < 8; ++i) {
                    max_seq = (max_seq << 8) | k[32 + i];
                }
            }
            return false;
        });

    return max_seq;
}

void ReplLog::compact(const crypto::Hash& key, uint64_t before_seq) {
    // Collect keys to delete first, then delete them
    std::vector<std::vector<uint8_t>> to_delete;
    std::vector<uint8_t> prefix(key.begin(), key.end());

    storage_.scan(storage::TABLE_REPL_LOG, prefix, [&](std::span<const uint8_t> k, std::span<const uint8_t> /*v*/) -> bool {
        if (k.size() < 40) return true;

        // Extract seq from composite key
        uint64_t entry_seq = 0;
        for (int i = 0; i < 8; ++i) {
            entry_seq = (entry_seq << 8) | k[32 + i];
        }

        if (entry_seq < before_seq) {
            to_delete.emplace_back(k.begin(), k.end());
        }

        return true;
    });

    for (const auto& ck : to_delete) {
        storage_.del(storage::TABLE_REPL_LOG, ck);
    }

    spdlog::debug("ReplLog: compacted {} entries before seq {}", to_delete.size(), before_seq);
}

} // namespace chromatin::replication
