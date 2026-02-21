#include <gtest/gtest.h>

#include "crypto/crypto.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

using namespace chromatin::replication;
using namespace chromatin::crypto;
using namespace chromatin::storage;

class ReplLogTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<Storage> storage_;
    std::unique_ptr<ReplLog> repl_log_;

    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("chromatin_repl_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(db_path_);
        storage_ = std::make_unique<Storage>(db_path_ / "test.mdbx");
        repl_log_ = std::make_unique<ReplLog>(*storage_);
    }

    void TearDown() override {
        repl_log_.reset();
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    static Hash make_key(uint8_t fill) {
        Hash key{};
        key.fill(fill);
        return key;
    }
};

// ---------------------------------------------------------------------------
// Test 1: AppendIncrementsSeq -- append 3 entries, seqs are 1, 2, 3
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, AppendIncrementsSeq) {
    Hash key = make_key(0x01);
    std::vector<uint8_t> data1 = {0xAA};
    std::vector<uint8_t> data2 = {0xBB};
    std::vector<uint8_t> data3 = {0xCC};

    uint64_t seq1 = repl_log_->append(key, Op::ADD, 0x00,data1);
    uint64_t seq2 = repl_log_->append(key, Op::ADD, 0x00,data2);
    uint64_t seq3 = repl_log_->append(key, Op::ADD, 0x00,data3);

    EXPECT_EQ(seq1, 1u);
    EXPECT_EQ(seq2, 2u);
    EXPECT_EQ(seq3, 3u);
}

// ---------------------------------------------------------------------------
// Test 2: CurrentSeq -- 0 initially, increments with append
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, CurrentSeq) {
    Hash key = make_key(0x02);

    EXPECT_EQ(repl_log_->current_seq(key), 0u);

    std::vector<uint8_t> data = {0x11};
    repl_log_->append(key, Op::ADD, 0x00,data);
    EXPECT_EQ(repl_log_->current_seq(key), 1u);

    repl_log_->append(key, Op::UPD, 0x00,data);
    EXPECT_EQ(repl_log_->current_seq(key), 2u);

    repl_log_->append(key, Op::DEL, 0x00,data);
    EXPECT_EQ(repl_log_->current_seq(key), 3u);
}

// ---------------------------------------------------------------------------
// Test 3: EntriesAfter -- append 4, entries_after(2) returns entries 3 and 4
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, EntriesAfter) {
    Hash key = make_key(0x03);
    std::vector<uint8_t> d1 = {0x01};
    std::vector<uint8_t> d2 = {0x02};
    std::vector<uint8_t> d3 = {0x03};
    std::vector<uint8_t> d4 = {0x04};

    repl_log_->append(key, Op::ADD, 0x00,d1);
    repl_log_->append(key, Op::ADD, 0x00,d2);
    repl_log_->append(key, Op::ADD, 0x00,d3);
    repl_log_->append(key, Op::ADD, 0x00,d4);

    auto entries = repl_log_->entries_after(key, 2);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].seq, 3u);
    EXPECT_EQ(entries[0].data, d3);
    EXPECT_EQ(entries[1].seq, 4u);
    EXPECT_EQ(entries[1].data, d4);
}

// ---------------------------------------------------------------------------
// Test 4: EntriesAfterZero -- returns all entries
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, EntriesAfterZero) {
    Hash key = make_key(0x04);
    std::vector<uint8_t> d1 = {0x10};
    std::vector<uint8_t> d2 = {0x20};

    repl_log_->append(key, Op::ADD, 0x00,d1);
    repl_log_->append(key, Op::ADD, 0x00,d2);

    auto entries = repl_log_->entries_after(key, 0);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].seq, 1u);
    EXPECT_EQ(entries[0].data, d1);
    EXPECT_EQ(entries[1].seq, 2u);
    EXPECT_EQ(entries[1].data, d2);
}

