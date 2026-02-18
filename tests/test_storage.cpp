#include <gtest/gtest.h>

#include "storage/storage.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace chromatin::storage;

class StorageTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<Storage> store_;

    void SetUp() override {
        // Create a unique temporary directory for each test
        db_path_ = std::filesystem::temp_directory_path() /
                   ("chromatin_test_" + std::to_string(::testing::UnitTest::GetInstance()
                                                       ->current_test_info()
                                                       ->line()) +
                    "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(db_path_);
        store_ = std::make_unique<Storage>(db_path_ / "test.mdbx");
    }

    void TearDown() override {
        store_.reset();
        std::filesystem::remove_all(db_path_);
    }

    static std::vector<uint8_t> to_bytes(const std::string& s) {
        return {s.begin(), s.end()};
    }
};

TEST_F(StorageTest, PutAndGet) {
    auto key = to_bytes("key1");
    auto val = to_bytes("value1");
    ASSERT_TRUE(store_->put(TABLE_PROFILES, key, val));

    auto result = store_->get(TABLE_PROFILES, key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, val);
}

TEST_F(StorageTest, GetMissingKey) {
    auto key = to_bytes("nonexistent");
    auto result = store_->get(TABLE_PROFILES, key);
    EXPECT_FALSE(result.has_value());
}

TEST_F(StorageTest, Overwrite) {
    auto key = to_bytes("key1");
    auto val1 = to_bytes("first");
    auto val2 = to_bytes("second");

    ASSERT_TRUE(store_->put(TABLE_PROFILES, key, val1));
    ASSERT_TRUE(store_->put(TABLE_PROFILES, key, val2));

    auto result = store_->get(TABLE_PROFILES, key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, val2);
}

TEST_F(StorageTest, Delete) {
    auto key = to_bytes("key1");
    auto val = to_bytes("value1");
    ASSERT_TRUE(store_->put(TABLE_PROFILES, key, val));
    EXPECT_TRUE(store_->del(TABLE_PROFILES, key));

    auto result = store_->get(TABLE_PROFILES, key);
    EXPECT_FALSE(result.has_value());
}

TEST_F(StorageTest, DeleteMissingKey) {
    auto key = to_bytes("nonexistent");
    EXPECT_FALSE(store_->del(TABLE_PROFILES, key));
}

TEST_F(StorageTest, SeparateTables) {
    auto key = to_bytes("same_key");
    auto val_profiles = to_bytes("profile_data");
    auto val_names = to_bytes("name_data");

    ASSERT_TRUE(store_->put(TABLE_PROFILES, key, val_profiles));
    ASSERT_TRUE(store_->put(TABLE_NAMES, key, val_names));

    auto r1 = store_->get(TABLE_PROFILES, key);
    auto r2 = store_->get(TABLE_NAMES, key);

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r1, val_profiles);
    EXPECT_EQ(*r2, val_names);
    EXPECT_NE(*r1, *r2);
}

TEST_F(StorageTest, Foreach) {
    auto k1 = to_bytes("a");
    auto k2 = to_bytes("b");
    auto k3 = to_bytes("c");
    auto v1 = to_bytes("1");
    auto v2 = to_bytes("2");
    auto v3 = to_bytes("3");

    store_->put(TABLE_INBOXES, k1, v1);
    store_->put(TABLE_INBOXES, k2, v2);
    store_->put(TABLE_INBOXES, k3, v3);

    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
    store_->foreach(TABLE_INBOXES, [&](std::span<const uint8_t> key, std::span<const uint8_t> value) {
        entries.emplace_back(
            std::vector<uint8_t>(key.begin(), key.end()),
            std::vector<uint8_t>(value.begin(), value.end()));
        return true;
    });

    EXPECT_EQ(entries.size(), 3u);
}

TEST_F(StorageTest, ForeachEarlyStop) {
    auto k1 = to_bytes("a");
    auto k2 = to_bytes("b");
    auto k3 = to_bytes("c");
    auto v1 = to_bytes("1");
    auto v2 = to_bytes("2");
    auto v3 = to_bytes("3");

    store_->put(TABLE_INBOXES, k1, v1);
    store_->put(TABLE_INBOXES, k2, v2);
    store_->put(TABLE_INBOXES, k3, v3);

    int count = 0;
    store_->foreach(TABLE_INBOXES, [&](std::span<const uint8_t>, std::span<const uint8_t>) {
        ++count;
        return false; // stop after first entry
    });

    EXPECT_EQ(count, 1);
}

TEST_F(StorageTest, ForeachEmptyTable) {
    int count = 0;
    store_->foreach(TABLE_NODES, [&](std::span<const uint8_t>, std::span<const uint8_t>) {
        ++count;
        return true;
    });
    EXPECT_EQ(count, 0);
}
