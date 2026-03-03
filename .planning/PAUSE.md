# CPUNK-DB — Paused State

**Paused:** 2026-03-03
**Workflow:** /gsd:new-project
**Stage:** Step 7 — Define Requirements (not yet started)

## What's Done

1. **PROJECT.md** — Updated with full architecture decisions from deep questioning
2. **config.json** — YOLO mode, comprehensive depth, quality models, all agents on
3. **Research** — All 5 files complete and committed:
   - STACK.md (libmdbx, FlatBuffers, liboqs 0.15.0, XXH3, custom HLC)
   - FEATURES.md (12 table stakes, 11 differentiators, 10 anti-features)
   - ARCHITECTURE.md (8 components, operation format, storage schema, data flows)
   - PITFALLS.md (6 critical, 6 moderate, 3 minor)
   - SUMMARY.md (synthesis with roadmap implications)

## What's Next

1. **Define Requirements (Step 7)** — Create REQUIREMENTS.md from PROJECT.md + research
   - Research exists, so use feature categories from FEATURES.md
   - Present features by category, user scopes each (v1/v2/out of scope)
   - Generate REQ-IDs, create traceability section
   - Key categories: Identity/Crypto, Data Model, Ordering/Conflict, Access Control, Sync, Encryption, Profile/Naming

2. **Create Roadmap (Step 8)** — Spawn gsd-roadmapper agent
   - Research SUMMARY.md suggests 6 phases
   - Use comprehensive depth (8-12 phases, 5-10 plans each)
   - Quality model profile (Opus for roadmapper)

3. **Done (Step 9)** — Present completion, point to /gsd:discuss-phase 1

## Key Architecture Decisions Made During Questioning

- **Telegram model, trust reversed**: Client = source of truth, relay = untrusted cache
- **7-day default TTL**: All data ephemeral except profiles
- **Permanent profiles (TTL=0)**: Identity anchor with pubkeys, bio, nickname, wallet addresses
- **Unique immutable nicknames**: FWW + relay-enforced, no blockchain
- **Hybrid crypto**: Library owns sign/verify, app provides key material
- **Key derivation is app-layer**: Library stores pubkeys, seed→keypair is app's job
- **Encrypted envelopes in v1**: ML-KEM + AES-GCM from day one
- **Relay is separate project**: Thin service over existing DB (PostgreSQL/CouchDB)
- **libmdbx over LMDB**: CMake support, page reclamation, TTL pruning
- **FlatBuffers**: Deterministic encoding for content-addressed signing
- **REGISTER op_type**: For unique name registration (FWW, not LWW)
- **Wallet addresses in profile**: Derived from same seed, stored as profile data

## Resume Command

```
/gsd:resume-work
```

Or manually continue the /gsd:new-project workflow from Step 7 (Define Requirements).
