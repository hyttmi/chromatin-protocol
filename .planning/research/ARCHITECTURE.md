# Architecture Research

**Domain:** Decentralized PQ-secure database node (daemon)
**Researched:** 2026-03-03
**Confidence:** HIGH (daemon patterns, libmdbx threading, epoll networking, and P2P node design are mature, stable domains)

## System Model

chromatindb is a **signed blob store daemon with peer-to-peer replication over PQ-encrypted TCP**. Each node is a standalone process that:

1. Accepts signed blobs from clients and peers
2. Verifies namespace ownership (SHA3-256(pubkey) == namespace + ML-DSA-87 sig check)
3. Stores blobs in libmdbx with content-addressed dedup
4. Replicates to/from peer nodes via hash-list diff sync
5. Discovers peers via bootstrap nodes + peer exchange
6. Expires old data via TTL-based pruning

The daemon is **intentionally dumb** -- it has no application semantics. It stores opaque signed blobs, verifies cryptographic ownership, and replicates. Application logic (messages, profiles, nicknames) lives in higher layers (Relay, Client) that connect to chromatindb as clients.

Key architectural properties:
- **Content-addressed**: blob ID = SHA3-256(blob content), free dedup
- **Namespace-scoped**: all blobs belong to a namespace = SHA3-256(pubkey)
- **Ephemeral by default**: 7-day TTL default, TTL=0 for permanent
- **Verified on ingest**: every blob's signature checked before storage
- **No global state**: no consensus, no ordering across namespaces

## System Overview

```
                    ┌──────────────────────────────────────┐
                    │           chromatindb daemon          │
                    │                                      │
  Clients ─────────┤  ┌──────────────┐  ┌──────────────┐  ├───── Peers
  (Relay,          │  │  Client      │  │  Peer        │  │     (other
   CLI,            │  │  Listener    │  │  Manager     │  │      chromatindb
   debug)          │  └──────┬───────┘  └──────┬───────┘  │      nodes)
                    │         │                 │          │
                    │         ▼                 ▼          │
                    │  ┌──────────────────────────────┐   │
                    │  │         Event Loop            │   │
                    │  │   (epoll, connection mgmt)    │   │
                    │  └──────────────┬───────────────┘   │
                    │                 │                    │
                    │         ┌───────┴────────┐          │
                    │         ▼                ▼          │
                    │  ┌────────────┐  ┌────────────┐    │
                    │  │   Blob     │  │   Sync     │    │
                    │  │   Engine   │  │   Engine   │    │
                    │  └─────┬──────┘  └─────┬──────┘    │
                    │        │               │            │
                    │        ▼               ▼            │
                    │  ┌──────────────────────────────┐   │
                    │  │        Storage (libmdbx)      │   │
                    │  └──────────────────────────────┘   │
                    │                 │                    │
                    │  ┌──────────────┴───────────────┐   │
                    │  │                              │   │
                    │  ▼                              ▼   │
                    │  ┌────────────┐  ┌────────────┐    │
                    │  │   Crypto   │  │   Config   │    │
                    │  │   Layer    │  │   + Log    │    │
                    │  └────────────┘  └────────────┘    │
                    └──────────────────────────────────────┘
```

## Component Responsibilities

| Component | Responsibility | Communicates With |
|-----------|----------------|-------------------|
| **Event Loop** | epoll-based IO multiplexing. Owns all socket file descriptors. Dispatches read/write events to connection handlers. Drives timers (TTL expiry, peer health checks, sync intervals) | All network-facing components |
| **Client Listener** | Accepts inbound TCP connections from clients (Relay, CLI, debug tools). Handles the client wire protocol: submit blob, query namespace, subscribe | Event Loop, Blob Engine |
| **Peer Manager** | Maintains connections to peer nodes. Handles bootstrap (connect to seed nodes on startup), peer exchange (learn new peers from existing peers), connection health (keepalive, reconnect). Initiates outbound connections | Event Loop, Sync Engine |
| **Blob Engine** | Core business logic. Receives blobs, verifies signatures (SHA3-256(pubkey) == namespace + ML-DSA-87 sig), checks for duplicates (content-addressed hash), assigns seq_num, persists. Handles queries ("give me namespace X since seq_num Y") | Storage, Crypto Layer, Sync Engine |
| **Sync Engine** | Bidirectional hash-list diff sync between peers. Computes what the local node has that a peer lacks (and vice versa). Transfers missing blobs. Tracks per-peer sync progress for resumable sync | Peer Manager, Storage, Blob Engine |
| **Storage** | libmdbx wrapper. Owns all database transactions. Provides typed access to the four indexes (blobs, sequence, expiry, peer state). Runs TTL expiry scans | Blob Engine, Sync Engine |
| **Crypto Layer** | liboqs + OpenSSL wrapper. ML-DSA-87 sign/verify, ML-KEM-1024 key exchange for transport encryption, AES-256-GCM channel encryption, SHA3-256 hashing | Blob Engine (sig verify), Peer Manager (transport encryption), Event Loop (TLS-like handshake) |
| **Config + Logging** | JSON config file parsing (nlohmann/json). spdlog structured logging. CLI argument handling | All components (read-only config, write logging) |

