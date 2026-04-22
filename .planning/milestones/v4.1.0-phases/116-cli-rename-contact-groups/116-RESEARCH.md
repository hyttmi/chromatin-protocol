# Phase 116: CLI Rename + Contact Groups - Research

**Researched:** 2026-04-16
**Domain:** C++ CLI tool rename, SQLite schema migration, contact group CRUD
**Confidence:** HIGH

## Summary

Phase 116 has two distinct workstreams: (1) renaming the CLI executable from `chromatindb-cli` to `cdb` with associated path/string changes, and (2) implementing contact groups with `--share @groupname` workflow plus contact import/export and schema versioning. Both workstreams operate entirely within the `cli/` directory and its SQLite contact database.

The rename is a mechanical search-and-replace across ~15 files with two critical exceptions: the HKDF label `chromatindb-envelope-kek-v1` in `envelope.cpp` is a cryptographic constant that MUST NOT be renamed (would break decryption of all existing envelopes), and the UDS path `/run/chromatindb/node.sock` stays unchanged per D-05 (the node binary is still chromatindb). The C++ namespace `chromatindb::cli` is an internal implementation detail of the broader chromatindb project and is NOT part of the rename scope -- only user-facing strings and build artifacts change.

The group feature extends the existing `ContactDB` class with two new normalized tables (`groups`, `group_members`) behind a schema migration system. The `--share` argument resolution needs to be extended to handle `@groupname` syntax, expanding group members into their KEM public keys for envelope encryption. All patterns (SQLite prepared statements, if/else command dispatch, subcommand parsing) are already established in the codebase and should be followed exactly.

**Primary recommendation:** Split into two plans -- Plan 01 for the rename (mechanical, high-confidence) and Plan 02 for groups + import/export + schema migration (new feature code). The rename must come first because the group feature should be built on top of the renamed binary.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** CMake target renamed from `chromatindb-cli` to `cdb`. CMake project name also changes to `cdb`.
- **D-02:** No backward-compatibility symlink. Clean break -- no trace of `chromatindb-cli`.
- **D-03:** Version string changes to `cdb 1.0.0` (was `chromatindb-cli 0.1.0`).
- **D-04:** Data directory changes from `~/.chromatindb/` to `~/.cdb/`.
- **D-05:** UDS path stays `/run/chromatindb/node.sock` -- the node binary hasn't changed.
- **D-06:** All usage/help strings, include paths, and test references updated. Full sweep.
- **D-07:** Normalized SQLite tables: `groups` (name TEXT PK) + `group_members` (group_name TEXT FK, contact_name TEXT FK). Many-to-many relationship.
- **D-08:** `group_members` has ON DELETE CASCADE for both foreign keys. Deleting a contact removes them from all groups. Deleting a group removes all its memberships.
- **D-09:** `--share` on both `put` and `reshare` resolves: `@groupname` -> group members, bare `name` -> contact lookup, file path -> fallback. Resolution order: @group first, then contact name, then file path.
- **D-10:** Error and abort if `--share @group` resolves to 0 members (empty group).
- **D-11:** Error if resolved contact has no kem_pk: "contact X has no encryption key".
- **D-12:** `group add` accepts contact names only (not namespace hex). Contacts must already exist in DB.
- **D-13:** `cdb group create <name>` -- create empty group.
- **D-14:** `cdb group add <group> <contact>...` -- add contacts to group.
- **D-15:** `cdb group rm <group>` -- delete entire group. `cdb group rm <group> <contact>` -- remove contact from group.
- **D-16:** `cdb group list` -- all groups with member counts. `cdb group list <name>` -- members of specific group.
- **D-17:** No command aliases (`delete`, etc.). `rm` is the verb, consistent with `contact rm` and `cdb rm`.
- **D-18:** Import format: JSON array of `{"name": "...", "namespace": "...64hex..."}`. Pubkeys fetched from node during import.
- **D-19:** Upsert on duplicate contact names (same INSERT OR REPLACE behavior as ContactDB::add).
- **D-20:** Skip failed pubkey fetches, continue importing. Print summary at end: "Imported N/M contacts. Failed: X (reason)".
- **D-21:** Add `cdb contact export` that dumps contacts to JSON in the same format as import (round-trip).
- **D-22:** `schema_version` table with single row: `CREATE TABLE schema_version (version INTEGER NOT NULL)`.
- **D-23:** On database open: check if `schema_version` exists. If not, create it with version=0, then run all migrations.
- **D-24:** Migrations run sequentially (0->1->2->...) with per-migration transactions (BEGIN/COMMIT per step).
- **D-25:** Version 1: existing contacts table (already present). Version 2: add groups + group_members tables.

