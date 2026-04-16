# Phase 117: Blob Type Indexing + ls Filtering - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-16
**Phase:** 117-blob-type-indexing-ls-filtering
**Areas discussed:** ls output format, Infrastructure hiding, Type labels

---

## ls Output Format

| Option | Description | Selected |
|--------|-------------|----------|
| Hash only | One hash per line, same as today | |
| Hash + type label | Hash and 4-char type label per line | |
| Hash + type + size | Hash, type, and data size per line | Initially selected |

**User's choice:** Initially selected Hash + type + size, then pulled back size to avoid node architecture drift (size would require a new index field and backfill).
**Final decision:** Hash + type label (no size).

### Size Source (withdrawn)

User decided to hold back size entirely. Reason: "let's hold back the size if it causes drift in the node arch"

### ls --raw Output

| Option | Description | Selected |
|--------|-------------|----------|
| Same format, all blobs | Hash + type label, includes infrastructure | ✓ |
| Hash only, all blobs | Just hashes, no labels, no filtering | |
| Hash + type, no labels | Hash + raw 4-byte hex prefix | |

**User's choice:** Same format, all blobs

### Unfiltered ListRequest

| Option | Description | Selected |
|--------|-------------|----------|
| Flag byte in ListRequest | 1-byte flags at offset 44, bit 0 = include_all | ✓ |
| Separate type filter | Sentinel type value meaning "no filter" | |

**User's choice:** Flag byte in ListRequest

---

## Infrastructure Hiding

### Architectural Discussion

Extended discussion about where filtering should happen. Key evolution:

1. User questioned why CLI needs client-side filtering at all — node should handle it
2. User then questioned what happens with developer-defined types — node can't know about them
3. User proposed removing ALL node-side filtering
4. User asked about expired blobs — they're removed by expiry scan, so filtering is redundant safety
5. User asked for honest architectural take

**Final architecture:** Node filters protocol internals (tombstones, delegations, expired). CLI filters application-level infrastructure types (PUBK, CDAT). Clean separation between protocol machinery and application concerns.

### Hidden Types (CLI-side)

| Option | Description | Selected |
|--------|-------------|----------|
| PUBK (0x50554B42) | Public key blobs | ✓ |
| CDAT (future) | Chunk data blobs (Phase 119) | ✓ |
| CENV (0x43454E56) | Encrypted envelope blobs | Initially selected, then withdrawn |
| Delegations (0xDE1E6A7E) | Defense-in-depth | ✓ |

**User's choice:** PUBK, CDAT, delegations. CENV withdrawn after clarification — those are user data.

### Unknown Type Label

| Option | Description | Selected |
|--------|-------------|----------|
| DATA | Generic label for unrecognized prefixes | ✓ |
| Show raw hex | Display actual 4-byte hex prefix | |
| Blank / no label | Only label recognized types | |

**User's choice:** DATA

### Summary Footer

| Option | Description | Selected |
|--------|-------------|----------|
| Silent | No mention of hidden blobs | ✓ |
| Count footer | "N infrastructure blobs hidden" message | |

**User's choice:** Silent

### Hide List Configuration

| Option | Description | Selected |
|--------|-------------|----------|
| Hardcoded | Compiled into CLI | ✓ (Claude's discretion) |
| Configurable | ~/.cdb/config.json hidden_types list | |

**User's choice:** You decide (Claude selected hardcoded — YAGNI)

---

## Type Labels

### Raw Mode Labels

| Option | Description | Selected |
|--------|-------------|----------|
| Human labels | CENV, PUBK, TOMB, DLGT in --raw mode | ✓ |
| Raw hex for all | Actual 4-byte hex prefix | |
| Human labels everywhere | Same labels in both modes | |

**User's choice:** Human labels (same in both modes)

### Label Names

| Option | Description | Selected |
|--------|-------------|----------|
| TOMB / DELE | Short, 4-char consistent | |
| TOMB / DLGT | DLGT avoids confusion with "delete" | ✓ |

**User's choice:** TOMB / DLGT

### --type Filter Flag

| Option | Description | Selected |
|--------|-------------|----------|
| Yes | cdb ls --type CENV for type-specific listing | ✓ |
| Not now | YAGNI, users can grep | |

**User's choice:** Yes

---

## Claude's Discretion

- Short blob type handling (<4 bytes)
- TYPE-03 server-side type filter wire format details
- Hide list implementation approach

## Deferred Ideas

- Blob size in ls output — deferred to avoid node architecture drift
