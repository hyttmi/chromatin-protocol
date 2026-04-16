# Phase 116: CLI Rename + Contact Groups - Context

**Gathered:** 2026-04-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Rename the CLI executable from `chromatindb-cli` to `cdb` and implement contact group CRUD with `--share @groupname` workflow for enterprise file distribution. Also add contact import/export and SQLite schema versioning.

</domain>

<decisions>
## Implementation Decisions

### Executable Rename
- **D-01:** CMake target renamed from `chromatindb-cli` to `cdb`. CMake project name also changes to `cdb`.
- **D-02:** No backward-compatibility symlink. Clean break — no trace of `chromatindb-cli`.
- **D-03:** Version string changes to `cdb 1.0.0` (was `chromatindb-cli 0.1.0`).
- **D-04:** Data directory changes from `~/.chromatindb/` to `~/.cdb/`.
- **D-05:** UDS path stays `/run/chromatindb/node.sock` — the node binary hasn't changed.
- **D-06:** All usage/help strings, include paths, and test references updated. Full sweep.

### Group Data Model
- **D-07:** Normalized SQLite tables: `groups` (name TEXT PK) + `group_members` (group_name TEXT FK, contact_name TEXT FK). Many-to-many relationship.
- **D-08:** `group_members` has ON DELETE CASCADE for both foreign keys. Deleting a contact removes them from all groups. Deleting a group removes all its memberships.
- **D-09:** `--share` on both `put` and `reshare` resolves: `@groupname` → group members, bare `name` → contact lookup, file path → fallback. Resolution order: @group first, then contact name, then file path.
- **D-10:** Error and abort if `--share @group` resolves to 0 members (empty group).
- **D-11:** Error if resolved contact has no kem_pk: "contact X has no encryption key".
- **D-12:** `group add` accepts contact names only (not namespace hex). Contacts must already exist in DB.

### Group CLI Commands
- **D-13:** `cdb group create <name>` — create empty group.
- **D-14:** `cdb group add <group> <contact>...` — add contacts to group.
- **D-15:** `cdb group rm <group>` — delete entire group. `cdb group rm <group> <contact>` — remove contact from group.
- **D-16:** `cdb group list` — all groups with member counts. `cdb group list <name>` — members of specific group.
- **D-17:** No command aliases (`delete`, etc.). `rm` is the verb, consistent with `contact rm` and `cdb rm`.

### Contact Import/Export
- **D-18:** Import format: JSON array of `{"name": "...", "namespace": "...64hex..."}`. Pubkeys fetched from node during import.
- **D-19:** Upsert on duplicate contact names (same INSERT OR REPLACE behavior as ContactDB::add).
- **D-20:** Skip failed pubkey fetches, continue importing. Print summary at end: "Imported N/M contacts. Failed: X (reason)".
- **D-21:** Add `cdb contact export` that dumps contacts to JSON in the same format as import (round-trip).

### Schema Migration
- **D-22:** `schema_version` table with single row: `CREATE TABLE schema_version (version INTEGER NOT NULL)`.
- **D-23:** On database open: check if `schema_version` exists. If not, create it with version=0, then run all migrations.
- **D-24:** Migrations run sequentially (0→1→2→...) with per-migration transactions (BEGIN/COMMIT per step).
- **D-25:** Version 1: existing contacts table (already present). Version 2: add groups + group_members tables.

### Claude's Discretion
- Where `--share` resolution logic lives (main.cpp vs commands.cpp)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### CLI Source
- `cli/CMakeLists.txt` — Current build config, target name, dependencies
- `cli/src/main.cpp` — Current argument parsing, command dispatch, usage strings
- `cli/src/commands.h` — Command function signatures
- `cli/src/commands.cpp` — Command implementations
- `cli/src/contacts.h` — ContactDB class, Contact struct
- `cli/src/contacts.cpp` — SQLite schema, CRUD operations
- `cli/src/identity.h` — Identity management (keypair storage)
- `cli/src/envelope.h` — Envelope format (encryption layer)
- `cli/src/wire.h` — Wire protocol helpers (sha3_256, to_hex)
- `cli/src/connection.h` — PQ connection management

### Tests
- `cli/tests/CMakeLists.txt` — Test build config
- `cli/tests/test_envelope.cpp` — Envelope tests
- `cli/tests/test_identity.cpp` — Identity tests
- `cli/tests/test_wire.cpp` — Wire protocol tests

### Project
- `.planning/REQUIREMENTS.md` — ERGO-01, CONT-01 through CONT-05

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `ContactDB` class (`contacts.h/cpp`): SQLite-backed contact CRUD. Already has `add()`, `remove()`, `get()`, `list()`. Extend with group methods.
- `sha3_256()` and `to_hex()` in `wire.h`: Used for namespace derivation from signing_pk.
- SQLite3 linked via pkg-config: Already a dependency, no new deps needed.
- `parse_ttl()` in main.cpp: Pattern for argument parsing helpers.

### Established Patterns
- Command dispatch: if/else chain in main.cpp matching command string to `cmd::` function.
- Subcommand parsing: `contact` command uses `subcmd` string for `add/rm/list` dispatch.
- Connection options: `ConnectOpts` struct threaded through all network commands.
- SQLite usage: Raw `sqlite3_*` API (no ORM). Prepared statements with bind.

### Integration Points
- `main.cpp` usage() — needs new `group` command entry.
- `main.cpp` command dispatch — needs `group` block mirroring `contact` pattern.
- `commands.h` — new function declarations for group CRUD.
- `ContactDB` class — extend with group methods or create separate GroupDB.
- `put` and `reshare` argument parsing — `--share` resolution before passing to cmd functions.

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 116-cli-rename-contact-groups*
*Context gathered: 2026-04-16*
