#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "crypto/crypto.h"
#include "storage/storage.h"

namespace chromatin::replication {

enum class Op : uint8_t { ADD = 0x00, DEL = 0x01, UPD = 0x02 };

struct LogEntry {
    uint64_t seq;
    Op op;
    uint64_t timestamp;
    std::vector<uint8_t> data;
};

// Serialization for storage in mdbx and for SYNC_RESP payloads.
// Format:
//   [8 bytes BE: seq]
//   [1 byte: op]
//   [8 bytes BE: timestamp]
//   [4 bytes BE: data_length]
//   [data bytes]
std::vector<uint8_t> serialize_entry(const LogEntry& entry);
LogEntry deserialize_entry(std::span<const uint8_t> data);

class ReplLog {
public:
    explicit ReplLog(storage::Storage& storage);

    // Append new entry, returns assigned seq number
    uint64_t append(const crypto::Hash& key, Op op, std::span<const uint8_t> data);

    // Get entries after a given seq (for SYNC_REQ response)
    std::vector<LogEntry> entries_after(const crypto::Hash& key, uint64_t after_seq) const;

    // Apply remote entries (idempotent -- skip existing seqs)
    void apply(const crypto::Hash& key, const std::vector<LogEntry>& entries);

    // Current highest seq for a key (0 if no entries)
    uint64_t current_seq(const crypto::Hash& key) const;

    // Delete entries before seq (compaction)
    void compact(const crypto::Hash& key, uint64_t before_seq);

private:
    storage::Storage& storage_;

    // Build composite key: [32 bytes: data_key_hash][8 bytes BE: seq_number]
    static std::vector<uint8_t> make_composite_key(const crypto::Hash& key, uint64_t seq);
};

} // namespace chromatin::replication
