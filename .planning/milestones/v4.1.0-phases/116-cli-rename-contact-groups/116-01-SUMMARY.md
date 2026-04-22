---
phase: 116-cli-rename-contact-groups
plan: 01
title: "CLI Rename + Schema Migration"
subsystem: cli
tags: [rename, schema-migration, groups, sqlite]
dependency_graph:
  requires: []
  provides: [cdb-binary, schema-versioning, group-tables]
  affects: [cli/CMakeLists.txt, cli/src/main.cpp, cli/src/contacts.h, cli/src/contacts.cpp]
tech_stack:
  added: []
  patterns: [schema-versioning, migration-system, cascading-deletes]
key_files:
  created:
    - cli/tests/test_contacts.cpp
  modified:
    - cli/CMakeLists.txt
    - cli/src/main.cpp
    - cli/src/contacts.h
    - cli/src/contacts.cpp
    - cli/tests/CMakeLists.txt
    - cli/README.md
decisions:
  - "Renamed TempDir to ContactsTempDir in test file to avoid ODR violation with test_identity.cpp"
metrics:
  duration: 19min
  completed: "2026-04-16T04:16:24Z"
  tasks: 3
  files: 7
requirements:
  - ERGO-01
  - CONT-05
---

# Phase 116 Plan 01: CLI Rename + Schema Migration Summary

Renamed CLI binary from chromatindb-cli to cdb with version 1.0.0 and implemented SQLite schema migration system with groups/group_members tables using ON DELETE CASCADE foreign keys.

## Task Summary

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Rename CLI executable and all user-facing strings | 77cfe2ae | cli/CMakeLists.txt, cli/src/main.cpp, cli/src/contacts.h, cli/README.md, cli/tests/CMakeLists.txt |
| 2 | Implement schema migration system with group tables | 9a3d7432 | cli/src/contacts.h, cli/src/contacts.cpp |
| 3 | Test infrastructure and build verification | 003ad321 | cli/tests/CMakeLists.txt, cli/tests/test_contacts.cpp |

## Changes Made

### Task 1: CLI Rename
- Build target renamed from `chromatindb-cli` to `cdb` in CMakeLists.txt
- Removed symlink `add_custom_command` block (no longer needed)
- Version bumped from 0.1.0 to 1.0.0
- Default identity directory changed from `~/.chromatindb/` to `~/.cdb/`
- All usage/help strings updated to say `cdb`
- Added `group` command to usage() listing
- README.md fully updated with new binary name and commands table
- Preserved: HKDF label `chromatindb-envelope-kek-v1`, UDS path `/run/chromatindb/node.sock`, C++ namespace `chromatindb::cli`

### Task 2: Schema Migration System
- Replaced `init_schema()` with `migrate()` in ContactDB
- Created `schema_version` table for version tracking
- Migration 1: creates contacts table (skipped if pre-existing)
- Migration 2: creates `groups` and `group_members` tables with ON DELETE CASCADE on both foreign keys
- `PRAGMA foreign_keys = ON` set on every connection
- Implemented 6 group CRUD methods: `group_create`, `group_remove`, `group_add_member`, `group_remove_member`, `group_members`, `group_list`
- All SQL uses `sqlite3_bind_text()` prepared statements (no string interpolation)

### Task 3: Test Infrastructure
- Created `test_contacts.cpp` with 10 test cases covering schema migration and group CRUD
- Updated `tests/CMakeLists.txt` with `contacts.cpp` source and `PkgConfig::sqlite3` link
- All 41 tests pass (31 existing + 10 new contacts tests)
- `cdb` binary builds correctly, outputs `cdb 1.0.0`
- No `chromatindb-cli` binary produced

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] ODR violation with TempDir struct**
- **Found during:** Task 3
- **Issue:** TempDir struct defined in both test_identity.cpp and test_contacts.cpp would cause ODR violation at link time
- **Fix:** Renamed to ContactsTempDir and wrapped in anonymous namespace
- **Files modified:** cli/tests/test_contacts.cpp
- **Commit:** 003ad321

**2. [Rule 1 - Bug] Missing include for REQUIRE_THROWS_WITH**
- **Found during:** Task 3
- **Issue:** `REQUIRE_THROWS_WITH` requires `catch2/matchers/catch_matchers_string.hpp` include
- **Fix:** Added the missing include
- **Files modified:** cli/tests/test_contacts.cpp
- **Commit:** 003ad321

## Verification Results

- `grep -rn "chromatindb-cli" cli/` returns 0 results
- `cli/build/cdb version` outputs `cdb 1.0.0`
- `cli/build/cdb` binary exists, no `chromatindb-cli` binary
- `chromatindb-envelope-kek-v1` preserved in envelope.cpp
- `/run/chromatindb/node.sock` preserved in commands.h
- All 41 tests pass (identity: 8, wire: 16, envelope: 7, contacts: 10)