## Recommended Project Structure

```
src/
├── main.cpp                 # Entry point, signal handling, daemon lifecycle
├── config/                  # Configuration and CLI
│   ├── config.h             # Config struct, defaults
│   └── config.cpp           # JSON parsing, CLI args, validation
├── crypto/                  # PQ crypto wrapper
│   ├── crypto.h             # sign, verify, hash, kem_encaps, kem_decaps, aes_encrypt, aes_decrypt
│   ├── crypto.cpp           # liboqs + OpenSSL calls
│   └── identity.h           # Node identity (keypair, namespace derivation)
├── storage/                 # libmdbx wrapper
│   ├── storage.h            # Database interface (store_blob, get_blob, query_namespace, etc.)
│   ├── storage.cpp          # libmdbx transactions, sub-database management
│   └── expiry.cpp           # TTL expiry scanner
├── blob/                    # Blob validation and processing
│   ├── blob.h               # Blob struct, BlobEngine interface
│   ├── blob.cpp             # Ingest pipeline: verify, dedup, assign seq, store
│   └── query.cpp            # Namespace queries, seq_num-based polling
├── net/                     # Networking layer
│   ├── event_loop.h         # epoll wrapper, timer management
│   ├── event_loop.cpp       # IO multiplexing, connection lifecycle
│   ├── connection.h         # PQ-encrypted TCP connection (handshake, read, write)
│   ├── connection.cpp        # ML-KEM handshake + AES-256-GCM channel
│   ├── client_listener.h    # Accept client connections, client protocol handler
│   ├── client_listener.cpp
│   ├── peer_manager.h       # Peer connection pool, bootstrap, peer exchange
│   └── peer_manager.cpp
├── sync/                    # Peer-to-peer sync
│   ├── sync_engine.h        # Hash-list diff protocol
│   ├── sync_engine.cpp      # Compute diffs, transfer blobs, track progress
│   └── fingerprint.h        # XXH3 fingerprints for quick set comparison
├── wire/                    # Wire format (FlatBuffers)
│   ├── schemas/             # .fbs schema files
│   │   ├── blob.fbs         # Blob message schema
│   │   ├── protocol.fbs     # Client and peer protocol messages
│   │   └── sync.fbs         # Sync protocol messages
│   └── generated/           # flatc output (gitignored, regenerated by CMake)
└── util/                    # Shared utilities
    ├── types.h              # Namespace, Hash, Signature type aliases
    └── log.h                # spdlog configuration

tests/
├── unit/                    # Per-component unit tests (Catch2)
│   ├── test_crypto.cpp
│   ├── test_storage.cpp
│   ├── test_blob_engine.cpp
│   ├── test_sync_engine.cpp
│   └── test_wire_format.cpp
├── integration/             # Multi-component tests
│   ├── test_ingest_flow.cpp      # Client submits blob, verify stored correctly
│   ├── test_sync_two_nodes.cpp   # Two nodes sync via loopback
│   └── test_expiry.cpp           # Blobs expire after TTL
└── property/                # Property-based / fuzz tests
    └── test_sync_convergence.cpp  # Random blob sets, split, sync, verify identical
```

### Structure Rationale