### Claude's Discretion
- Where `--share` resolution logic lives (main.cpp vs commands.cpp)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| ERGO-01 | CLI executable is `cdb` as primary name (not symlink) | D-01 through D-06: CMake target rename, version bump, path changes, full string sweep. 47 occurrences of `chromatindb-cli` and `chromatindb` in cli/ files identified. |
| CONT-01 | User can create named contact groups | D-07, D-13: `groups` table with name TEXT PK. `cdb group create <name>` command. |
| CONT-02 | User can add/remove contacts from groups | D-07, D-08, D-12, D-14, D-15: `group_members` join table with CASCADE. `group add`/`group rm` commands. |
| CONT-03 | User can share with a group in one flag | D-09, D-10, D-11: `--share @groupname` resolution in `load_recipient_kem_pubkeys()`, expanding to member KEM pubkeys. |
| CONT-04 | User can import contacts from a JSON file | D-18, D-19, D-20, D-21: JSON array import with node pubkey fetch, upsert semantics, skip-on-failure with summary. Plus export for round-trip. |
| CONT-05 | SQLite schema versioning | D-22, D-23, D-24, D-25: `schema_version` table, bootstrap detection, sequential per-migration transactions, version 1=contacts, version 2=groups. |
</phase_requirements>

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Executable rename | Build System (CMake) | CLI Source | CMake target name drives binary name; source files contain user-facing strings |
| Data directory path | CLI Source | -- | `default_identity_dir()` in main.cpp is the single source of truth |
| Schema migration | Database / Storage | CLI Source | SQLite schema changes happen at DB open time; migration code lives in ContactDB |
| Group CRUD | CLI Source | Database / Storage | Command dispatch in main.cpp, business logic in contacts.cpp, data in SQLite |
| Share resolution | CLI Source | -- | `load_recipient_kem_pubkeys()` in commands.cpp resolves names/@groups to KEM pubkeys |
| Contact import/export | CLI Source | API / Backend | JSON parsing is local; pubkey fetch requires node connection |

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| SQLite3 | 3.53.0 (system) | Contact/group storage | Already linked via pkg-config, raw C API used throughout [VERIFIED: pkg-config] |
| nlohmann/json | 3.11.3 | JSON import/export parsing | Already a FetchContent dependency [VERIFIED: cli/CMakeLists.txt] |
| Catch2 | 3.7.1 | Unit tests | Already a FetchContent dependency for test target [VERIFIED: cli/CMakeLists.txt] |

### Supporting
No new dependencies needed. All functionality is implementable with existing linked libraries.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Raw sqlite3 API | SQLite ORM (sqlpp11, etc.) | Project uses raw API everywhere -- consistency > convenience |
| nlohmann/json | rapidjson, simdjson | Already linked, JSON payloads are tiny (contact lists) |

## Architecture Patterns

### System Architecture Diagram

```
User CLI Input
    |
    v
[main.cpp: argument parsing]
    |
    +-- "version" -> print "cdb 1.0.0"
    +-- "group create/add/rm/list" -> [cmd::group_*()]
    |                                       |
    |                                       v
    |                              [ContactDB: SQLite]
    |                              groups + group_members
    |
    +-- "put --share @team" -> [resolve_share_args()]
    |                               |
    |                               +-- starts with '@' -> ContactDB.group_members()
    |                               +-- contact name -> ContactDB.get()
    |                               +-- file path -> Identity::load_public_keys()
    |                               |
    |                               v
    |                          [KEM pubkey list]
    |                               |
    |                               v
    |                          [envelope::encrypt()]
    |                               |
    |                               v
    |                          [Connection -> Node]
    |
    +-- "contact import" -> [JSON parse] -> [contact_add() per entry] -> [Node: fetch PUBK]
    +-- "contact export" -> [ContactDB.list()] -> [JSON dump]
```

