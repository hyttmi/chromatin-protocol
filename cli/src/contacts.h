#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace chromatindb::cli {

struct Contact {
    std::string name;
    std::string namespace_hex;  // SHA3-256 of signing_pk
    std::vector<uint8_t> signing_pk;
    std::vector<uint8_t> kem_pk;
};

/// Local contact database backed by SQLite.
/// Stored at ~/.chromatindb/contacts.db
class ContactDB {
public:
    explicit ContactDB(const std::string& db_path);
    ~ContactDB();

    ContactDB(const ContactDB&) = delete;
    ContactDB& operator=(const ContactDB&) = delete;

    /// Add or update a contact.
    void add(const std::string& name,
             const std::vector<uint8_t>& signing_pk,
             const std::vector<uint8_t>& kem_pk);

    /// Remove a contact by name.
    bool remove(const std::string& name);

    /// Look up a contact by name.
    std::optional<Contact> get(const std::string& name) const;

    /// List all contacts.
    std::vector<Contact> list() const;

private:
    void init_schema();
    sqlite3* db_ = nullptr;
};

} // namespace chromatindb::cli
