#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "cli/src/contacts.h"

#include <sqlite3.h>
#include <filesystem>
#include <random>

namespace fs = std::filesystem;

namespace {

/// RAII temp dir for tests.
struct ContactsTempDir {
    fs::path path;
    explicit ContactsTempDir(const std::string& prefix = "cli_contacts_test_") {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        path = fs::temp_directory_path() / (prefix + std::to_string(dist(gen)));
        fs::create_directories(path);
    }
    ~ContactsTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ContactsTempDir(const ContactsTempDir&) = delete;
    ContactsTempDir& operator=(const ContactsTempDir&) = delete;
};

// ML-DSA-87 signing pubkey is 2592 bytes, ML-KEM-1024 kem_pk is 1568 bytes
const std::vector<uint8_t> fake_spk(2592, 0x01);
const std::vector<uint8_t> fake_kpk(1568, 0x02);

// Second contact with different key data
const std::vector<uint8_t> fake_spk2(2592, 0x03);
const std::vector<uint8_t> fake_kpk2(1568, 0x04);

} // anonymous namespace

TEST_CASE("contacts: fresh database creates schema_version at version 2", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();

    chromatindb::cli::ContactDB db(db_path);

    // Query schema_version directly
    sqlite3* raw_db = nullptr;
    REQUIRE(sqlite3_open(db_path.c_str(), &raw_db) == SQLITE_OK);

    const char* sql = "SELECT version FROM schema_version";
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw_db, sql, -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    int version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(raw_db);

    REQUIRE(version == 2);
}

TEST_CASE("contacts: pre-existing contacts table bootstraps at version 1 then migrates", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();

    // Manually create a contacts table (simulating old-style init_schema)
    {
        sqlite3* raw_db = nullptr;
        REQUIRE(sqlite3_open(db_path.c_str(), &raw_db) == SQLITE_OK);
        const char* sql =
            "CREATE TABLE IF NOT EXISTS contacts ("
            "  name TEXT PRIMARY KEY,"
            "  namespace_hex TEXT NOT NULL,"
            "  signing_pk BLOB NOT NULL,"
            "  kem_pk BLOB NOT NULL"
            ")";
        REQUIRE(sqlite3_exec(raw_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw_db);
    }

    // Now open with ContactDB -- should detect existing table, set version=1, migrate to 2
    chromatindb::cli::ContactDB db(db_path);

    // Verify schema_version is 2
    sqlite3* raw_db = nullptr;
    REQUIRE(sqlite3_open(db_path.c_str(), &raw_db) == SQLITE_OK);

    const char* sql = "SELECT version FROM schema_version";
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw_db, sql, -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    int version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // Verify groups table exists
    const char* check = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='groups'";
    REQUIRE(sqlite3_prepare_v2(raw_db, check, -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    sqlite3_close(raw_db);

    REQUIRE(version == 2);
}

TEST_CASE("contacts: group create and list", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.group_create("engineering");
    auto groups = db.group_list();

    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].first == "engineering");
    REQUIRE(groups[0].second == 0);
}

TEST_CASE("contacts: group create duplicate throws", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.group_create("team");
    REQUIRE_THROWS_AS(db.group_create("team"), std::runtime_error);
}

TEST_CASE("contacts: group add member and list members", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.add("alice", fake_spk, fake_kpk);
    db.group_create("eng");
    db.group_add_member("eng", "alice");

    auto members = db.group_members("eng");
    REQUIRE(members.size() == 1);
    REQUIRE(members[0].name == "alice");
}

TEST_CASE("contacts: group add nonexistent contact throws", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.group_create("eng");
    REQUIRE_THROWS_WITH(db.group_add_member("eng", "nobody"),
                        "Contact not found: nobody");
}

TEST_CASE("contacts: group remove deletes group", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.group_create("temp");
    REQUIRE(db.group_remove("temp"));
    auto groups = db.group_list();
    REQUIRE(groups.empty());
}

TEST_CASE("contacts: group remove member", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.add("alice", fake_spk, fake_kpk);
    db.group_create("eng");
    db.group_add_member("eng", "alice");

    REQUIRE(db.group_remove_member("eng", "alice"));
    auto members = db.group_members("eng");
    REQUIRE(members.empty());
}

TEST_CASE("contacts: cascade delete contact removes from group", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.add("bob", fake_spk2, fake_kpk2);
    db.group_create("team");
    db.group_add_member("team", "bob");

    // Delete the contact -- cascade should remove from group_members
    db.remove("bob");
    auto members = db.group_members("team");
    REQUIRE(members.empty());
}

TEST_CASE("contacts: cascade delete group removes memberships", "[contacts]") {
    ContactsTempDir tmp;
    auto db_path = (tmp.path / "contacts.db").string();
    chromatindb::cli::ContactDB db(db_path);

    db.add("alice", fake_spk, fake_kpk);
    db.group_create("team");
    db.group_add_member("team", "alice");

    // Delete the group -- cascade should remove memberships
    db.group_remove("team");
    auto groups = db.group_list();
    REQUIRE(groups.empty());

    // Contact should still exist
    auto alice = db.get("alice");
    REQUIRE(alice.has_value());
    REQUIRE(alice->name == "alice");
}