- **`crypto/`:** Isolated from everything else. Pure functions (bytes in, bytes out). Easy to test, easy to replace if liboqs API changes. No network, no storage awareness.
- **`storage/`:** Owns all libmdbx access. No other component opens the database directly. This enforces the single-writer constraint and keeps transaction lifecycle in one place.
- **`blob/`:** The domain logic. Knows about namespaces, signatures, seq_nums, dedup -- but does not know about networking or sync protocols. Takes bytes, returns results.
- **`net/`:** Owns all sockets and the event loop. Knows about TCP connections and PQ-encrypted channels, but does not understand blob semantics. Passes deserialized messages to blob/ and sync/.
- **`sync/`:** Knows the sync protocol (hash-list diff) but does not own connections. Receives peer state from net/, queries storage/ for local state, produces sync messages for net/ to send.
- **`wire/`:** FlatBuffers schemas and generated code. Separated because the generated code is build-time artifact, and schemas are the wire format contract.

## Architectural Patterns

### Pattern 1: Single Event Loop with Worker Offload

**What:** One thread runs the epoll event loop, handling all socket IO (accept, read, write). CPU-heavy work (signature verification, hashing) is dispatched to a small thread pool. Results are posted back to the event loop thread via an eventfd or pipe.

**When to use:** Always. This is the core threading model.

**Trade-offs:**
- PRO: No locking on socket state. Connection lifecycle is simple. Matches how epoll is designed to be used.
- PRO: CPU-heavy crypto doesn't block the event loop.
- CON: Worker results must be marshaled back to the event loop thread (minor complexity).
- CON: If the thread pool is saturated, ingest latency increases (backpressure signal).

**Example:**
```cpp
// Event loop thread
void EventLoop::on_readable(int fd) {
    auto msg = connection_map_[fd]->read_message();
    if (msg.type == MessageType::SubmitBlob) {
        // Dispatch to worker pool -- crypto is expensive
        worker_pool_.submit([this, fd, blob = std::move(msg.blob)]() {
            auto result = blob_engine_.ingest(std::move(blob));
            // Post result back to event loop thread via eventfd
            this->post_result(fd, std::move(result));
        });
    }
}

// Worker thread (in pool)
// blob_engine_.ingest() does: verify sig, check dedup, assign seq, store
// Storage uses its own write transaction (libmdbx single-writer serialized internally)
```

### Pattern 2: libmdbx Transaction Discipline

**What:** libmdbx is single-writer / multi-reader with MVCC. All write paths go through a single write transaction at a time (libmdbx enforces this -- a second write txn blocks until the first commits/aborts). Read transactions are cheap and concurrent. The Storage component owns all transaction lifecycle.

**When to use:** Every database access.

**Trade-offs:**
- PRO: Readers never block writers, writers never block readers (MVCC).
- PRO: Zero-copy reads via mmap -- FlatBuffers data can be read directly from the mmap'd page without deserialization.
- CON: Only one write transaction at a time. Writes must be batched or serialized.
- CON: Long-lived read transactions pin old pages, preventing page reclamation. Read transactions must be short.

**Rules:**
1. Write transactions are owned by a single codepath (storage write queue or single writer thread). Never hold a write txn across an async boundary.
2. Read transactions must be scoped to a single query. Open, read, close. No holding read txns while waiting for network IO.
3. Batch writes where possible: ingesting N blobs in one write transaction is much faster than N separate transactions.
4. The expiry scanner runs in its own write transaction, separate from ingest.

**Example:**
```cpp
// Storage wrapper enforces transaction discipline
class Storage {
public:
    // Write path -- called from single writer context
    StoreResult store_blob(const Blob& blob) {
        auto txn = env_.start_write();  // blocks if another write txn active
        // Check dedup
        if (txn.get(blobs_dbi_, make_key(blob.namespace_, blob.hash))) {
            txn.abort();
            return StoreResult::Duplicate;
        }
        txn.put(blobs_dbi_, make_key(blob.namespace_, blob.hash), blob.data());
        txn.put(seq_dbi_, make_seq_key(blob.namespace_, next_seq_), blob.hash);
        txn.put(expiry_dbi_, make_expiry_key(blob.expiry_time(), blob.hash), blob.namespace_);
        txn.commit();
        return StoreResult::Stored;
    }

    // Read path -- concurrent, zero-copy
    std::optional<BlobView> get_blob(const Namespace& ns, const Hash& hash) {
        auto txn = env_.start_read();  // cheap, concurrent, never blocks
        auto result = txn.get(blobs_dbi_, make_key(ns, hash));
        // result points directly into mmap'd memory -- zero copy
        return result ? std::optional{BlobView{result}} : std::nullopt;
    }
};
```

