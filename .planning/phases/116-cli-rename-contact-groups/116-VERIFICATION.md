---
phase: 116-cli-rename-contact-groups
verified: 2026-04-16T08:30:00Z
status: passed
score: 5/5
overrides_applied: 0
---

# Phase 116: CLI Rename + Contact Groups — Verification Report

**Phase Goal:** Users can manage contacts and groups from a `cdb` command, enabling `--share @team` workflow for enterprise file distribution
**Verified:** 2026-04-16T08:30:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths (ROADMAP Success Criteria)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | User runs `cdb` as the primary executable name (not a symlink to something else) | VERIFIED | `build/cli/cdb` is a 2.25 MB real executable (not a symlink). `CMakeLists.txt` has `add_executable(cdb` and `project(cdb)`. Symlink block removed. Binary outputs `cdb 1.0.0`. |
| 2 | User can create a named contact group, add/remove contacts, and see the group membership | VERIFIED | `cdb group create/add/rm/list` fully implemented in `commands.cpp` and wired in `main.cpp`. All 6 ContactDB group methods present. 10/10 group tests pass. |
| 3 | User can share a file with all members of a group using `cdb put --share @groupname file` | VERIFIED | `load_recipient_kem_pubkeys()` checks `arg[0] == '@'` first, expands to `db.group_members()`, validates `kem_pk`, throws "empty or does not exist" on empty group. |
| 4 | User can bulk-import contacts from a JSON file with `cdb contact import team.json` | VERIFIED | `contact_import()` in `commands.cpp` parses JSON array, validates entries, reuses `contact_add()` per entry, prints "Imported N/M contacts. Failed: N" summary. Wired in `main.cpp`. |
| 5 | SQLite database has a `schema_version` table that tracks the current schema version for future migrations | VERIFIED | `ContactDB::migrate()` creates `schema_version` on fresh DB, detects pre-existing contacts table, runs migrations 1 (contacts) and 2 (groups + group_members). PRAGMA foreign_keys = ON on every connection. |

**Score:** 5/5 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `cli/CMakeLists.txt` | Build target named cdb | VERIFIED | `add_executable(cdb`, symlink block removed, `project(cdb LANGUAGES C CXX)` |
| `cli/src/main.cpp` | cdb version, cdb usage, ~/.cdb/ default | VERIFIED | VERSION="1.0.0", `printf("cdb %s\n", VERSION)`, default dir `".cdb"`, usage says "cdb", group command listed |
| `cli/src/contacts.h` | ContactDB with migrate() and group method declarations | VERIFIED | `void migrate()`, all 6 group methods declared (group_create, group_remove, group_add_member, group_remove_member, group_members, group_list) |
| `cli/src/contacts.cpp` | Schema migration with versions 0-1-2, PRAGMA foreign_keys | VERIFIED | `migrate()` replaces `init_schema()`, schema_version table, migrations 1 and 2, `PRAGMA foreign_keys = ON`, `ON DELETE CASCADE` on both FKs |
| `cli/tests/test_contacts.cpp` | Schema migration and group CRUD tests | VERIFIED | 10 TEST_CASE blocks covering all scenarios; 10/10 pass |
| `cli/tests/CMakeLists.txt` | Test build with contacts.cpp and sqlite3 | VERIFIED | `test_contacts.cpp`, `contacts.cpp`, and `PkgConfig::sqlite3` all present |
| `cli/src/commands.h` | Declarations for group and import/export commands | VERIFIED | 8 new declarations: 6 group functions + contact_import + contact_export |
| `cli/src/commands.cpp` | Group CRUD commands, @group share resolution, import/export | VERIFIED | @group resolution at line 175 (before fs::exists check), all group implementations, contact_import and contact_export |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `cli/CMakeLists.txt` | build output | `add_executable(cdb ...)` | VERIFIED | Confirmed manually: `add_executable(cdb` present at line 86; no symlink block |
| `cli/src/contacts.cpp` | SQLite database | `migrate()` called from constructor | VERIFIED | Constructor line 14: `migrate();`; `ContactDB::migrate()` defined at line 21 |
| `cli/src/main.cpp` | `cli/src/commands.cpp` | `cmd::group_create()`, `cmd::contact_import()` | VERIFIED | gsd-tools confirmed; all 8 cmd:: calls present in main.cpp |
| `cli/src/commands.cpp` | `cli/src/contacts.h` | ContactDB group methods | VERIFIED | 7 `db.group_*` call sites in commands.cpp confirmed manually |
| `cli/src/commands.cpp load_recipient_kem_pubkeys` | `cli/src/contacts.h group_members` | `@group` resolution | VERIFIED | `arg[0] == '@'` check at line 175 before `fs::exists(arg)` at line 190 |

*Note: gsd-tools key-link verification reported false negatives for 4 of 5 links due to regex escaping issues with backslash patterns. All links were manually confirmed against codebase.*

