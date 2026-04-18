---
phase: 999.1-acl-enforcement-and-live-revocation-test-sigsegvs
status: resolved
resolved: 2026-04-18
mode: inline
---

# Phase 999.1 — Resolved Inline (2026-04-18)

## What the bug was

Node-to-node sync was silently broken in open mode. After handshake,
both nodes classified each other as `client` (not `peer`), so the peer
replication machinery (sync, PEX, dedup, BlobNotify) was skipped. Blob
counts stayed at `syncs=0` forever.

Two related test SIGSEGVs had the same root cause: `is_client` was
inferred by probing ACL allow-lists (`acl_.is_client_allowed()` returns
`true` for all identities in open mode), so TCP inbound connections
were incorrectly flagged as clients regardless of the remote's actual
role.

## How it was fixed

Added an explicit 1-byte **Role** enum carried in the AEAD-encrypted
`AuthSignature` payload (not cleartext TrustedHello — stronger because
the role is integrity-protected by the session keys and covers both
lightweight and PQ handshake paths uniformly).

```
AuthSignature payload (new) = [role:1][pubkey_size:4 BE][pubkey][signature]
```

Reserved enum slots for future roles (Observer, Admin, Relay) so later
additions don't need another wire bump. Decoders fail-closed on unknown
role values.

Receiver-side classification now uses `conn->peer_role()` directly, not
ACL inference. `PeerInfo::is_client` was replaced with `net::Role`
throughout the peer-management layer.

## Commits

| Commit | Scope |
|---|---|
| `f5fa13bf` | `db/net/role.h` — the enum |
| `0eaa4377` | Wire format + classifier + both binaries updated |
| `34a4ad31` | `is_client` → `Role` cleanup across peer manager |
| `784b1260` | PROTOCOL.md documentation |

## Verification

**Production (2026-04-18, laptop at 192.168.1.173 ↔ server at 192.168.1.73):**

Server log after fix:
```
accepted connection from 192.168.1.173
handshake complete (responder, PQ, peer_role=peer)
Connected peer 1ec444e56d6afb24@192.168.1.173:59730
peer 192.168.1.173:59730 announced all sync namespaces
Sync responder 192.168.1.173:59730: received 0 blobs, sent 0 blobs, 1 namespaces (cursor: 1 hits, 0 misses)
metrics: ... syncs=1 ...
```

Blob round-trip verified end-to-end: `cdb put` on one node propagates
via sync; `cdb rm` tombstone propagates and deletes on the peer.

**Tests:**

- `[auth_helpers]`: 66 assertions, 12 cases, pass (role round-trip +
  fail-closed on unknown role).
- `[handshake]`: 48/48 pass.
- `[peer]`: 75/76 pass. Both target SIGSEGVs fixed. Remaining failure
  is the pre-existing `nodeinfo types_count` cosmetic (tracked in
  project memory, unrelated).

## Not included (deferred as separate backlog items)

- **999.2** — `cdb rm` idempotency + target-existence check
- **999.3** — `cdb put` of the same file creates duplicate blobs