### Pattern 3: PQ-Encrypted Transport Channel

**What:** Every peer-to-peer and client-to-node TCP connection performs an ML-KEM-1024 key exchange on connect, then uses AES-256-GCM for all subsequent data. This is a TLS-like pattern but with post-quantum key exchange instead of ECDHE.

**When to use:** Every TCP connection (peer and client).

**Trade-offs:**
- PRO: All data in transit is PQ-secure. Passive eavesdropper with a quantum computer cannot decrypt.
- PRO: Simple to implement -- one handshake, then symmetric encryption. No certificate chains, no PKI.
- CON: ML-KEM-1024 ciphertexts are 1568 bytes, public keys 1568 bytes. Handshake is ~3KB overhead.
- CON: No authentication in ML-KEM alone. Handshake must be followed by ML-DSA-87 signed challenge to authenticate the peer (prevents MITM).

**Handshake sequence:**
```
Initiator                              Responder
    │                                      │
    │──── ML-KEM encaps(responder_pk) ────>│
    │     [ciphertext: 1568 bytes]         │
    │                                      │ decaps → shared_secret
    │                                      │
    │<─── ML-KEM encaps(initiator_pk) ─────│
    │     [ciphertext: 1568 bytes]         │
    │ decaps → shared_secret               │
    │                                      │
    │ Both: session_key = SHA3-256(ss_a || ss_b || "chromatindb-v1")
    │                                      │
    │──── AES-GCM(sign(challenge)) ───────>│
    │<─── AES-GCM(sign(challenge)) ────────│
    │                                      │
    │ Both: Authenticated + Encrypted      │
```

### Pattern 4: Hash-List Diff Sync Protocol

**What:** Peer sync works by exchanging hash lists for overlapping namespace ranges, computing the symmetric difference, then transferring missing blobs. Simple, correct, and easy to reason about.

**When to use:** Every peer-to-peer sync session.

**Protocol flow:**
```
Node A                                 Node B
    │                                      │
    │──── SYNC_REQUEST(namespace, since_seq) ─>│
    │                                      │
    │<─── HASH_LIST([hash1, hash2, ...]) ──│  (hashes B has for this namespace since seq)
    │                                      │
    │ Compute: missing = B_hashes - A_hashes
    │ Compute: extra = A_hashes - B_hashes
    │                                      │
    │──── WANT([missing_hash1, ...]) ─────>│  (hashes A needs from B)
    │──── HAVE([extra_hash1, ...]) ────────>│  (hashes A has that B might need)
    │                                      │
    │<─── BLOBS([blob1, blob2, ...]) ──────│  (B sends what A wanted)
    │──── BLOBS([blob3, blob4, ...]) ─────>│  (A sends what B wanted)
    │                                      │
    │ Both: update per-peer sync progress  │
```

**Trade-offs:**
- PRO: Simple to implement and debug. Correct by construction (set difference is well-defined).
- PRO: Resumable -- per-peer sync state tracks last synced seq_num.
- CON: O(n) in total items per namespace for hash list exchange. Acceptable for small-to-medium namespaces (<100K blobs). For large namespaces, upgrade to Negentropy (range-based reconciliation) in a future version.
- CON: Full hash list must fit in memory. For 100K blobs with 32-byte hashes = ~3.2MB. Acceptable.

### Pattern 5: Timer-Driven TTL Expiry

**What:** A periodic timer (integrated into the event loop) triggers the expiry scanner. The scanner opens a write transaction, cursor-scans the expiry index up to `now`, deletes expired blobs from all indexes, and commits.

**When to use:** Continuously, on a configurable interval (default: every 60 seconds).

**Example:**
```cpp
void Storage::run_expiry_scan() {
    auto txn = env_.start_write();
    auto cursor = txn.open_cursor(expiry_dbi_);

    uint64_t now = current_timestamp();
    size_t purged = 0;

    // expiry index is sorted by timestamp -- scan from beginning
    for (cursor.to_first(); cursor.valid(); cursor.to_next()) {
        auto [expiry_key, ns_value] = cursor.current();
        uint64_t expiry_time = extract_timestamp(expiry_key);

        if (expiry_time > now) break;  // all remaining entries are in the future

        Hash blob_hash = extract_hash(expiry_key);
        Namespace ns = extract_namespace(ns_value);

        // Remove from all indexes
        txn.del(blobs_dbi_, make_key(ns, blob_hash));
        // Note: seq index entries also need cleanup or lazy cleanup on read
        cursor.del();  // remove from expiry index
        purged++;
    }

    txn.commit();
    spdlog::info("Expiry scan: purged {} blobs", purged);
}
```

