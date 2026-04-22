---
phase: 116-cli-rename-contact-groups
reviewed: 2026-04-16T00:00:00Z
depth: standard
files_reviewed: 9
files_reviewed_list:
  - cli/CMakeLists.txt
  - cli/README.md
  - cli/src/commands.cpp
  - cli/src/commands.h
  - cli/src/contacts.cpp
  - cli/src/contacts.h
  - cli/src/main.cpp
  - cli/tests/CMakeLists.txt
  - cli/tests/test_contacts.cpp
findings:
  critical: 2
  warning: 5
  info: 4
  total: 11
status: issues_found
---

# Phase 116: Code Review Report

**Reviewed:** 2026-04-16T00:00:00Z
**Depth:** standard
**Files Reviewed:** 9
**Status:** issues_found

## Summary

Reviewed the full CLI source including the new contact group feature (contacts DB layer, command dispatch, group subcommands, and tests). The DB schema migration design is sound and the group cascade-delete tests are well-targeted. Two critical issues stand out: an unchecked optional dereference in `contact_add` that will crash on malformed hex input, and a silently-dropped step result in `ContactDB::add` that hides write failures. Several unchecked `sqlite3_exec` calls in migration code also need attention.

## Critical Issues

### CR-01: Unchecked dereference of `from_hex()` in `contact_add`

**File:** `cli/src/commands.cpp:1397`
**Issue:** `auto target_ns = *from_hex(namespace_hex);` dereferences the `std::optional` return value without checking whether it has a value. If `namespace_hex` contains non-hex characters or has the wrong length, `from_hex()` returns `std::nullopt` and dereferencing it is undefined behavior (crash). The size check on line 1398 (`if (target_ns.size() != 32)`) fires after the dereference and is therefore useless as a guard.

**Fix:**
```cpp
auto maybe_ns = from_hex(namespace_hex);
if (!maybe_ns || maybe_ns->size() != 32) {
    std::fprintf(stderr, "Error: namespace must be 64 hex chars\n");
    return 1;
}
auto target_ns = *maybe_ns;
```

---

### CR-02: `sqlite3_step` return value silently ignored in `ContactDB::add`

**File:** `cli/src/contacts.cpp:141`
**Issue:** `sqlite3_step(stmt)` is called but its return value is discarded entirely. `INSERT OR REPLACE` suppresses duplicate-key errors, but other failures (SQLITE_IOERR, SQLITE_FULL, SQLITE_CONSTRAINT from another violation) are swallowed silently. The caller receives no indication that the contact was not stored.

**Fix:**
```cpp
int rc = sqlite3_step(stmt);
sqlite3_finalize(stmt);
if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("Failed to insert contact: ") +
                             sqlite3_errmsg(db_));
}
```

---

## Warnings

### WR-01: `PRAGMA foreign_keys = ON` return value ignored

**File:** `cli/src/contacts.cpp:23`
**Issue:** The return value of `sqlite3_exec(db_, "PRAGMA foreign_keys = ON", ...)` is ignored. If this PRAGMA fails (compiled-out SQLite, or a transient error), foreign key enforcement is silently disabled. All cascade delete behavior — which the tests explicitly verify — will stop working with no diagnostic.

**Fix:**
```cpp
char* pragma_err = nullptr;
if (sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, &pragma_err) != SQLITE_OK) {
    std::string msg = pragma_err ? pragma_err : "unknown";
    sqlite3_free(pragma_err);
    throw std::runtime_error("Failed to enable foreign keys: " + msg);
}
```

---

### WR-02: Unchecked `sqlite3_exec` for schema bootstrap inserts/updates in `migrate()`

**File:** `cli/src/contacts.cpp:45,54-55,86-88,114-116`
**Issue:** Several `sqlite3_exec` calls that write to `schema_version` inside migration transactions do not check their return value. For example, `INSERT INTO schema_version VALUES (0)` at line 45, and the two `UPDATE schema_version SET version = N` calls inside migrations. If either fails, the schema version is not recorded, and the next DB open will attempt to re-run the migration, hitting "table already exists" and throwing.

**Fix:** Check the return value and throw on failure, consistent with the pattern already used for the `CREATE TABLE` calls in the same migrations. Example for the bootstrap insert:
```cpp
char* err = nullptr;
if (sqlite3_exec(db_, "INSERT INTO schema_version (version) VALUES (0)",
                 nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown";
    sqlite3_free(err);
    throw std::runtime_error("Failed to bootstrap schema_version: " + msg);
}
```

---

### WR-03: Out-of-bounds read on malformed `DelegationListResponse` in `delegations()`