### Recommended Project Structure
```
cli/
├── CMakeLists.txt           # target: cdb (was chromatindb-cli)
├── README.md                # Updated references
├── src/
│   ├── main.cpp             # Updated: usage(), version, default_identity_dir()
│   ├── commands.h           # New: group_create/add/rm/list, contact_import/export
│   ├── commands.cpp          # New: group + import/export implementations
│   ├── contacts.h           # Extended: group methods, schema migration
│   ├── contacts.cpp         # Extended: group CRUD, schema versioning
│   ├── identity.h           # Unchanged (internal)
│   ├── identity.cpp         # Unchanged (internal)
│   ├── envelope.h           # Unchanged (HKDF label stays!)
│   ├── envelope.cpp         # Unchanged (HKDF label stays!)
│   ├── wire.h               # Unchanged (internal)
│   ├── wire.cpp             # Unchanged (internal)
│   ├── connection.h         # Unchanged (UDS path stays per D-05)
│   └── connection.cpp       # Unchanged
└── tests/
    ├── CMakeLists.txt       # Updated: comment + add test_contacts.cpp
    ├── test_contacts.cpp    # NEW: group CRUD + schema migration tests
    ├── test_identity.cpp    # Updated: namespace references
    ├── test_wire.cpp        # Updated: namespace references
    └── test_envelope.cpp    # Updated: namespace references
```

### Pattern 1: Command Dispatch (existing pattern to follow)
**What:** if/else chain in main.cpp matching command string, with subcommand parsing via `subcmd` variable.
**When to use:** Adding `group` command block.
**Example:**
```cpp
// Source: cli/src/main.cpp lines 769-809 (contact command pattern)
if (command == "group") {
    if (is_help_flag(argc, argv, arg_idx)) {
        std::fprintf(stderr, "Usage: cdb group create <name>\n"
                     "       cdb group add <group> <contact>...\n"
                     "       cdb group rm <group> [<contact>]\n"
                     "       cdb group list [<group>]\n");
        return 0;
    }
    if (arg_idx >= argc) {
        std::fprintf(stderr, "Usage: group <create|add|rm|list> ...\n");
        return 1;
    }
    std::string subcmd = argv[arg_idx++];
    // ... dispatch to cmd::group_create(), etc.
}
```

### Pattern 2: SQLite Prepared Statements (existing pattern)
**What:** Raw `sqlite3_prepare_v2` / `bind` / `step` / `finalize` pattern.
**When to use:** All new database operations.
**Example:**
```cpp
// Source: cli/src/contacts.cpp lines 37-59 (ContactDB::add pattern)
const char* sql = "INSERT INTO group_members (group_name, contact_name) VALUES (?, ?)";
sqlite3_stmt* stmt = nullptr;
if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("Failed to prepare insert");
}
sqlite3_bind_text(stmt, 1, group_name.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 2, contact_name.c_str(), -1, SQLITE_TRANSIENT);
sqlite3_step(stmt);
sqlite3_finalize(stmt);
```

### Pattern 3: Schema Migration
**What:** Sequential version-based migrations with per-step transactions.
**When to use:** `ContactDB` constructor, replacing current `init_schema()`.
**Example:**
```cpp
// Replace init_schema() with migrate()
void ContactDB::migrate() {
    // Check if schema_version table exists
    const char* check = "SELECT name FROM sqlite_master WHERE type='table' AND name='schema_version'";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, check, -1, &stmt, nullptr);
    bool has_version = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    int current_version = 0;
    if (!has_version) {
        // Bootstrap: create schema_version with version=0
        sqlite3_exec(db_, "CREATE TABLE schema_version (version INTEGER NOT NULL)", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "INSERT INTO schema_version (version) VALUES (0)", nullptr, nullptr, nullptr);
        // Check if contacts table already exists (pre-migration DB)
        // If so, set version=1 to skip migration 1
    } else {
        // Read current version
        sqlite3_prepare_v2(db_, "SELECT version FROM schema_version", -1, &stmt, nullptr);
        sqlite3_step(stmt);
        current_version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    // Run migrations sequentially
    if (current_version < 1) run_migration_1();  // contacts table
    if (current_version < 2) run_migration_2();  // groups + group_members
}
```