## Data Flow

### Blob Ingest (Client Submits Blob)

```
Client TCP Connection
    │ FlatBuffers: SubmitBlob message
    ▼
Event Loop (epoll readable)
    │ Deserialize wire message
    │ Dispatch to worker pool
    ▼
Worker Thread
    │ 1. SHA3-256(pubkey) == namespace?     ──── NO ──> Reject: bad namespace
    │ 2. ML-DSA-87 verify(sig, blob)?       ──── NO ──> Reject: bad signature
    │ 3. SHA3-256(blob) → hash (blob ID)
    │ 4. Check dedup: hash already stored?  ──── YES ─> ACK: already stored
    ▼
Storage (write transaction)
    │ 5. Assign next seq_num for namespace
    │ 6. Store blob (namespace + hash → data)
    │ 7. Store seq entry (namespace + seq → hash)
    │ 8. Store expiry entry (expiry_time + hash → namespace)
    │ 9. Commit transaction
    ▼
Post back to Event Loop
    │ 10. Send ACK to client (with replication count = 1, local only so far)
    │ 11. Queue blob for outbound sync to connected peers
    ▼
Sync Engine (next sync cycle)
    │ 12. Push new blob to peer nodes
    ▼
Peer nodes verify + store independently
```

### Namespace Query (Client Reads)

```
Client TCP Connection
    │ FlatBuffers: QueryNamespace(namespace, since_seq)
    ▼
Event Loop
    │ Dispatch to blob engine (read path -- no worker needed, reads are fast)
    ▼
Blob Engine
    │ 1. Open read transaction (cheap, zero-copy via mmap)
    │ 2. Cursor scan seq index: namespace + since_seq → hash, hash, hash, ...
    │ 3. For each hash, fetch blob from blobs index
    │ 4. Filter: skip if expired (expiry_time < now)
    ▼
Event Loop
    │ 5. Serialize response: BlobList message (FlatBuffers)
    │ 6. Send to client
    ▼
Client receives blobs
```

### Peer Sync (Bidirectional)

```
Sync Timer fires (or new peer connects)
    │
    ▼
Sync Engine
    │ 1. For each connected peer, for each tracked namespace:
    │    a. Read per-peer sync state (last synced seq for this namespace)
    │    b. Query local seq index: all hashes since last_synced_seq
    │    c. Send SYNC_REQUEST to peer
    ▼
Peer responds with HASH_LIST
    │
    ▼
Sync Engine
    │ 2. Compute set difference:
    │    - missing_from_local = peer_hashes - local_hashes
    │    - missing_from_peer = local_hashes - peer_hashes
    │ 3. Send WANT(missing_from_local) to peer
    │ 4. Send BLOBS(missing_from_peer) to peer
    ▼
Receive BLOBS from peer
    │ 5. For each blob: run full ingest pipeline (verify, dedup, store)
    │ 6. Update per-peer sync state
    ▼
Both nodes now have union of blobs for synced namespaces
```

### Peer Discovery (Bootstrap + Exchange)

```
Node startup
    │
    ▼
Peer Manager
    │ 1. Read bootstrap node list from config
    │ 2. For each bootstrap node:
    │    a. TCP connect
    │    b. PQ handshake (ML-KEM-1024 key exchange + ML-DSA-87 auth)
    │    c. Send PEER_EXCHANGE_REQUEST
    ▼
Bootstrap node responds with PEER_LIST
    │
    ▼
Peer Manager
    │ 3. Add new peers to known peer list
    │ 4. Connect to a subset (target: maintain N peer connections, e.g., 8)
    │ 5. Periodically exchange peer lists with all connected peers
    │ 6. Reconnect on disconnect (with backoff)
    ▼
Steady state: N active peer connections, peer list grows organically
```

## Threading Model