// ---------------------------------------------------------------------------
// Test 5: ApplyIdempotent -- append 2, apply [seq2 (dup), seq3 (new)], result = 3
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, ApplyIdempotent) {
    Hash key = make_key(0x05);
    std::vector<uint8_t> d1 = {0xA1};
    std::vector<uint8_t> d2 = {0xA2};

    repl_log_->append(key, Op::ADD, 0x00,d1);
    repl_log_->append(key, Op::ADD, 0x00,d2);

    // Build entries to apply: seq 2 (duplicate) and seq 3 (new)
    LogEntry dup_entry;
    dup_entry.seq = 2;
    dup_entry.op = Op::ADD;
    dup_entry.timestamp = 1000;
    dup_entry.data_type = 0x00;
    dup_entry.data = {0xA2};

    LogEntry new_entry;
    new_entry.seq = 3;
    new_entry.op = Op::ADD;
    new_entry.timestamp = 2000;
    new_entry.data_type = 0x00;
    new_entry.data = {0xA3};

    repl_log_->apply(key, {dup_entry, new_entry});

    // Should have 3 entries total
    auto entries = repl_log_->entries_after(key, 0);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].seq, 1u);
    EXPECT_EQ(entries[1].seq, 2u);
    EXPECT_EQ(entries[2].seq, 3u);
    EXPECT_EQ(entries[2].data, std::vector<uint8_t>({0xA3}));
}

// ---------------------------------------------------------------------------
// Test 6: SeparateKeys -- different data keys have independent seq counters
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, SeparateKeys) {
    Hash key_a = make_key(0x10);
    Hash key_b = make_key(0x20);

    std::vector<uint8_t> data = {0xFF};

    uint64_t seq_a1 = repl_log_->append(key_a, Op::ADD, 0x00, data);
    uint64_t seq_a2 = repl_log_->append(key_a, Op::ADD, 0x00, data);
    uint64_t seq_b1 = repl_log_->append(key_b, Op::ADD, 0x00, data);

    EXPECT_EQ(seq_a1, 1u);
    EXPECT_EQ(seq_a2, 2u);
    EXPECT_EQ(seq_b1, 1u);

    EXPECT_EQ(repl_log_->current_seq(key_a), 2u);
    EXPECT_EQ(repl_log_->current_seq(key_b), 1u);
}

// ---------------------------------------------------------------------------
// Test 7: Compact -- append 4, compact(3), only entries 3 and 4 remain
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, Compact) {
    Hash key = make_key(0x06);
    std::vector<uint8_t> d1 = {0x01};
    std::vector<uint8_t> d2 = {0x02};
    std::vector<uint8_t> d3 = {0x03};
    std::vector<uint8_t> d4 = {0x04};

    repl_log_->append(key, Op::ADD, 0x00,d1);
    repl_log_->append(key, Op::ADD, 0x00,d2);
    repl_log_->append(key, Op::ADD, 0x00,d3);
    repl_log_->append(key, Op::ADD, 0x00,d4);

    // Compact: delete entries with seq < 3 (entries 1 and 2)
    repl_log_->compact(key, 3);

    auto entries = repl_log_->entries_after(key, 0);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].seq, 3u);
    EXPECT_EQ(entries[0].data, d3);
    EXPECT_EQ(entries[1].seq, 4u);
    EXPECT_EQ(entries[1].data, d4);
}

// ---------------------------------------------------------------------------
// Test 8: SerializeDeserializeEntry -- round-trip
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, SerializeDeserializeEntry) {
    LogEntry original;
    original.seq = 42;
    original.op = Op::UPD;
    original.timestamp = 1708000000;
    original.data_type = 0x02;
    original.data = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

    auto serialized = serialize_entry(original);
    auto deserialized = deserialize_entry(serialized);

    EXPECT_EQ(deserialized.seq, original.seq);
    EXPECT_EQ(deserialized.op, original.op);
    EXPECT_EQ(deserialized.timestamp, original.timestamp);
    EXPECT_EQ(deserialized.data_type, original.data_type);
    EXPECT_EQ(deserialized.data, original.data);
}