### Anti-Patterns to Avoid
- **Renaming HKDF_LABEL:** The `chromatindb-envelope-kek-v1` string in `envelope.cpp` is a cryptographic constant used in key derivation. Changing it would make all existing encrypted envelopes undecryptable. NEVER rename this.
- **Renaming C++ namespaces:** The `chromatindb::cli` namespace is internal to the codebase. The rename is about the user-facing binary name, not internal code organization. Renaming namespaces would be churn with zero user benefit.
- **Renaming UDS path:** Per D-05, the UDS path `/run/chromatindb/node.sock` stays because it refers to the node, not the CLI.
- **Opening ContactDB multiple times:** When resolving `--share @group`, do not open a separate ContactDB connection. The `load_recipient_kem_pubkeys()` function should receive a reference to an already-open ContactDB (or open one and use it for all resolutions).
- **Missing FOREIGN KEY enforcement:** SQLite has foreign keys disabled by default. Must execute `PRAGMA foreign_keys = ON` before any operation that depends on CASCADE behavior.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| JSON parsing | Manual string parsing | nlohmann/json (already linked) | Handles escaping, unicode, nested structures |
| Schema migration framework | Complex migration registry | Sequential version check + hardcoded migration functions | Only 2 migrations total, framework is overkill |
| Group membership queries | Application-level join logic | SQLite JOINs with prepared statements | Let the database handle relational queries |

**Key insight:** This phase adds ~300-400 lines of C++ to existing patterns. No new dependencies, no architectural changes.

## Runtime State Inventory

This phase includes a rename (`chromatindb-cli` -> `cdb`, `~/.chromatindb/` -> `~/.cdb/`), so runtime state must be inventoried.

| Category | Items Found | Action Required |
|----------|-------------|------------------|
| Stored data | `~/.chromatindb/` directory with identity keys (identity.key, identity.pub, identity.kem, identity.kpub) -- exists on dev machine | **User action**: manually `mv ~/.chromatindb ~/.cdb` after build. NOT a code task -- the CLI just reads from the new default path. |
| Live service config | Node at 192.168.1.73 has its own config; UDS path stays `/run/chromatindb/node.sock` per D-05 | None -- node is not renamed |
| OS-registered state | None -- CLI is not registered as a service or scheduled task | None |
| Secrets/env vars | No env vars reference `chromatindb-cli` by name. Identity keys are file-based. | None -- files just need to be in `~/.cdb/` instead of `~/.chromatindb/` |
| Build artifacts | `build/chromatindb-cli` binary and `build/cdb` symlink will be replaced by just `build/cdb` on next build. Old build directory should be cleaned. | `rm -rf build && mkdir build` before rebuilding |

**Critical note on data directory migration:** The code change is just updating `default_identity_dir()` from `".chromatindb"` to `".cdb"`. Existing users must manually move their data directory. Since this is a single-deployment project (per REQUIREMENTS.md: "Single local network deployment, not needed" for backward compatibility), this is acceptable. The plan should include a verification step that tests with `~/.cdb/`.

## Common Pitfalls

### Pitfall 1: SQLite Foreign Keys Off By Default
**What goes wrong:** `ON DELETE CASCADE` silently does nothing. Deleting a contact leaves orphaned group_members rows.
**Why it happens:** SQLite disables foreign key enforcement by default for backward compatibility. Must execute `PRAGMA foreign_keys = ON` on every connection.
**How to avoid:** Add `PRAGMA foreign_keys = ON` immediately after `sqlite3_open()` in `ContactDB` constructor, before any other SQL.
**Warning signs:** `group_members` rows survive after `contact rm`.

### Pitfall 2: Schema Migration Race with Existing Databases
**What goes wrong:** Existing `contacts.db` files have a `contacts` table but no `schema_version` table. If migration 1 tries to CREATE TABLE contacts, it fails or creates duplicates.
**Why it happens:** The current `init_schema()` uses `CREATE TABLE IF NOT EXISTS contacts`. The new migration system must detect this pre-existing state.
**How to avoid:** During bootstrap (no `schema_version` table), check if `contacts` table already exists in `sqlite_master`. If yes, set initial version to 1 (skip migration 1). Then run remaining migrations.
**Warning signs:** `SQLITE_ERROR: table contacts already exists` during migration.

### Pitfall 3: Incomplete Rename Leaves Ghost References
**What goes wrong:** Some usage string or help text still says `chromatindb-cli` after rename.
**Why it happens:** Mechanical search-and-replace misses conditional paths or concatenated strings.
**How to avoid:** After rename, grep the entire `cli/` directory for `chromatindb-cli` (exact match). Also grep for `chromatindb` excluding the UDS path and the HKDF label to catch any remaining references that should have changed.
**Warning signs:** `grep -r "chromatindb-cli" cli/` returns any results.

