# chromatindb — Project Brief

## What This Is

A decentralized, post-quantum secure database node. You run chromatindb on a server, it joins a network of other chromatindb nodes, stores signed blobs in cryptographically-owned namespaces, and replicates data across the network. Anyone can run a node. Anyone can generate a keypair and start writing. The system is designed to be technically unstoppable.

The database layer is intentionally dumb. It doesn't understand messages, profiles, nicknames, or any application semantics. It stores signed blobs, verifies ownership, replicates, and expires old data. Application logic (messaging, identity, social) lives in higher layers (relay, client) built on top.

## System Layers (build bottom-up)

```
┌─────────────────────────────────────────────────────────────┐
│  LAYER 3: CLIENTS (mobile/desktop apps)         [FUTURE]    │
│  • SQLite local cache                                        │
│  • Sign/encrypt operations, send to relay via WebSocket      │
│  • Verify received data                                      │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────────┐
│  LAYER 2: RELAY (e.g. 0.relay.cpunk.io)         [FUTURE]    │
│  • Application semantics: messages, profiles, nicknames      │
│  • Has its own keypair → writes to its own namespace         │
│  • Syncs from chromatindb network, serves clients            │
│  • Anyone can start their own network with their own keys    │
└──────────────────────────┬──────────────────────────────────┘
                           │  chromatindb protocol
                           │  PQ-encrypted TCP (ML-KEM-1024)
┌──────────────────────────┴──────────────────────────────────┐
│  LAYER 1: CHROMATINDB NETWORK (decentralized)   [BUILD NOW] │
│  • Anyone can run a node — permissionless                    │
│  • Stores signed blobs in namespaces                         │
│  • Namespace = SHA3-256(pubkey) — the math IS access control │
│  • Signature verification on every write                     │
│  • seq_num per namespace for efficient polling               │
│  • TTL on all blobs (7-day default, 0 = permanent)           │
│  • Node-to-node sync with hash-list diff                     │
│  • Node-to-node transport: TCP, PQ-encrypted (ML-KEM-1024)  │
│  • Write ACKs — confirmation that N nodes have the data      │
│  • Technically unstoppable — more nodes = more resilient     │
└─────────────────────────────────────────────────────────────┘
```

## Layer 1: chromatindb (what we're building now)

### Namespace Model

- Namespace = SHA3-256(pubkey). No registration. No authority. Generate key → you own a namespace.
- One key, one namespace. The node verifies SHA3(pubkey) == claimed namespace.
- The database does not know or care what's inside the blobs. It verifies ownership and stores.

### What a Node Does

```
Receive blob:
  1. Verify SHA3-256(pubkey) == namespace → reject if mismatch
  2. Verify ML-DSA-87 signature → reject if invalid
  3. Check dedup (content-addressed by SHA3-256 hash)
  4. Store blob, assign seq_num for this namespace
  5. Replicate to peer nodes
  6. ACK back to writer (with replication count)

Serve data:
  • "Give me namespace X since seq_num Y" → return blobs
  • "Give me all namespaces you have" → return namespace list

Sync with peers:
  • Hash-list diff: exchange blob hashes, transfer missing blobs
  • Bidirectional: both nodes end up with the union

Expiry:
  • Scan for expired blobs (TTL elapsed), prune from storage
  • Default TTL: 7 days. TTL=0 means permanent.
```

### Storage (libmdbx)

Each node stores:
- **Blobs**: signed data, keyed by namespace + hash
- **Sequence index**: per-namespace monotonic seq_num → blob hash
- **Expiry index**: sorted by expiry timestamp for efficient pruning
- **Peer state**: per-peer sync progress

### Sync Protocol

- Hash-list diff: nodes exchange lists of blob hashes, then exchange missing blobs
- Bidirectional: after sync, both nodes have the same data
- Resumable: per-peer state tracks progress
- Skips expired: don't sync dead blobs

