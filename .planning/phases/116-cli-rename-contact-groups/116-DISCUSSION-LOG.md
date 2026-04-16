# Phase 116: CLI Rename + Contact Groups - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-16
**Phase:** 116-cli-rename-contact-groups
**Areas discussed:** Executable rename, Group data model, Contact import format, Schema migration

---

## Executable Rename

| Option | Description | Selected |
|--------|-------------|----------|
| cdb is the target | CMake target becomes `cdb`. No symlink, no `chromatindb-cli` binary. All usage/help text says `cdb`. Clean break. | ✓ |
| cdb target + compat symlink | CMake target is `cdb`, but keep a `chromatindb-cli` symlink for backward compat. | |
| Just flip the symlink | Keep target as `chromatindb-cli`, make `cdb` the advertised name via symlink. | |

**User's choice:** cdb is the target (clean break)
**Notes:** No backward compatibility needed.

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, project = cdb | project(cdb ...) — simple, matches the binary name | ✓ |
| Keep project name | project(chromatindb-cli ...) stays | |

**User's choice:** Yes, project = cdb

| Option | Description | Selected |
|--------|-------------|----------|
| cdb 0.1.0 | Matches the new binary name | |
| chromatindb 0.1.0 | Use full project name without -cli suffix | |
| cdb 1.0.0 | Bump version since this is effectively a new release identity | ✓ |

**User's choice:** cdb 1.0.0

| Option | Description | Selected |
|--------|-------------|----------|
| Keep ~/.chromatindb/ | Avoids breaking existing identities and contacts.db. | |
| Change to ~/.cdb/ | Consistent with new name. | ✓ |

**User's choice:** Change to ~/.cdb/

| Option | Description | Selected |
|--------|-------------|----------|
| Keep /run/chromatindb/ | Node hasn't changed — it's still chromatindb. | ✓ |

**User's choice:** Keep /run/chromatindb/

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, update everything | All include paths, test references, help strings — full sweep. | ✓ |

**User's choice:** Full sweep, no trace of chromatindb-cli.

---

## Group Data Model

| Option | Description | Selected |
|--------|-------------|----------|
| Normalized tables | `groups` table + `group_members` table. Standard relational. | ✓ |
| JSON column on contacts | Add a `groups TEXT` column with JSON array. Denormalized. | |
| Separate groups.json file | Keep groups outside SQLite. | |

**User's choice:** Normalized tables

| Option | Description | Selected |
|--------|-------------|----------|
| Resolve in main.cpp | If arg starts with @, look up group members, expand to pubkeys. | |
| Resolve in commands.cpp | Push resolution into cmd::put(). | |
| You decide | Claude's discretion. | ✓ |

**User's choice:** Claude's discretion on where resolution lives.

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, names and @groups | --share alice resolves contact, --share @team resolves group. File path fallback. | ✓ |
| @ for groups only | --share only accepts pubkey files and @group. | |

**User's choice:** Names and @groups

| Option | Description | Selected |
|--------|-------------|----------|
| cdb group create/add/rm/list | Matches requirements. | ✓ |
| You decide | Claude designs subcommand structure. | |

**User's choice:** create/add/rm/list

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, many-to-many | alice can be in @engineering and @london. | ✓ |
| No, one group only | Each contact in at most one group. | |

**User's choice:** Many-to-many

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, delete whole group | `group rm engineering` deletes group. `group rm engineering alice` removes alice. | ✓ |
| Require --all flag | Must specify --all for full deletion. | |

**User's choice:** Overloaded rm: no contact = delete group, with contact = remove member.

| Option | Description | Selected |
|--------|-------------|----------|
| Cascade delete | Removing contact removes from all groups. ON DELETE CASCADE. | ✓ |
| Block deletion | Can't delete contact in any group. | |

**User's choice:** Cascade delete

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, both modes | `group list` = all groups. `group list engineering` = members. | ✓ |

**User's choice:** Both modes

| Option | Description | Selected |
|--------|-------------|----------|
| Contact names only | `group add engineering alice bob`. Contacts must exist. | ✓ |
| Also accept namespace hex | Mixed argument types. | |

**User's choice:** Contact names only

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, both put and reshare | Same @ and name resolution for both. | ✓ |
| put only for now | Only put gets @group. | |

**User's choice:** Both put and reshare

| Option | Description | Selected |
|--------|-------------|----------|
| Error and abort | Sharing with empty group is likely a mistake. | ✓ |
| Warn and continue | Print warning but proceed. | |

**User's choice:** Error on empty group

| Option | Description | Selected |
|--------|-------------|----------|
| Already in DB | group add only works with existing contacts. | ✓ |
| Fetch on demand | group add can optionally fetch. | |

**User's choice:** Already in DB

| Option | Description | Selected |
|--------|-------------|----------|
| No aliases | rm is the verb, consistent. | ✓ |
| Add delete alias | Both rm and delete work. | |

**User's choice:** No aliases

| Option | Description | Selected |
|--------|-------------|----------|
| Error: can't share | Error with clear message about missing kem_pk. | ✓ |

**User's choice:** Error on missing kem_pk

---

## Contact Import Format

| Option | Description | Selected |
|--------|-------------|----------|
| Array of {name, namespace} | Minimal. Pubkeys fetched from node during import. | ✓ |
| Full export format | Includes keys, no node needed. | |
| With optional groups | Same but with groups array per contact. | |

**User's choice:** Minimal format, pubkeys fetched from node

| Option | Description | Selected |
|--------|-------------|----------|
| Update (upsert) | Re-fetch pubkey and update. Same as INSERT OR REPLACE. | ✓ |
| Skip with warning | Skip existing, only add new. | |
| Error and abort | Refuse if any duplicate. | |

**User's choice:** Upsert

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, add export too | `cdb contact export > team.json`. Round-trip. | ✓ |
| Import only for now | Keep scope tight. | |

**User's choice:** Add export for round-trip

| Option | Description | Selected |
|--------|-------------|----------|
| Skip failed, report at end | Import what you can. Summary at end. | ✓ |
| Abort on first failure | Any failure aborts entire import. | |

**User's choice:** Skip failed with summary

---

## Schema Migration

| Option | Description | Selected |
|--------|-------------|----------|
| Single-row version table | One row, current version number. Run migrations sequentially. | ✓ |
| Migration history table | Track each migration with timestamp and description. | |

**User's choice:** Single-row version table

| Option | Description | Selected |
|--------|-------------|----------|
| Detect and bootstrap | If no schema_version, create with version=0, run all migrations. | ✓ |
| Require fresh start | Error if no schema_version, user deletes DB. | |

**User's choice:** Detect and bootstrap

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, per migration | Each step in BEGIN/COMMIT. If one fails, that step rolls back. | ✓ |
| All-or-nothing | Entire chain in one transaction. | |

**User's choice:** Per-migration transactions

---

## Claude's Discretion

- Where `--share` resolution logic lives (main.cpp vs commands.cpp)

## Deferred Ideas

None — discussion stayed within phase scope.
