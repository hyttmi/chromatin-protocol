# Phase 122: Schema + Signing Cleanup — Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-20
**Phase:** 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
**Areas discussed:** Signing canonical form, signer_hint derivation, PUBK overwrite semantics, owner_pubkeys DBI layout, transport wire format, migration, Storage vocabulary

---

## Gray Area Selection

| Option | Description | Selected |
|--------|-------------|----------|
| Signing canonical form | Which bytes go into the SHA3 sponge for ML-DSA-87 signing | ✓ |
| signer_hint derivation | How the 32-byte hint is derived from the ML-DSA pubkey | ✓ |
| PUBK overwrite + rotation semantics | What happens when a second PUBK arrives for a registered namespace | ✓ |
| owner_pubkeys DBI key/value layout | What the new DBI stores and where it lives | ✓ |

**User's choice:** All four (multi-select).

---

## Signing Canonical Form

| Option | Description | Selected |
|--------|-------------|----------|
| (A) SHA3(signer_hint \|\| data \|\| ttl \|\| ts) | Pure rename of today's namespace_id → signer_hint. Byte-identical to today. | ✓ (initial) |
| (B) SHA3(pubkey \|\| data \|\| ttl \|\| ts) | Absorb full 2592B pubkey directly. Semantically cleaner but no real security gain given ML-DSA-87's key binding. | |

**User's choice (initial):** (A). **REVISED in Area 5a** — see below.

**Final decision after Area 5 analysis:** `SHA3(target_namespace || data || ttl || ts)`. Signer commits to target namespace, not signer_hint. Byte-identical to today's protocol. Prevents delegate cross-namespace replay.

---

## signer_hint Derivation

| Option | Description | Selected |
|--------|-------------|----------|
| (A) SHA3-256(pubkey) | Renames today's namespace_id derivation. 32-byte collision-resistant hint. | ✓ |
| (C) Different hash function | SHA3-512-truncated, BLAKE3, etc. No benefit, introduces second hash primitive. | |

**User's choice:** (A) SHA3-256(pubkey).
**Notes:** Literally the computation already at engine.cpp:190-191. No new hash primitive.

---

## PUBK Overwrite + Rotation Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| (A) First-wins, idempotent on match | Same signing pubkey → accept (KEM rotation OK). Different → reject with PUBK_MISMATCH. | ✓ |
| (C) Delegation-gated rotation | Rotation auth blob signed by old key authorizes PUBK overwrite. | |

**User's choice:** (A) first-wins.
**Deferred:** (C) delegation-gated rotation — its own future phase if needed; ML-DSA-87 designed for multi-decade security so no urgency.

---

## owner_pubkeys DBI Key/Value Layout

| Option | Description | Selected |
|--------|-------------|----------|
| (A) Bare signing pubkey (2592B) | Key=signer_hint, value=raw ML-DSA pubkey. KEM pubkey stays in PUBK blob. | ✓ |
| (C) (A) + metadata (first_seen_ts + originating_blob_hash) | ~40 extra bytes per namespace for observability. | |

**User's choice:** (A) bare signing pubkey.
**Deferred:** Metadata wrapper — can be added later without breaking change.
**Home:** New DBI on Storage class (structurally identical to existing delegation_map). Four new public methods: register_owner_pubkey, get_owner_pubkey, has_owner_pubkey, count_owner_pubkeys. All gated by STORAGE_THREAD_CHECK (Phase 121).

---

## Continuation Prompt

| Option | Description | Selected |
|--------|-------------|----------|
| Discuss transport-layer + 2-3 more | Cover 5a-7: transport wire format, migration, storage vocabulary. | ✓ |
| Write CONTEXT.md now | Lock what we have, defer remaining to research phase. | |

**User's choice:** Continue with 3 more areas.

---

## Transport Wire Format (Areas 5a + 5b — revises Area 1)

**Issue surfaced:** Locking owner_pubkeys DBI layout in Area 4 made it clear that for DELEGATE writes, `signer_hint ≠ target_namespace`. The Area 1 choice (sign `signer_hint`) would allow a delegate holding delegations on multiple namespaces to have their signed blobs re-filed across namespaces (cross-namespace replay). Today's protocol signs `namespace_id` (= target namespace), preventing this.

| Option | Description | Selected |
|--------|-------------|----------|
| Revise — sign target_namespace, transport carries it | Matches today's protection. signer_hint is lookup-only transport hint. Pure parameter rename in codec.cpp. | ✓ |
| Keep Area 1 as-is (signer_hint in sponge) | Simpler code but loses cross-namespace replay protection. | |

**User's choice:** Revise.

**5b — Transport envelope:** new body schema `{ target_namespace: 32B, blob: Blob }`. Planning should decide whether to repurpose `Data = 8` payload or add a new `BlobWrite` TransportMsgType (Claude recommendation: add new type for auditability; deferred to planning as Claude's Discretion).

---

## Migration

| Option | Description | Selected |
|--------|-------------|----------|
| (C) Detect + refuse with guidance | Schema version check, exit with clear message. | |
| (A) Hard cutover, silent | No detection. Operator wipes manually. | ✓ (via freeform) |
| (B) Build one-time migration tool | Re-serialize old-schema blobs to new schema. | |

**User's choice (freeform):** "i'll simply wipe the databases before i upgrade, i literally have only 2 nodes running".
**Interpretation:** Pure (A). No detection code. No warning. No migration tool. Operator coordinates the wipe manually.

---

## Storage Primary Key Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| Rename namespace_id → namespace in Storage API | Mechanical rename, no byte-level change, clearer vocabulary. | ✓ |
| Leave parameter names as namespace_id | Zero churn but inconsistent with new protocol vocabulary. | |

**User's choice:** Rename.

---

## Claude's Discretion

- Exact field name for the transport envelope's target_namespace (`target_namespace` vs `ns` vs `namespace`).
- Whether new Storage API uses `std::span<const uint8_t, 2592>` or `std::array<uint8_t, 2592>` for the pubkey parameter.
- Whether PUBK-first lives in a shared helper or is inlined at engine + sync paths (per no-duplicate-code preference).
- Naming of the new TransportMsgType if option (b) is chosen — `BlobWrite`, `SignedBlob`, `Write`, etc.
- Whether to add a schema-version byte to the MDBX env for future forensics.

## Deferred Ideas

- Delegation-gated PUBK rotation (own future phase if ML-DSA key rotation ever becomes necessary).
- Migration tool for pre-122 data dirs (ruled out; operator wipes manually).
- owner_pubkeys metadata wrapper (first_seen_ts, originating_blob_hash).
- Schema-version byte in the MDBX env.