```
┌─────────────────────────────────────────────────────────────┐
│                    Main Thread (Event Loop)                  │
│                                                             │
│  epoll_wait() → dispatch IO events                          │
│  Timer callbacks (expiry scan trigger, sync trigger,        │
│                   peer health check, stats reporting)       │
│  Connection accept/close lifecycle                          │
│  Write responses to sockets                                 │
│  Receive results from worker threads via eventfd            │
└──────────────────────────┬──────────────────────────────────┘
                           │ submit work
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                Worker Thread Pool (N threads)                │
│                                                             │
│  Signature verification (ML-DSA-87: ~0.3ms on x86)         │
│  Blob hashing (SHA3-256)                                    │
│  Storage writes (serialized by libmdbx single-writer)       │
│  Sync diff computation                                      │
│  PQ handshake crypto (ML-KEM-1024 encaps/decaps)           │
└─────────────────────────────────────────────────────────────┘
```

**Thread count:** 1 event loop thread + N worker threads (default N = number of CPU cores, configurable). For a small VPS (2 cores), this means 1 + 2 = 3 threads.

**Key threading rules:**
1. The event loop thread never does crypto or database writes. It only does IO and dispatching.
2. Worker threads never touch sockets. They post results back via eventfd.
3. libmdbx write transactions are serialized internally (second write blocks until first commits). No explicit locking needed, but keep write transactions short.
4. libmdbx read transactions can run concurrently on any thread.
5. The expiry scanner runs on a worker thread, triggered by the event loop timer.

## Storage Schema (libmdbx)

```
Database: chromatindb

Sub-databases (4 total -- intentionally minimal):

  blobs
    Key:   [namespace:32][hash:32]           (64 bytes)
    Value: [serialized Blob (FlatBuffers)]
    Purpose: Primary blob store. Content-addressed by namespace + hash.
    Sorted by namespace for efficient "all blobs in namespace" scans.

  sequence
    Key:   [namespace:32][seq_num:8]         (40 bytes)
    Value: [hash:32]
    Purpose: Per-namespace monotonic sequence index for efficient polling.
    Client asks "give me namespace X since seq_num Y" -- cursor scan from Y onward.
    seq_num is assigned by the receiving node (local ordering, not global).

  expiry
    Key:   [expiry_timestamp:8][hash:32]     (40 bytes)
    Value: [namespace:32]
    Purpose: Secondary index sorted by expiry time. Expiry scanner cursors from
    beginning, deleting entries where expiry_timestamp < now. Efficient batch purge.

  peers
    Key:   [peer_id:32][namespace:32]        (64 bytes)
    Value: [last_synced_seq:8][last_sync_time:8]   (16 bytes)
    Purpose: Per-peer, per-namespace sync progress. Enables resumable sync after
    disconnect. peer_id = SHA3-256(peer's pubkey).
```

**Why 4 sub-databases (not 7 from old design):**
- No grants sub-database (grants are relay/app layer)
- No profiles sub-database (profiles are app-layer semantics on top of blobs)
- No meta sub-database (no HLC, no per-namespace clock ceiling)
- No state sub-database (no materialized KV state -- blob store has no "current value" concept)
- No sync_state sub-database (merged into peers -- same purpose, simpler name)

## Wire Protocol (FlatBuffers)

Three protocol contexts, each with its own message set:

### Client Protocol (client <-> node)

```
ClientMessage (union):
  SubmitBlob      { blob: Blob }
  QueryNamespace  { namespace: [byte:32], since_seq: uint64, limit: uint32 }
  GetBlob         { namespace: [byte:32], hash: [byte:32] }

ServerMessage (union):
  BlobAck         { hash: [byte:32], replication_count: uint32 }
  BlobList        { blobs: [Blob] }
  BlobData        { blob: Blob }
  Error           { code: uint16, message: string }
```

### Peer Protocol (node <-> node)

```
PeerMessage (union):
  Handshake       { pubkey: [byte:2592], kem_ciphertext: [byte:1568] }
  HandshakeReply  { kem_ciphertext: [byte:1568], signed_challenge: [byte] }
  PeerExchange    { peers: [PeerInfo] }
  SyncRequest     { namespace: [byte:32], since_seq: uint64 }
  HashList        { hashes: [byte] }     // packed array of 32-byte hashes
  Want            { hashes: [byte] }
  Have            { hashes: [byte] }
  Blobs           { blobs: [Blob] }
  Keepalive       {}
```

### Blob Format

