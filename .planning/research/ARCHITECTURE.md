# Architecture Research

**Domain:** Decentralized replicated key-value database with post-quantum cryptography
**Researched:** 2026-03-03
**Confidence:** MEDIUM-HIGH (component patterns well-established, Negentropy C++ availability unverified)

## System Model

CPUNK-DB is a **signed append-only operation log with materialized state and TTL-based expiry**. The architecture draws from three proven patterns:

- **Event Sourcing** — the operation log is the source of truth, current state is derived
- **Nostr relay model** — pubkey-owned data, dumb relays, client-side verification
- **Negentropy** — range-based set reconciliation for efficient sync

Data is **ephemeral by default** — all operations carry a TTL and expire. The exception is the profile namespace (identity anchor with public keys, bio, relay hints) which is permanent.

The library does NOT own transport. It produces and consumes sync messages. The caller provides the pipe (WebSocket, TCP, Bluetooth, IPC — doesn't matter).

## Components

### Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│                     Public API                          │
│  get() set() delete() grant() subscribe() sync_msg()   │
├──────────┬──────────┬───────────────┬───────────────────┤
│  State   │  Sync    │   Encryption  │   Namespace/AuthZ │
│  Engine  │  Engine  │   Envelope    │   (Grants)        │
├──────────┴──────────┴───────┬───────┴───────────────────┤
│              Operation Log                              │
│  create_op() verify_op() append() query() expire()     │
├─────────────────┬───────────────────────────────────────┤
│  Local Storage  │           Crypto Layer                │
│  (libmdbx)     │  sign() verify() encrypt() decrypt()  │
├─────────────────┤  encapsulate() decapsulate() hash()   │
│       HLC       │                                       │
└─────────────────┴───────────────────────────────────────┘
```

### Component Boundaries

| Component | Responsibility | Inputs | Outputs |
|-----------|---------------|--------|---------|
| **Crypto Layer** | ML-DSA-87 sign/verify, ML-KEM-1024 encaps/decaps, AES-256-GCM encrypt/decrypt, SHA3-256 hash | Raw bytes, keys | Signatures, ciphertexts, hashes |
| **HLC** | Hybrid Logical Clock — timestamp generation, merge on receive, bounded skew detection (MAX_SKEW ~5min) | Wall clock, remote HLC values | HLC timestamps (uint64: 48-bit ms + 16-bit counter) |
| **Operation Log** | Create, validate, store signed operations. Content-addressed by SHA3-256 hash. TTL field on every op | Key, value, op_type, ttl | Signed Operation struct |
| **Local Storage** | Persist operations and materialized state in libmdbx. TTL-based expiry scanning. Indexes by namespace, key, HLC, hash | Operations, queries | Stored ops, query results |
| **Namespace/AuthZ** | Verify write permission. Check author has valid grant for namespace + key prefix. Profile namespace rules (TTL=0 only for `profile/` keys) | Operation, namespace metadata | Allow/Deny + reason |
| **State Engine** | Materialize current KV state from operation log. LWW conflict resolution. Skip expired ops | Operation log queries | Current value for any (namespace, key) |
| **Sync Engine** | Set reconciliation. Produce/consume sync messages. Skip expired ops. Determine which ops to send/request | Local op set, remote sync messages | Sync messages (HAVE, WANT, RANGE, OPS) |
| **Encryption Envelope** | Encrypt values for specific recipients. ML-KEM key wrapping + AES-GCM payload encryption | Plaintext, recipient pubkeys | EncryptedEnvelope struct |
| **Public API** | User-facing interface. Orchestrates all components | Application calls | Results, events, sync messages |

## Operation Format

```
Operation {
    version:   uint8          // Format version (1 for v0.1)
    namespace: bytes[32]      // SHA3-256(owner_ml_dsa_pubkey)
    author:    bytes[32]      // SHA3-256(author_ml_dsa_pubkey)
    key:       string         // UTF-8, max 256 bytes
    value:     bytes          // payload, max 64KB
    op_type:   uint8          // SET=1, DELETE=2, GRANT=3, REVOKE=4
    hlc:       uint64         // HLC timestamp (48-bit wall clock ms + 16-bit counter)
    seq:       uint64         // per-author monotonic sequence number
    ttl:       uint32         // seconds until expiry (0 = permanent, profile keys only)
    prev:      bytes[32]      // hash of author's previous op in this namespace
    sig:       bytes[4627]    // ML-DSA-87 signature over hash
}

// hash = SHA3-256(version || namespace || author || key || value || op_type || hlc || seq || ttl || prev)
// sig = ML-DSA-87-Sign(hash, author_private_key)
// op_id = hash (content-addressed — the hash IS the operation ID)
```

**TTL rules:**
- `ttl = 0`: permanent. Only valid for operations where `namespace == author` AND key starts with `profile/`
- `ttl > 0`: operation expires at `hlc_to_wall_clock(hlc) + ttl` seconds
- Relays and clients prune expired operations
- Sync engine skips expired operations
- Grants inherit TTL — a grant cannot outlive its own TTL

## Data Flow

### Write Path

```
Application
    │ set(namespace, key, value, ttl)
    ▼
Public API
    │ 1. Validate TTL (ttl=0 only for own profile/ keys)
    │ 2. Check: is caller the namespace owner or has valid grant?
    ▼
Namespace/AuthZ
    │ 3. Generate HLC timestamp
    ▼
HLC
    │ 4. Create operation struct with all fields
    │ 5. Hash operation (SHA3-256)
    │ 6. Sign hash (ML-DSA-87)
    ▼
Operation Log + Crypto Layer
    │ 7. Persist to libmdbx (op log + update materialized state)
    ▼
Local Storage
    │ 8. Emit operation to sync engine for outbound push
    ▼
Sync Engine → produces OPS message → caller sends to relay(s)
```

### Read Path

```
Application
    │ get(namespace, key)
    ▼
Public API
    │ 1. Query materialized state (not the log)
    │ 2. Check if winning operation has expired (TTL)
    ▼
State Engine → Local Storage (libmdbx)
    │ 3. Return current value (already conflict-resolved, TTL-checked)
    ▼
Application (or null if expired/not found)
```

### Sync Path (Inbound)

```
Caller delivers sync message (from relay)
    │
    ▼
Sync Engine
    │ 1. Parse message type (HAVE / RANGE / OPS)
    │
    ├── HAVE/RANGE: Compare with local state, produce response messages
    │
    └── OPS: For each operation received:
            │ 2. Check TTL — skip if already expired
            │ 3. Verify signature (Crypto Layer)
            │ 4. Verify author permission (Namespace/AuthZ)
            │ 5. Check for duplicates (content-addressed hash)
            │ 6. Merge HLC (update local clock, bounded skew check)
            ▼
        Operation Log
            │ 7. Persist to libmdbx
            │ 8. Update materialized state (State Engine — LWW resolution)
            ▼
        Local Storage
            │ 9. Emit event to application (new data available)
            ▼
        Application callback / subscription
```

### Sync Path (Reconciliation)

```
Client connects to relay
    │
    ▼
Sync Engine
    │ 1. For each subscribed namespace, compute range fingerprint
    │    (XOR of all non-expired op hashes, bucketed by HLC range)
    ▼
    │ 2. Send RANGE message: [{lower_hlc, upper_hlc, fingerprint}, ...]
    ▼
Relay responds with RANGE (finer buckets for differing ranges)
    │
    │ 3. Recurse: split differing ranges, exchange fingerprints
    │    (typically 2-3 rounds to converge)
    ▼
    │ 4. Differing ops identified — send WANT for missing op hashes
    ▼
Relay responds with OPS (the actual operations)
    │
    │ 5. Ingest via normal inbound sync path
    ▼
Done — namespace is in sync
```

### Expiry Path

```
Background / periodic:
    │
    ▼
Local Storage
    │ 1. Scan operations where wall_clock(hlc) + ttl < now
    │ 2. Remove expired operations from op log
    │ 3. Update materialized state (next-best op wins, or key disappears)
    │ 4. Reclaim libmdbx pages (automatic page reclamation)
    ▼
Done — storage bounded by TTL
```

## Storage Schema (libmdbx)

```
Database: cpunkdb

Sub-databases:

  operations
    Key:   [namespace:32][hlc:8][hash:32]    (72 bytes)
    Value: [serialized Operation (FlatBuffers)]
    Sorted by namespace → HLC → hash

  state
    Key:   [namespace:32][key_bytes]
    Value: [winning_op_hash:32]
    Materialized current state (pointer to winning operation)

  expiry
    Key:   [expiry_timestamp:8][op_hash:32]  (40 bytes)
    Value: [namespace:32]
    Secondary index for efficient TTL scanning (sorted by expiry time)

  grants
    Key:   [namespace:32][grantee:32]
    Value: [serialized GrantPayload]
    Active capability grants for fast AuthZ lookup

  profiles
    Key:   [namespace:32]
    Value: [serialized profile data]
    Permanent profile data (identity anchor, pubkeys, bio, relay hints)

  meta
    Key:   [namespace:32]["hlc"]
    Value: [latest_hlc:8]
    Per-namespace HLC ceiling (for skew detection)

  sync_state
    Key:   [namespace:32][relay_id]
    Value: [last_synced_hlc:8][range_fingerprint:32]
    Per-relay sync progress (resume after disconnect)
```

## Build Order

Dependencies dictate the build order. Each layer depends only on layers below it.

```
Phase 1: Foundation (no dependencies)
├── Crypto Layer (liboqs wrapper — sign, verify, hash, encrypt)
└── HLC (standalone, ~100 LOC, bounded skew detection)

Phase 2: Core Data (depends on Phase 1)
├── Operation format (FlatBuffers schema, create/hash/sign/verify, TTL field)
├── Local Storage (libmdbx wrapper, CRUD, expiry index)
└── Namespace/AuthZ (ownership, grants, profile namespace rules)

Phase 3: Intelligence (depends on Phase 2)
├── State Engine (materialize KV, LWW conflict resolution, TTL checks)
└── Encrypted Envelopes (ML-KEM key wrapping + AES-GCM)

Phase 4: Sync (depends on Phase 3)
└── Sync Engine (set reconciliation, skip expired ops)
     Start with hash-list diff, upgrade to Negentropy

Phase 5: API (depends on Phase 4)
└── Public API (orchestrate all components, expose clean C++ interface)
```

## Open Questions

| Question | Impact | When to Decide |
|----------|--------|---------------|
| Negentropy C/C++ implementation availability | Build vs port decision for sync engine | Before Phase 4 planning |
| Default TTL values for common use cases | UX and storage sizing | Phase 2 (operation format) |
| Profile namespace key structure | What goes in profile/ besides pubkeys and bio | Phase 2 (namespace design) |
| ML-DSA-87 batch verification in liboqs | Performance optimization for bulk ingest | Phase 2 (nice to have) |
| Anti-spam mechanism (PoW vs capability-only) | Affects operation format fields | Phase 2 (operation format) |
| Snapshot operation type | Enables log compaction — reserve in format even if deferred | Phase 2 (operation format) |

---
*Architecture research for: decentralized replicated KV database with PQ crypto*
*Researched: 2026-03-03*