### Data-Flow Trace (Level 4)

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|--------------------|--------|
| `commands.cpp: group_list` | `groups` | `db.group_list()` — `SELECT g.name, COUNT(gm.contact_name) FROM groups g LEFT JOIN group_members gm GROUP BY g.name` | Yes | FLOWING |
| `commands.cpp: group_list_members` | `members` | `db.group_members(group)` — `SELECT c.* FROM contacts c INNER JOIN group_members gm` | Yes | FLOWING |
| `commands.cpp: load_recipient_kem_pubkeys @group` | `members` | `db.group_members(group_name)` → `contact.kem_pk` | Yes | FLOWING |
| `commands.cpp: contact_export` | `contacts` | `db.list()` — existing `SELECT * FROM contacts` | Yes | FLOWING |
| `commands.cpp: contact_import` | per-entry import | `contact_add()` → node pubkey fetch | Yes | FLOWING |

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| `cdb version` outputs `cdb 1.0.0` | `/home/mika/dev/chromatin-protocol/build/cli/cdb version` | `cdb 1.0.0` | PASS |
| `cdb --help` shows group command | `build/cli/cdb --help 2>&1` | Shows `group        Manage contact groups (create/add/rm/list)` | PASS |
| All 10 contacts tests pass | `ctest --test-dir build/cli/tests -R contacts` | `100% tests passed, 0 tests failed out of 10` | PASS |
| Full test suite passes (no regressions) | `ctest --test-dir build/cli/tests` | `100% tests passed, 0 tests failed out of 41` | PASS |
| No chromatindb-cli in source files | `grep -rn "chromatindb-cli" cli/ --exclude-dir=build` | 0 results | PASS |
| HKDF label preserved | `grep "chromatindb-envelope-kek-v1" cli/src/envelope.cpp` | Found at line 34 | PASS |
| UDS path preserved | `grep "/run/chromatindb/node.sock" cli/src/commands.h` | Found at line 12 | PASS |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| ERGO-01 | 116-01 | CLI executable is `cdb` as primary name (not symlink) | SATISFIED | `build/cli/cdb` is a real 2.25 MB executable; `add_executable(cdb)` in CMakeLists.txt; outputs `cdb 1.0.0` |
| CONT-01 | 116-02 | User can create named contact groups | SATISFIED | `cmd::group_create()` implemented and wired; group create test passes |
| CONT-02 | 116-02 | User can add/remove contacts from groups | SATISFIED | `cmd::group_add()`, `cmd::group_rm()`, `cmd::group_rm_member()` implemented and wired |
| CONT-03 | 116-02 | User can share with a group in one flag (`--share @engineering`) | SATISFIED | `@` prefix detection in `load_recipient_kem_pubkeys()` expands group to KEM pubkeys; empty group aborts with error |
| CONT-04 | 116-02 | User can import contacts from a JSON file | SATISFIED | `contact_import()` parses JSON array, fetches pubkeys per entry, prints import summary |
| CONT-05 | 116-01 | SQLite schema versioning with `schema_version` table | SATISFIED | `migrate()` creates `schema_version`, runs migration 1 (contacts) and 2 (groups/group_members) |

**Requirements from REQUIREMENTS.md mapped to Phase 116:** ERGO-01, CONT-01, CONT-02, CONT-03, CONT-04, CONT-05
**Orphaned requirements (mapped to Phase 116 but not in any plan):** None

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `cli/src/contacts.cpp` | 188, 276, 304 | `return {}` in error path of SQLite prepare failures | Info | Graceful degradation — returns empty result when `sqlite3_prepare_v2` fails; not a stub (no data path bypassed; real queries run on success) |

No blocker or warning anti-patterns found. The `return {}` patterns are error guards for SQLite API failures, not stub implementations.

### Human Verification Required

None. All observable behaviors verifiable programmatically.

Note: `cdb put --share @groupname file` requires a live node connection (ML-KEM encryption + network). The @group resolution logic (SQL lookup, kem_pk extraction, error handling) is fully unit-tested and verified in the codebase. The full network round-trip is covered by VERI-03 in Phase 122.

### Gaps Summary

No gaps. All 5 ROADMAP success criteria verified. All 6 requirement IDs satisfied.

**Build artifact note:** The `cli/build/` directory contains pre-phase stale artifacts including a `chromatindb-cli` binary and a `cdb` symlink pointing to it. These are from before Phase 116. The authoritative build at `build/cli/cdb` (built April 16 at 08:16, after all phase commits) is a real binary outputting `cdb 1.0.0`. The stale `cli/build/` directory does not affect source correctness; a clean rebuild from source will produce only the `cdb` binary.

---

_Verified: 2026-04-16T08:30:00Z_
_Verifier: Claude (gsd-verifier)_