```
Blob:
  namespace:  [byte:32]     // SHA3-256(pubkey)
  pubkey:     [byte:2592]   // ML-DSA-87 public key
  data:       [byte]        // opaque payload
  ttl:        uint32        // seconds until expiry (0 = permanent, default 604800)
  timestamp:  uint64        // wall clock time of creation (unix seconds)
  signature:  [byte:4627]   // ML-DSA-87 signature over (namespace || data || ttl || timestamp)

Computed by node (not on wire):
  hash    = SHA3-256(namespace || pubkey || data || ttl || timestamp || signature)
  seq_num = assigned locally per namespace (monotonic counter)
```

## Build Order (Dependencies Between Components)

Dependencies dictate the build order. Each layer depends only on layers below it.

```
Phase 1: Foundation
├── Crypto Layer         (liboqs + OpenSSL wrapper: sign, verify, hash,
│                         kem_encaps, kem_decaps, aes_encrypt, aes_decrypt)
├── Wire Format          (FlatBuffers schemas: Blob, ClientMessage, PeerMessage)
└── Config + Logging     (nlohmann/json config, spdlog, CLI args)

Phase 2: Storage
└── Storage Layer        (libmdbx wrapper: 4 sub-databases, CRUD operations,
                          expiry scanner, transaction discipline)
    Depends on: Crypto (hashing), Wire Format (serialization)

Phase 3: Blob Engine
└── Blob Engine          (ingest pipeline: verify namespace, verify signature,
                          content-address, dedup, assign seq, store, query)
    Depends on: Storage, Crypto

Phase 4: Networking
├── Event Loop           (epoll wrapper, timer management, eventfd for worker results)
├── PQ Connection        (ML-KEM handshake, AES-GCM encrypted channel)
├── Client Listener      (accept clients, client protocol handler)
└── Worker Pool          (thread pool for CPU-heavy crypto/storage work)
    Depends on: Crypto, Wire Format

Phase 5: Peer System
├── Peer Manager         (bootstrap, peer exchange, connection pool, reconnect)
└── Sync Engine          (hash-list diff, bidirectional, resumable)
    Depends on: Networking, Blob Engine, Storage

Phase 6: Integration + Hardening
└── Full daemon          (main.cpp wiring, signal handling, graceful shutdown,
                          integration tests, multi-node test scenarios)
    Depends on: Everything
```

**Build order rationale:**
- **Crypto and wire format first** (Phase 1) because every other component needs hashing, signing, or serialization. These are leaf dependencies with no dependencies of their own.
- **Storage before blob engine** (Phase 2 before Phase 3) because the blob engine writes to and reads from storage. Storage can be tested independently with raw byte operations.
- **Blob engine before networking** (Phase 3 before Phase 4) because the blob engine can be tested without sockets -- feed it blobs directly in unit tests. This validates the entire ingest pipeline (verify, dedup, store) before any network complexity.
- **Networking before peers** (Phase 4 before Phase 5) because peer discovery and sync require an event loop, encrypted connections, and worker threads. The networking layer can be tested with a simple echo server before peer logic is added.
- **Peer system last** (Phase 5) because it integrates everything: networking + blob engine + storage + sync. This is where multi-node tests become possible.
- **Integration last** (Phase 6) because the daemon binary wiring (main.cpp, signal handling, graceful shutdown) should be done after all components are individually tested.

## Anti-Patterns

### Anti-Pattern 1: Thread-Per-Connection

**What people do:** Spawn a new thread for each incoming TCP connection.
**Why it's wrong:** With 100+ peer connections + client connections, thread count explodes. Context switching kills throughput. Thread-local libmdbx transactions get complicated.
**Do this instead:** Single event loop thread (epoll) handles all IO. Worker thread pool (fixed size) handles CPU-heavy work. This is the standard high-performance server pattern.

### Anti-Pattern 2: Holding libmdbx Read Transactions Across Async Boundaries

**What people do:** Open a read transaction, start reading, yield to the event loop to send partial results, come back later to read more.
**Why it's wrong:** Long-lived read transactions pin old MVCC snapshots. libmdbx cannot reclaim pages from committed write transactions until all readers that see those pages have closed their transactions. The database file grows without bound.
**Do this instead:** Open read transaction, read everything needed, close transaction, then send results. If the result set is large, paginate with cursors across separate transactions (using the seq_num as a resumption token).