// ---------------------------------------------------------------------------
// Test 9: CompactWithTimeFloor -- time-based floor preserves recent entries
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, CompactWithTimeFloor) {
    Hash key = make_key(0xCC);

    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Create 5 old entries (8 days ago) with seqs 1-5
    std::vector<LogEntry> old_entries;
    for (uint64_t i = 1; i <= 5; ++i) {
        LogEntry e;
        e.seq = i;
        e.op = Op::ADD;
        e.timestamp = now_ms - 8ULL * 24 * 3600 * 1000;  // 8 days ago
        e.data_type = 0x02;
        e.data = {0x01, 0x02};
        old_entries.push_back(e);
    }
    repl_log_->apply(key, old_entries);

    // Create 5 recent entries (1 hour ago) with seqs 6-10
    std::vector<LogEntry> recent_entries;
    for (uint64_t i = 6; i <= 10; ++i) {
        LogEntry e;
        e.seq = i;
        e.op = Op::ADD;
        e.timestamp = now_ms - 3600ULL * 1000;  // 1 hour ago
        e.data_type = 0x02;
        e.data = {0x03, 0x04};
        recent_entries.push_back(e);
    }
    repl_log_->apply(key, recent_entries);

    EXPECT_EQ(repl_log_->current_seq(key), 10u);

    // Compact with time floor of 7 days:
    // before_seq = 8 (would delete seqs 1-7 based on count alone)
    // before_timestamp = 7 days ago
    uint64_t before_seq = 8;
    uint64_t before_timestamp = now_ms - 7ULL * 24 * 3600 * 1000;  // 7 days ago

    repl_log_->compact(key, before_seq, before_timestamp);

    // Entries 1-5 (old AND below seq threshold) -> DELETED
    // Entries 6-7 (recent, below seq threshold) -> PRESERVED by time floor
    // Entries 8-10 (above seq threshold) -> PRESERVED
    auto remaining = repl_log_->entries_after(key, 0);

    EXPECT_EQ(remaining.size(), 5u);  // seqs 6, 7, 8, 9, 10

    for (const auto& e : remaining) {
        EXPECT_GE(e.seq, 6u);
    }
}

// ---------------------------------------------------------------------------
// Test 10: CompactWithTimeFloorDeletesAll -- when all entries are old
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, CompactWithTimeFloorDeletesAll) {
    Hash key = make_key(0xDD);

    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Create 5 old entries (10 days ago)
    std::vector<LogEntry> entries;
    for (uint64_t i = 1; i <= 5; ++i) {
        LogEntry e;
        e.seq = i;
        e.op = Op::ADD;
        e.timestamp = now_ms - 10ULL * 24 * 3600 * 1000;  // 10 days ago
        e.data_type = 0x02;
        e.data = {0xAA};
        entries.push_back(e);
    }
    repl_log_->apply(key, entries);

    // Compact: before_seq=4 (delete seqs 1-3), before_timestamp=7 days ago
    uint64_t before_seq = 4;
    uint64_t before_timestamp = now_ms - 7ULL * 24 * 3600 * 1000;

    repl_log_->compact(key, before_seq, before_timestamp);

    // Entries 1-3 are old AND below seq threshold -> DELETED
    // Entries 4-5 are above seq threshold -> PRESERVED
    auto remaining = repl_log_->entries_after(key, 0);
    EXPECT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0].seq, 4u);
    EXPECT_EQ(remaining[1].seq, 5u);
}

// ---------------------------------------------------------------------------
// Test 11: CompactWithTimeFloorPreservesAll -- when all entries are recent
// ---------------------------------------------------------------------------

TEST_F(ReplLogTest, CompactWithTimeFloorPreservesAll) {
    Hash key = make_key(0xEE);

    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Create 5 recent entries (1 hour ago)
    std::vector<LogEntry> entries;
    for (uint64_t i = 1; i <= 5; ++i) {
        LogEntry e;
        e.seq = i;
        e.op = Op::ADD;
        e.timestamp = now_ms - 3600ULL * 1000;  // 1 hour ago
        e.data_type = 0x02;
        e.data = {0xBB};
        entries.push_back(e);
    }
    repl_log_->apply(key, entries);

    // Compact: before_seq=4 (would delete seqs 1-3), but time floor is 7 days ago
    uint64_t before_seq = 4;
    uint64_t before_timestamp = now_ms - 7ULL * 24 * 3600 * 1000;

    repl_log_->compact(key, before_seq, before_timestamp);

    // All entries are recent (1 hour ago > 7 days ago) -> ALL PRESERVED
    auto remaining = repl_log_->entries_after(key, 0);
    EXPECT_EQ(remaining.size(), 5u);
}
