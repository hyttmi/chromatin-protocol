# Phase 92: KEM Key Versioning - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-07
**Phase:** 92-kem-key-versioning
**Areas discussed:** Key ring persistence, UserEntry v2 format, Envelope header changes, Rotation API surface

---

## Key Ring Persistence

| Option | Description | Selected |
|--------|-------------|----------|
| Numbered files | kem.0.sec/pub, kem.1.sec/pub, kem.sec/pub = latest copy | Yes |
| JSON manifest | Single keyring.json with base64-encoded keys | |
| Subdirectory per version | kem/0/sec, kem/1/sec | |

**User's choice:** Numbered files
**Notes:** None

### Follow-up: Pre-rotation Identity Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Lazy migration | Treat existing kem.sec as version 0 in memory, no file changes until rotate | Yes |
| Eager migration on load | Immediately create kem.0.sec from kem.sec on load | |

**User's choice:** Lazy migration
**Notes:** None

---

## UserEntry Format

| Option | Description | Selected |
|--------|-------------|----------|
| Version byte bump + key_version field | version=0x02, append key_version:4 BE before kem_sig | Yes (simplified) |
| Name field convention | Encode in display name | |
| Separate versioned blob type | Separate blob alongside UserEntry | |

**User's choice:** Option 1, but simplified — user clarified that no backward compatibility is needed (pre-MVP rule). No v1/v2 distinction. Single format with key_version field. Old entries simply not parsed.
**Notes:** User reminded that the no-backward-compat rule applies project-wide. No dual-format parsing, no migration tooling.

---

## Envelope Header Changes

| Option | Description | Selected |
|--------|-------------|----------|
| No changes needed | kem_pk_hash already identifies key. Decryptor tries ring keys via hash map. | Yes |
| Add key_version to stanza | Redundant with pk_hash, 4 extra bytes per recipient | |

**User's choice:** No changes needed
**Notes:** None

---

## Rotation API Surface

| Option | Description | Selected |
|--------|-------------|----------|
| Offline only | Identity.rotate_kem() is local. Caller publishes via directory.register() | Yes |
| Combined rotate + publish | Directory.rotate_kem() does both | |

**User's choice:** Offline only
**Notes:** Consistent with Identity being network-free

---

## Claude's Discretion

- Key ring internal data structure
- File write ordering and atomicity in rotate_kem()
- Test fixture design
- Error message wording

## Deferred Ideas

None
