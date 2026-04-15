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
    init_schema();
}

ContactDB::~ContactDB() {
    if (db_) sqlite3_close(db_);
}

void ContactDB::init_schema() {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS contacts ("
        "  name TEXT PRIMARY KEY,"
        "  namespace_hex TEXT NOT NULL,"
        "  signing_pk BLOB NOT NULL,"
        "  kem_pk BLOB NOT NULL"
        ")";
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("Failed to create contacts table: " + msg);
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

} // namespace chromatindb::cli