### Pitfall 4: Group Resolution in --share Breaks File Path Fallback
**What goes wrong:** A file named `@team` (unlikely but possible) is treated as a group reference instead of a file path.
**Why it happens:** The resolution order D-09 specifies: @group first, then contact name, then file path. If `@team` is checked as a group first, it never reaches the file path fallback.
**How to avoid:** This is actually the correct behavior per D-09. The `@` prefix is an unambiguous indicator for group lookup. Document that file names starting with `@` need a path prefix (`./`).
**Warning signs:** N/A -- this is by design.

### Pitfall 5: Import Fails Silently on Invalid JSON
**What goes wrong:** `cdb contact import` with malformed JSON prints no useful error.
**Why it happens:** nlohmann/json throws on parse failure; if caught generically, the error message is unhelpful.
**How to avoid:** Validate JSON structure explicitly: must be an array, each element must have `name` (string) and `namespace` (string, 64 hex chars). Report specific validation errors.
**Warning signs:** Import reports "0/0 contacts imported" with no error detail.

## Code Examples

### Share Resolution with @group Support
```cpp
// Recommendation: extend load_recipient_kem_pubkeys() in commands.cpp
// This is Claude's discretion area -- keeping it in commands.cpp
// because it already handles contact name -> pubkey resolution.

static std::vector<std::vector<uint8_t>> load_recipient_kem_pubkeys(
    const std::vector<std::string>& share_args,
    const std::string& identity_dir) {

    std::vector<std::vector<uint8_t>> kem_pks;
    auto db_path = identity_dir + "/contacts.db";
    ContactDB db(db_path);

    for (const auto& arg : share_args) {
        if (arg.size() > 1 && arg[0] == '@') {
            // Group resolution: @groupname
            std::string group_name = arg.substr(1);
            auto members = db.group_members(group_name);
            if (members.empty()) {
                throw std::runtime_error("Group '@" + group_name + "' is empty or does not exist");
            }
            for (const auto& contact : members) {
                if (contact.kem_pk.empty()) {
                    throw std::runtime_error("contact " + contact.name + " has no encryption key");
                }
                kem_pks.push_back(contact.kem_pk);
            }
        } else if (fs::exists(arg)) {
            // File path: load pubkey from file
            auto data = read_file_bytes(arg);
            auto [signing_pk, kem_pk] = Identity::load_public_keys(data);
            kem_pks.push_back(std::move(kem_pk));
        } else {
            // Contact name lookup
            auto contact = db.get(arg);
            if (!contact) {
                throw std::runtime_error("Unknown contact or file: " + arg);
            }
            if (contact->kem_pk.empty()) {
                throw std::runtime_error("contact " + contact->name + " has no encryption key");
            }
            kem_pks.push_back(contact->kem_pk);
        }
    }
    return kem_pks;
}
```

### Contact Import Command
```cpp
// Source pattern: commands.cpp contact_add() for pubkey fetch logic
int contact_import(const std::string& identity_dir, const std::string& json_path,
                   const ConnectOpts& opts) {
    // 1. Read and parse JSON file
    auto data = read_file_bytes(json_path);
    auto json = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
    if (json.is_discarded() || !json.is_array()) {
        std::fprintf(stderr, "Error: expected JSON array\n");
        return 1;
    }

    auto id = Identity::load_from(identity_dir);
    auto db_path = identity_dir + "/contacts.db";
    int imported = 0, failed = 0;

    for (const auto& entry : json) {
        // Validate entry
        if (!entry.contains("name") || !entry["name"].is_string() ||
            !entry.contains("namespace") || !entry["namespace"].is_string()) {
            ++failed;
            continue;
        }
        std::string name = entry["name"];
        std::string ns = entry["namespace"];

        // Reuse contact_add logic: fetch PUBK from node
        // ... (fetch pubkey, save to contacts.db)
    }

    std::fprintf(stderr, "Imported %d/%zu contacts. Failed: %d\n",
                 imported, json.size(), failed);
    return (failed > 0 && imported == 0) ? 1 : 0;
}
```