### Peer Discovery (bootstrap + peer exchange)

No DHT. No gossip protocol. Simple and proven (same pattern as Bitcoin):

1. Node starts → connects to hardcoded bootstrap node(s) (e.g. `bootstrap.cpunk.io`)
2. Bootstrap sends its peer list
3. Node connects to peers, they share their peer lists
4. Node builds a view of the network
5. Peer list persisted locally — survives restarts without needing bootstrap again

Relay discovers chromatindb nodes the same way — it's just another peer connecting to the network.

Redundancy:
- Multiple hardcoded bootstrap addresses
- DNS fallback: `_chromatindb._tcp.cpunk.io` → SRV records
- Once a node has peers, it doesn't need bootstrap anymore

### Transport

- TCP between nodes
- Fully PQ-encrypted: ML-KEM-1024 key exchange + AES-256-GCM channel

### Blob Format (on the wire)

```
Blob {
  namespace:  [32B]     SHA3-256(pubkey)
  pubkey:     [2592B]   ML-DSA-87 public key
  data:       [bytes]   opaque payload (database doesn't interpret this)
  ttl:        u32       seconds until expiry (0 = permanent, default 604800)
  timestamp:  u64       wall clock time of creation
  signature:  [4627B]   ML-DSA-87 signature over (namespace || data || ttl || timestamp)
}

Computed:
  hash    = SHA3-256(blob content) — blob ID, dedup key
  seq_num = assigned by receiving node (local ordering per namespace)
```

### ACK Model

1. Writer sends blob to connected node(s)
2. Each node verifies, stores, ACKs
3. Nodes replicate to peers
4. Writer can poll: "how many nodes have blob X?"
5. Writer decides when replication is sufficient

### What chromatindb Does NOT Do

- No application semantics (no messages, profiles, nicknames, routing)
- No human-readable names (relay/app concern)
- No client authentication (relay concern)
- No message routing (relay concern)
- No conflict resolution / LWW / HLC (relay/app layer)
- No encrypted envelopes (relay/app layer)

chromatindb is a signed, replicated, expiring blob store. Nothing more.

## Higher Layers (future, not in scope now)

### Layer 2: Relay
- Has its own ML-DSA-87 keypair → owns a namespace on chromatindb
- Writes application data (messages, profiles, nicknames) as blobs
- Syncs from chromatindb to stay current
- Own libmdbx for indexed lookups
- Serves clients via WebSocket
- Anyone can start their own network: own keys → own namespace → own relay

### Layer 3: Client
- Mobile/desktop app with SQLite local cache
- Signs and encrypts locally, talks to relay
- Private key stays on device

## Crypto Stack (all NIST Category 5, via liboqs 0.15.0)

- **ML-DSA-87**: Signing/verification (2592B pubkey, 4627B sig)
- **ML-KEM-1024**: PQ-encrypted transport between nodes (1568B pubkey, 1568B ct)
- **AES-256-GCM**: Symmetric channel encryption (via OpenSSL 3.x)
- **SHA3-256**: Namespace derivation, blob IDs, content addressing

## Tech Stack

- C/C++20, CMake, FetchContent for all deps
- liboqs 0.15.0 (ML-DSA-87, ML-KEM-1024, SHA3-256)
- OpenSSL 3.x (AES-256-GCM)
- libmdbx 0.13.11 (storage)
- FlatBuffers 25.12.19 (wire format, deterministic encoding for signing)
- xxHash 0.8.3 (sync fingerprints)
- Catch2 3.13.0, spdlog 1.17.0, nlohmann/json 3.12.0

## Lessons From Previous Projects

- **chromatin-protocol**: Kademlia + libmdbx + WebSocket = too complex. No DHT ever again.
- **DNA messenger**: DHT storage unreliable. SQLite-as-cache on client worked.
- **PQCC**: PQ crypto stack proven and production-ready. Reuse directly.
