#include <gtest/gtest.h>

#include "crypto/crypto.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

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

    uint64_t seq1 = repl_log_->append(key, Op::ADD, data1);
    uint64_t seq2 = repl_log_->append(key, Op::ADD, data2);
    uint64_t seq3 = repl_log_->append(key, Op::ADD, data3);

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
    repl_log_->append(key, Op::ADD, data);
    EXPECT_EQ(repl_log_->current_seq(key), 1u);

    repl_log_->append(key, Op::UPD, data);
    EXPECT_EQ(repl_log_->current_seq(key), 2u);

    repl_log_->append(key, Op::DEL, data);
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

    repl_log_->append(key, Op::ADD, d1);
    repl_log_->append(key, Op::ADD, d2);
    repl_log_->append(key, Op::ADD, d3);
    repl_log_->append(key, Op::ADD, d4);

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

    repl_log_->append(key, Op::ADD, d1);
    repl_log_->append(key, Op::ADD, d2);

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

    repl_log_->append(key, Op::ADD, d1);
    repl_log_->append(key, Op::ADD, d2);

    // Build entries to apply: seq 2 (duplicate) and seq 3 (new)
    LogEntry dup_entry;
    dup_entry.seq = 2;
    dup_entry.op = Op::ADD;
    dup_entry.timestamp = 1000;
    dup_entry.data = {0xA2};

    LogEntry new_entry;
    new_entry.seq = 3;
    new_entry.op = Op::ADD;
    new_entry.timestamp = 2000;
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

    uint64_t seq_a1 = repl_log_->append(key_a, Op::ADD, data);
    uint64_t seq_a2 = repl_log_->append(key_a, Op::ADD, data);
    uint64_t seq_b1 = repl_log_->append(key_b, Op::ADD, data);

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

    repl_log_->append(key, Op::ADD, d1);
    repl_log_->append(key, Op::ADD, d2);
    repl_log_->append(key, Op::ADD, d3);
    repl_log_->append(key, Op::ADD, d4);

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
    original.data = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

    auto serialized = serialize_entry(original);
    auto deserialized = deserialize_entry(serialized);

    EXPECT_EQ(deserialized.seq, original.seq);
    EXPECT_EQ(deserialized.op, original.op);
    EXPECT_EQ(deserialized.timestamp, original.timestamp);
    EXPECT_EQ(deserialized.data, original.data);
}