### Schema Migration Bootstrap
```cpp
// Source: replaces contacts.cpp init_schema()
void ContactDB::migrate() {
    // CRITICAL: Enable foreign keys before anything else
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);

    // Check for schema_version table
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
        char* err = nullptr;
        sqlite3_exec(db_, "CREATE TABLE schema_version (version INTEGER NOT NULL)",
                     nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); throw std::runtime_error("Failed to create schema_version"); }
        sqlite3_exec(db_, "INSERT INTO schema_version (version) VALUES (0)",
                     nullptr, nullptr, nullptr);

        // Detect pre-existing contacts table
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
        const char* sql = "CREATE TABLE contacts ("
                          "name TEXT PRIMARY KEY,"
                          "namespace_hex TEXT NOT NULL,"
                          "signing_pk BLOB NOT NULL,"
                          "kem_pk BLOB NOT NULL)";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("Migration 1 failed: " + msg);
        }
        sqlite3_exec(db_, "UPDATE schema_version SET version = 1", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }

    // Migration 2: groups + group_members
    if (current_version < 2) {
        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        const char* sql =
            "CREATE TABLE groups (name TEXT PRIMARY KEY);"
            "CREATE TABLE group_members ("
            "  group_name TEXT NOT NULL REFERENCES groups(name) ON DELETE CASCADE,"
            "  contact_name TEXT NOT NULL REFERENCES contacts(name) ON DELETE CASCADE,"
            "  PRIMARY KEY (group_name, contact_name)"
            ")";
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("Migration 2 failed: " + msg);
        }
        sqlite3_exec(db_, "UPDATE schema_version SET version = 2", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `chromatindb-cli` binary | `cdb` binary | This phase | All usage docs, help strings, CMake target |
| `~/.chromatindb/` data dir | `~/.cdb/` data dir | This phase | Default identity path changes |
| `CREATE TABLE IF NOT EXISTS` schema | Version-tracked migrations | This phase | Future schema changes are safe |
| Contacts only | Contacts + Groups | This phase | `--share @group` workflow enabled |

## Rename Scope Inventory

### MUST Rename (user-facing, build artifacts)
| File | What Changes | Count |
|------|-------------|-------|
| `cli/CMakeLists.txt` | `project(chromatindb-cli ...)` -> `project(cdb ...)`, target name, symlink removed | 7 occurrences |
| `cli/src/main.cpp` | VERSION string, `default_identity_dir()`, all usage() strings | ~20 occurrences |
| `cli/src/contacts.h` | Comment: `~/.chromatindb/contacts.db` -> `~/.cdb/contacts.db` | 1 occurrence |
| `cli/README.md` | All command examples, config path references | ~15 occurrences |
| `cli/tests/CMakeLists.txt` | Comment referencing `chromatindb-cli` | 1 occurrence |

### MUST NOT Rename (cryptographic constants, node references, internal namespaces)
| File | What Stays | Reason |
|------|-----------|--------|
| `cli/src/envelope.cpp` | `chromatindb-envelope-kek-v1` HKDF label | Cryptographic constant -- changing breaks all existing envelopes |
| `cli/src/commands.h` | `/run/chromatindb/node.sock` UDS path | Per D-05: node binary unchanged |
| All .cpp/.h files | `chromatindb::cli` C++ namespace | Internal implementation, not user-facing |
| `cli/src/connection.h` | `chromatindb` in comment "connection to a chromatindb node" | Describes the node, not the CLI |

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | C++ namespace `chromatindb::cli` should NOT be renamed -- only user-facing strings change | Rename Scope | If user intended full namespace rename, ~80 additional lines change across all files + tests. Low risk -- D-06 says "include paths and test references" not "C++ namespaces". |
| A2 | Existing `~/.chromatindb/` data migration is a manual user step, not automated in code | Runtime State | If user expects automatic migration (detect old dir, move to new), code needs a migration path in `default_identity_dir()`. Low risk -- single deployment. |
| A3 | The `--share` resolution logic should live in `commands.cpp` (Claude's discretion per CONTEXT.md) | Architecture | If placed in main.cpp instead, the resolution would need to happen before `cmd::put()` is called, requiring refactoring of the put function signature. Medium risk -- either location works, but commands.cpp is where `load_recipient_kem_pubkeys()` already lives. |

## Open Questions

1. **Should `cdb` auto-detect and migrate `~/.chromatindb/` to `~/.cdb/`?**
   - What we know: D-04 changes the default path. Existing identity at `~/.chromatindb/`.
   - What's unclear: Whether the CLI should detect the old path and offer to migrate (or just use it).
   - Recommendation: Do NOT auto-migrate. Print a one-time hint if `~/.cdb/` doesn't exist but `~/.chromatindb/` does: `"Hint: mv ~/.chromatindb ~/.cdb"`. This is consistent with YAGNI and clean-break philosophy.

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| CMake | Build system | Yes | 4.3.1 | -- |
| g++ (C++20) | Compilation | Yes | 15.2.1 | -- |
| SQLite3 | Contact/group storage | Yes | 3.53.0 | -- |
| pkg-config | Dependency resolution | Yes | -- | -- |
| Catch2 | Unit tests | Yes (FetchContent) | 3.7.1 | -- |

**Missing dependencies:** None. All tools available.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 3.7.1 (FetchContent) |
| Config file | `cli/tests/CMakeLists.txt` |
| Quick run command | `cd build/cli && ctest --test-dir tests -R contacts --output-on-failure` |
| Full suite command | `cd build/cli && ctest --test-dir tests --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| ERGO-01 | Binary is named `cdb` | build verification | `ls build/cli/cdb && ! ls build/cli/chromatindb-cli 2>/dev/null` | N/A (build check) |
| CONT-01 | Create group | unit | `ctest --test-dir build/cli/tests -R group_create --output-on-failure` | Wave 0 |
| CONT-02 | Add/remove group members | unit | `ctest --test-dir build/cli/tests -R group_add --output-on-failure` | Wave 0 |
| CONT-03 | Share resolution with @group | unit | `ctest --test-dir build/cli/tests -R share_resolve --output-on-failure` | Wave 0 |
| CONT-04 | Contact import/export | unit | `ctest --test-dir build/cli/tests -R contact_import --output-on-failure` | Wave 0 |
| CONT-05 | Schema versioning | unit | `ctest --test-dir build/cli/tests -R schema_version --output-on-failure` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build/cli && ctest --test-dir tests --output-on-failure`
- **Per wave merge:** Full suite including existing identity/wire/envelope tests
- **Phase gate:** All tests green + `build/cli/cdb version` outputs `cdb 1.0.0`

### Wave 0 Gaps
- [ ] `cli/tests/test_contacts.cpp` -- covers CONT-01 through CONT-05 (group CRUD, schema migration, import/export)
- [ ] Update `cli/tests/CMakeLists.txt` -- add `test_contacts.cpp` source, link `PkgConfig::sqlite3`

## Security Domain

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | No | N/A (no user auth in CLI) |
| V3 Session Management | No | N/A |
| V4 Access Control | No | N/A (namespace-level, handled by node) |
| V5 Input Validation | Yes | Validate group names, contact names, JSON structure, namespace hex format |
| V6 Cryptography | No change | HKDF label MUST NOT be renamed. Envelope encryption unchanged. |

### Known Threat Patterns for SQLite CLI

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| SQL injection via group/contact names | Tampering | Prepared statements with `sqlite3_bind_text()` -- already used throughout |
| Path traversal in import file path | Information Disclosure | `std::ifstream` with user-provided path -- acceptable for CLI tool (user runs as themselves) |
| Malformed JSON crash | Denial of Service | `nlohmann::json::parse(..., nullptr, false)` with `is_discarded()` check -- already used pattern |

## Sources

### Primary (HIGH confidence)
- `cli/CMakeLists.txt` -- build configuration, dependency versions, target name
- `cli/src/main.cpp` -- full command dispatch, usage strings, version constant
- `cli/src/contacts.h` / `contacts.cpp` -- ContactDB API, SQLite schema, CRUD patterns
- `cli/src/commands.cpp` -- `load_recipient_kem_pubkeys()` share resolution, `contact_add()` pubkey fetch pattern
- `cli/src/envelope.cpp` -- HKDF label constant (cryptographic, must not rename)
- `cli/src/commands.h` -- UDS path default
- `116-CONTEXT.md` -- all 25 locked decisions

### Secondary (MEDIUM confidence)
- SQLite documentation on `PRAGMA foreign_keys` -- foreign keys off by default [CITED: https://www.sqlite.org/foreignkeys.html]
- SQLite documentation on `sqlite3_exec()` for multi-statement execution [CITED: https://www.sqlite.org/c3ref/exec.html]

### Tertiary (LOW confidence)
None -- all claims verified against codebase.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all verified in CMakeLists.txt
- Architecture: HIGH -- all patterns extracted from existing codebase
- Pitfalls: HIGH -- foreign key pragma and schema bootstrap are well-documented SQLite behaviors

**Research date:** 2026-04-16
**Valid until:** 2026-05-16 (stable domain, no external API changes expected)