### Anti-Pattern 3: Verifying Signatures on the Event Loop Thread

**What people do:** Receive blob, verify signature, store, all on the event loop thread.
**Why it's wrong:** ML-DSA-87 verification takes ~0.3ms on x86 (potentially 1-3ms on ARM). During verification, the event loop is blocked -- no other IO can be processed. With burst ingest (100 blobs), the event loop stalls for 30-300ms.
**Do this instead:** Dispatch verification to the worker thread pool. The event loop stays responsive.

### Anti-Pattern 4: Global Sync (Sync Everything With Every Peer)

**What people do:** On connecting to a new peer, dump the entire local database to sync.
**Why it's wrong:** With N peers and M total blobs, sync traffic is O(N * M). Network overwhelmed, no progress on ingest.
**Do this instead:** Namespace-scoped sync. Each peer connection tracks which namespaces to sync. Start with namespaces the local node cares about (has clients subscribed to). Use per-peer, per-namespace seq_num progress to only sync deltas.

### Anti-Pattern 5: Designing the Client Protocol First

**What people do:** Start with "what does the client see?" and build top-down.
**Why it's wrong:** For a daemon like this, the hard part is the peer system (encryption, sync, discovery). The client protocol is simple (submit, query, subscribe). Building top-down means the hard parts get designed under pressure at the end.
**Do this instead:** Build bottom-up (crypto -> storage -> blob engine -> networking -> peers -> client protocol). The client protocol falls out naturally from the blob engine API.

## Scaling Considerations

| Scale | Architecture Adjustments |
|-------|--------------------------|
| 1-10 nodes, <1K blobs/sec | Single event loop + 2-4 worker threads. All fits in RAM. No special tuning |
| 10-100 nodes, <10K blobs/sec | Increase worker pool. Tune libmdbx map size. Consider namespace-scoped sync to limit per-peer traffic. Monitor expiry scan duration |
| 100+ nodes, >10K blobs/sec | Shard by namespace across node clusters. Upgrade to Negentropy sync (O(diff) instead of O(n) hash lists). Batch blob ingest. Consider read replicas (libmdbx multi-reader is free) |

### Scaling Priorities

1. **First bottleneck: signature verification throughput.** ML-DSA-87 verification at ~0.3ms/op = ~3,300 verifications/sec/core. With 4 worker threads: ~13K blobs/sec. This is the ceiling. Mitigation: verification cache (skip re-verification of known hashes), batch processing (verify N sigs in pipeline).

2. **Second bottleneck: sync traffic.** Hash-list diff is O(n) per namespace per peer. With 100K blobs in a namespace and 10 peers, that's 1M hashes exchanged per sync cycle (32MB). Mitigation: upgrade to Negentropy (O(diff)), increase sync interval for large namespaces, compress hash lists.

3. **Third bottleneck: libmdbx write serialization.** Single-writer means write throughput is limited by transaction commit speed. Mitigation: batch multiple blob stores in a single write transaction (e.g., accumulate for 10ms or 100 blobs, whichever comes first, then commit once).

## Sources

- libmdbx MVCC model and single-writer semantics: well-documented in libmdbx header and erthink/libmdbx repository -- HIGH confidence
- epoll-based event loop pattern: standard Linux systems programming (Stevens, Kerrisk) -- HIGH confidence
- ML-KEM-1024 / ML-DSA-87 parameters and sizes: NIST FIPS 203/204 and liboqs documentation -- HIGH confidence
- Hash-list diff sync: standard set reconciliation approach, used in various distributed systems -- HIGH confidence
- Worker pool + eventfd pattern: standard modern C++ server architecture -- HIGH confidence
- FlatBuffers deterministic encoding: Google FlatBuffers documentation -- HIGH confidence
- P2P bootstrap + peer exchange: standard pattern from BitTorrent, Bitcoin, IPFS peer discovery -- HIGH confidence

**Confidence notes:**
- All patterns described here are well-established and stable. No rapidly-evolving areas where training data staleness is a concern.
- Specific library versions and API details should be verified against current documentation during implementation (particularly liboqs 0.15.0 API and libmdbx 0.13.11 C++ header).
- ML-DSA-87 verification timing (~0.3ms on x86) is an estimate from community benchmarks -- validate on target hardware.

---
*Architecture research for: chromatindb decentralized PQ-secure database node*
*Researched: 2026-03-03*
