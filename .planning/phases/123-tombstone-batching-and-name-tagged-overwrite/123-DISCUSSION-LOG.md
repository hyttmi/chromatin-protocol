# Phase 123: Tombstone Batching + Name-Tagged Overwrite — Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-20
**Phase:** 123-tombstone-batching-and-name-tagged-overwrite
**Areas discussed:** Seq strategy for NAME overwrite; BOMB target encoding + batch policy; Name cache + enumeration fallback; Delegate permissions; Node validation scope; Auto-BOMB on overwrite

---

## Seq strategy for NAME overwrite

### Q1: Seq source

| Option | Description | Selected |
|--------|-------------|----------|
| Use blob.timestamp | Existing signed timestamp = seq. No new state. | ✓ |
| Local counter in name_cache | Cache tracks last_seq per name. | |
| Query node for max-seen seq | Roundtrip per write. | |
| No explicit seq — timestamp + content_hash lex | No seq field at all. | |

### Q2: Tiebreak for equal timestamps

| Option | Description | Selected |
|--------|-------------|----------|
| content_hash DESC | Lexicographically larger hash wins. | ✓ |
| blob_hash DESC | Server-returned hash (non-deterministic). | |
| First-seen (cursor order) | Node decides. | |
| Reject ties — require writer retry | Bump timestamp and rewrite. | |

### Q3: NAME payload layout

| Option | Description | Selected |
|--------|-------------|----------|
| target_content_hash:32 only | Per ROADMAP, minimal. | ✓ |
| target_content_hash:32 + explicit_seq:8 | Self-contained NAME body. | |
| target_content_hash:32 + target_seq:8 | Content-blob seq. | |

### Q4: Name charset + length

| Option | Description | Selected |
|--------|-------------|----------|
| UTF-8, 1–255 bytes, no NUL/slash/control | POSIX-safe subset. | |
| UTF-8, 1–65535 bytes, no restrictions | Max flexibility. | ✓ |
| ASCII-only, 1–63 bytes, filename-safe | Predictable. | |

---

## BOMB target encoding + batch policy

### Q1: BOMB target encoding

| Option | Description | Selected |
|--------|-------------|----------|
| target_hash:32 only | Self-contained. | ✓ |
| seq:8 only | Smaller, requires seq→hash resolution. | |
| Mixed with type byte | Max flexibility, worst complexity. | |

### Q2: cdb rm batching triggers

| Option | Description | Selected |
|--------|-------------|----------|
| N seconds OR K targets | Time + count. | ✓ |
| K targets only | No time trigger. | |
| N seconds only | No count trigger. | |
| Explicit --batch flag | Manual grouping. | |

### Q3: Pending-batch store

| Option | Description | Selected |
|--------|-------------|----------|
| Long-lived daemon | Real batching across invocations. | |
| State file + scheduler tick | systemd/launchd integration. | |
| Foreground blocking cdb rm | Per-invocation N-second wait. | |
| No cross-invocation batching — per-command only | Simplest. No daemon, no state. | ✓ |

### Q4: Per-command behavior with many targets

| Option | Description | Selected |
|--------|-------------|----------|
| One BOMB per command regardless of count | 1 invocation = 1 BOMB. | ✓ |
| Chunk into K=64 per BOMB within invocation | Multiple signatures. | |
| One BOMB per command, hard-cap at K=4096 | Ceiling for sanity. | |
| No limits | MAX_BLOB_DATA_SIZE is the only ceiling. | |

**Notes:** Q2 answer reconciles with Q3's per-command choice — the N-second timer is vacuous in per-command mode. Q4 clarifies: within one invocation, emit one BOMB unconditionally.

---

## Name cache design + enumeration fallback

### Q1: Cache format

| Option | Description | Selected |
|--------|-------------|----------|
| Flat JSON `{ns_hex: {name: {hash, seq}}}` | Per-user file. | |
| Per-namespace sharded files | One file per namespace. | |
| MDBX client-side | Fast but heavy dep. | |
| No cache — always enumerate | Stateless. | ✓ |

### Q2: Enumeration API

| Option | Description | Selected |
|--------|-------------|----------|
| Node-side ListByMagic(ns, magic) | Generic prefix filter. | ✓ |
| Use sync-cursor + filter client-side | Massive over-fetch. | |
| Dedicated node-side NAME index DBI | Breaks "opaque" contract. | |
| Client maintains in-memory index | Needs running client. | |

**Notes:** User chose no-cache, deviating from ROADMAP's "cache with fallback" wording. Enumeration API (Q2) is the load-bearing dependency — ListByMagic chosen because it's semantically opaque (prefix compare, no NAME parsing).

---

## Delegate permissions for NAME + BOMB

### Q1: Delegate NAME writes

| Option | Description | Selected |
|--------|-------------|----------|
| Yes — delegates can write NAME | Authorized to write content → authorized to label. | |
| No — NAME is owner-only | Conservative. | |
| Allow — delegate NAME trackable via signer_hint | Same as Yes, noting audit property. | ✓ |

### Q2: Delegate BOMB emission

| Option | Description | Selected |
|--------|-------------|----------|
| No — BOMB owner-only, same as single tombstones | Consistent with engine.cpp:275. | ✓ |
| Yes — scope-limited to delegate's namespace | Complex verification. | |
| Yes — delegates can BOMB only their own blobs | Per-target signer lookup. | |
| Yes — unrestricted | Key-compromise mass-delete risk. | |

---

## Node validation scope + auto-BOMB

### Q1: BOMB validation at ingest (multi-select)

| Option | Description | Selected |
|--------|-------------|----------|
| ttl == 0 | Mandatory per memory. | ✓ |
| magic + count header sanity | Structural check. | ✓ |
| Delegates cannot emit BOMB | Same as Q2 above. | ✓ |
| Verify every target_hash is known blob | Wrong and expensive. | |

### Q2: Auto-BOMB on NAME overwrite

| Option | Description | Selected |
|--------|-------------|----------|
| Opt-in via --replace flag | Explicit, preserves history. | ✓ |
| Always auto-BOMB on overwrite | Aggressive reclamation. | |
| Never auto-BOMB — user runs cdb rm | Leaks storage. | |

---

## Claude's Discretion

Areas where user deferred to planner/Claude judgment:
- Exact BOMB magic byte values (proposal: `0x424F4D42` "BOMB").
- Exact new TransportMsgType name for D-10 ListByMagic.
- Whether `--replace` emits a BOMB-of-1 or a single tombstone.
- Whether `is_name()` / `is_bomb()` live inline in codec.h or in a new shared header.
- Test file naming + structure within `[phase123]` tag conventions.
- Page size / pagination for ListByMagic response.
- Error code byte values for BOMB rejection variants.

## Deferred Ideas

- Local name_cache.json — deferred until ListByMagic perf is measured.
- Server-side NAME index DBI — future 999.x phase per ROADMAP.
- Long-lived batching daemon for cdb rm — YAGNI.
- Delegation-gated BOMB (delegates BOMB only their own blobs) — complexity not justified.
- Explicit seq:8 field in NAME payload — blob.timestamp suffices.
- Multi-BOMB splitting within one invocation — one invocation = one BOMB.
