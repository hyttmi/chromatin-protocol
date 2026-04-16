#include "cli/src/contacts.h"
#include "cli/src/wire.h"

#include <sqlite3.h>
#include <stdexcept>

namespace chromatindb::cli {

ContactDB::ContactDB(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error(std::string("Failed to open contacts db: ") +
                                 sqlite3_errmsg(db_));
    }
    migrate();
}

ContactDB::~ContactDB() {
    if (db_) sqlite3_close(db_);
}

void ContactDB::migrate() {
    // CRITICAL: Enable foreign keys before anything else (per Pitfall 1)
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);

    // Check if schema_version table exists
    bool has_version_table = false;
    {
        const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='schema_version'";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        has_version_table = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }

    int current_version = 0;
    if (!has_version_table) {
        // Bootstrap: create schema_version with version=0
        char* err = nullptr;
        if (sqlite3_exec(db_, "CREATE TABLE schema_version (version INTEGER NOT NULL)",
                         nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("Failed to create schema_version: " + msg);
        }
        sqlite3_exec(db_, "INSERT INTO schema_version (version) VALUES (0)",
                     nullptr, nullptr, nullptr);

        // Detect pre-existing contacts table (per Pitfall 2)
        const char* check = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='contacts'";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, check, -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            current_version = 1;  // Skip migration 1
            sqlite3_exec(db_, "UPDATE schema_version SET version = 1",
                         nullptr, nullptr, nullptr);
        }
        sqlite3_finalize(stmt);
    } else {
        // Read current version
        const char* sql = "SELECT version FROM schema_version";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            current_version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Migration 1: contacts table
    if (current_version < 1) {
        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        const char* sql =
            "CREATE TABLE contacts ("
            "  name TEXT PRIMARY KEY,"
            "  namespace_hex TEXT NOT NULL,"
            "  signing_pk BLOB NOT NULL,"
            "  kem_pk BLOB NOT NULL"
            ")";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("Migration 1 failed: " + msg);
        }
        sqlite3_exec(db_, "UPDATE schema_version SET version = 1",
                     nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }

    // Migration 2: groups + group_members
    if (current_version < 2) {
        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        const char* sql1 = "CREATE TABLE groups (name TEXT PRIMARY KEY)";
        const char* sql2 =
            "CREATE TABLE group_members ("
            "  group_name TEXT NOT NULL REFERENCES groups(name) ON DELETE CASCADE,"
            "  contact_name TEXT NOT NULL REFERENCES contacts(name) ON DELETE CASCADE,"
            "  PRIMARY KEY (group_name, contact_name)"
            ")";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql1, nullptr, nullptr, &err) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("Migration 2 failed (groups): " + msg);
        }
        if (sqlite3_exec(db_, sql2, nullptr, nullptr, &err) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("Migration 2 failed (group_members): " + msg);
        }
        sqlite3_exec(db_, "UPDATE schema_version SET version = 2",
                     nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }
}

void ContactDB::add(const std::string& name,
                     const std::vector<uint8_t>& signing_pk,
                     const std::vector<uint8_t>& kem_pk) {
    auto ns_hash = sha3_256(signing_pk);
    auto ns_hex = to_hex(std::span<const uint8_t>(ns_hash.data(), 32));

    const char* sql =
        "INSERT OR REPLACE INTO contacts (name, namespace_hex, signing_pk, kem_pk) "
        "VALUES (?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert");
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ns_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, signing_pk.data(),
                      static_cast<int>(signing_pk.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 4, kem_pk.data(),
                      static_cast<int>(kem_pk.size()), SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool ContactDB::remove(const std::string& name) {
    const char* sql = "DELETE FROM contacts WHERE name = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

std::optional<Contact> ContactDB::get(const std::string& name) const {
    const char* sql = "SELECT name, namespace_hex, signing_pk, kem_pk FROM contacts WHERE name = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    Contact c;
    c.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    c.namespace_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

    auto* spk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
    int spk_len = sqlite3_column_bytes(stmt, 2);
    c.signing_pk.assign(spk, spk + spk_len);

    auto* kpk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 3));
    int kpk_len = sqlite3_column_bytes(stmt, 3);
    c.kem_pk.assign(kpk, kpk + kpk_len);

    sqlite3_finalize(stmt);
    return c;
}

std::vector<Contact> ContactDB::list() const {
    const char* sql = "SELECT name, namespace_hex, signing_pk, kem_pk FROM contacts ORDER BY name";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};

    std::vector<Contact> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Contact c;
        c.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        c.namespace_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        auto* spk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
        int spk_len = sqlite3_column_bytes(stmt, 2);
        c.signing_pk.assign(spk, spk + spk_len);

        auto* kpk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 3));
        int kpk_len = sqlite3_column_bytes(stmt, 3);
        c.kem_pk.assign(kpk, kpk + kpk_len);

        result.push_back(std::move(c));
    }

    sqlite3_finalize(stmt);
    return result;
}

void ContactDB::group_create(const std::string& name) {
    const char* sql = "INSERT INTO groups (name) VALUES (?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare group create");
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Group already exists: " + name);
    }
}

bool ContactDB::group_remove(const std::string& name) {
    const char* sql = "DELETE FROM groups WHERE name = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

void ContactDB::group_add_member(const std::string& group, const std::string& contact) {
    // Verify contact exists
    auto c = get(contact);
    if (!c) {
        throw std::runtime_error("Contact not found: " + contact);
    }
    const char* sql = "INSERT OR IGNORE INTO group_members (group_name, contact_name) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare group add member");
    }
    sqlite3_bind_text(stmt, 1, group.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, contact.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to add " + contact + " to group " + group);
    }
}

bool ContactDB::group_remove_member(const std::string& group, const std::string& contact) {
    const char* sql = "DELETE FROM group_members WHERE group_name = ? AND contact_name = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, group.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, contact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes > 0;
}

std::vector<Contact> ContactDB::group_members(const std::string& group) const {
    const char* sql =
        "SELECT c.name, c.namespace_hex, c.signing_pk, c.kem_pk "
        "FROM contacts c "
        "INNER JOIN group_members gm ON gm.contact_name = c.name "
        "WHERE gm.group_name = ? "
        "ORDER BY c.name";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_text(stmt, 1, group.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<Contact> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Contact c;
        c.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        c.namespace_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto* spk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
        int spk_len = sqlite3_column_bytes(stmt, 2);
        c.signing_pk.assign(spk, spk + spk_len);
        auto* kpk = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 3));
        int kpk_len = sqlite3_column_bytes(stmt, 3);
        c.kem_pk.assign(kpk, kpk + kpk_len);
        result.push_back(std::move(c));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<std::pair<std::string, int>> ContactDB::group_list() const {
    const char* sql =
        "SELECT g.name, COUNT(gm.contact_name) "
        "FROM groups g "
        "LEFT JOIN group_members gm ON gm.group_name = g.name "
        "GROUP BY g.name "
        "ORDER BY g.name";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};

    std::vector<std::pair<std::string, int>> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        int count = sqlite3_column_int(stmt, 1);
        result.emplace_back(std::move(name), count);
    }
    sqlite3_finalize(stmt);
    return result;
}

} // namespace chromatindb::cli