**File:** `cli/src/commands.cpp:1270-1275`
**Issue:** After reading `count` from the response payload, the loop reads `4 + i * 64` bytes per entry without first verifying `resp->payload.size() >= 4 + count * 64`. A malformed or malicious server response with `count` larger than the actual payload will cause an out-of-bounds read on the payload vector. The `revoke()` function at line 1166 correctly checks this bound; `delegations()` does not.

**Fix:**
```cpp
uint32_t count = load_u32_be(resp->payload.data());
if (resp->payload.size() < 4 + static_cast<size_t>(count) * 64) {
    std::fprintf(stderr, "Error: truncated DelegationListResponse\n");
    return 1;
}
for (uint32_t i = 0; i < count; ++i) {
    // ... existing loop body
}
```

---

### WR-04: `group_create` error message conflates all SQLite failures as "already exists"

**File:** `cli/src/contacts.cpp:220-222`
**Issue:** When `sqlite3_step` returns anything other than `SQLITE_DONE`, the error thrown is always `"Group already exists: " + name`. But `SQLITE_CONSTRAINT_PRIMARYKEY` is only one possible non-DONE result. A disk full, I/O error, or locking failure would produce a misleading error message.

**Fix:**
```cpp
if (rc != SQLITE_DONE) {
    if (rc == SQLITE_CONSTRAINT) {
        throw std::runtime_error("Group already exists: " + name);
    }
    throw std::runtime_error(std::string("Failed to create group: ") +
                             sqlite3_errmsg(db_));
}
```

---

### WR-05: `group_list_members` returns exit code 0 for non-existent group

**File:** `cli/src/commands.cpp:1589-1592`
**Issue:** `ContactDB::group_members()` returns an empty vector both when the group exists but is empty and when the group does not exist at all. `group_list_members()` prints "no members in group: X" and returns 0 in both cases, making it impossible for callers or scripts to distinguish "group exists and is empty" from "group does not exist". This is user-visible confusion given the README shows `cdb group list <name>` as a primary operation.

**Fix:** Add a group-existence check before the members query, or add a `group_exists()` method to `ContactDB` and return exit code 1 with a distinct error message when the group is not found.

---

## Info

### IN-01: Hardcoded `VERSION` string not tied to CMake project version

**File:** `cli/src/main.cpp:17`
**Issue:** `static const char* VERSION = "1.0.0";` is a manually maintained constant. It will silently diverge from the project's actual version as development continues.

**Fix:** Generate via CMake `configure_file` or pass as a compile definition:
```cmake
target_compile_definitions(cdb PRIVATE CDB_VERSION="${CMAKE_PROJECT_VERSION}")
```
Then in `main.cpp`: `static const char* VERSION = CDB_VERSION;`

---

### IN-02: `contact add` silently accepts multiple host arguments

**File:** `cli/src/main.cpp:792-794`
**Issue:** The `contact add` subcommand consumes any remaining args after `<name> <namespace_hex>` as host[:port] targets in a `while` loop. `cdb contact add alice abc...64hex 192.168.1.73 typo` will parse "typo" as a second host with no error, silently overwriting the first. Every other command that accepts a positional host parses exactly one host and errors on extras.

**Fix:** Parse at most one positional host and report an error for additional args.

---

### IN-03: No test coverage for `group_list_members` empty-vs-missing group distinction

**File:** `cli/tests/test_contacts.cpp`
**Issue:** The test suite has good coverage of the DB layer but does not test `group_members()` against a group name that was never created. Given WR-05 above, a targeted test would pin the current behavior and catch if a fix introduces a regression.

**Fix:** Add a test case:
```cpp
TEST_CASE("contacts: group_members on nonexistent group returns empty", "[contacts]") {
    ContactsTempDir tmp;
    ContactDB db((tmp.path / "contacts.db").string());
    auto members = db.group_members("does-not-exist");
    REQUIRE(members.empty()); // documents current behavior
}
```

---

### IN-04: `contact_import` prints misleading "skip: pubkey fetch failed" on network error

**File:** `cli/src/commands.cpp:1640`
**Issue:** `contact_add()` can return non-zero for many reasons (connection failure, no pubkey found, bad namespace hex). The import loop collapses all failures to "skip: pubkey fetch failed", making bulk import failures opaque. The existing per-entry stderr output from `contact_add()` itself gives the real error, so this secondary message is mostly harmless but slightly confusing.

**Fix:** Accept the current behavior (low priority), or suppress the redundant message since `contact_add` already printed the real error: remove the `std::fprintf(stderr, "  skip: %s: pubkey fetch failed\n", ...)` line.

---

_Reviewed: 2026-04-16T00:00:00Z_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
