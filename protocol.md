# Helix Protocol

> Working document. Do not commit to git.

---

## 1. Overview

Helix is a decentralized, post-quantum-safe message relay protocol. Nodes form
a simplified DHT network for deterministic message routing, with local persistent
storage on each node. All cryptographic operations use post-quantum algorithms
exclusively.

**Design principles:**
- Zero classical cryptography — PQ only, everywhere
- No privileged nodes — all nodes are equal
- Delete-after-fetch — minimal storage footprint
- Metadata protection — nodes don't learn real recipient identities
- Censorship resistant — no single point of failure
- Simple — small network (dozens to hundreds of nodes), not millions

---

## 2. Cryptographic Primitives

| Purpose              | Algorithm        | Notes                          |
|----------------------|------------------|--------------------------------|
| Identity / signing   | ML-DSA-1024 (Dilithium5) | All signatures           |
| Key encapsulation    | ML-KEM-1024 (Kyber-1024) | Message encryption       |
| Symmetric encryption | AES-256-GCM      | Payload encryption after KEM   |
| Key derivation       | HKDF-SHA3-256    | Derive symmetric key from KEM shared secret |
| Hashing              | SHA3-256          | Fingerprints, mailbox addresses, node IDs |
| TLS                  | PQ via OQS-OpenSSL 3.x provider | Statically linked |

No classical crypto (RSA, ECDSA, ECDH, Ed25519, X25519) is used anywhere in the
protocol.

---

## 3. Identity — DNA

A **DNA** is a user's self-sovereign identity on the network.

### 3.1 Core

- ML-DSA-1024 keypair (private key never leaves client)
- ML-KEM-1024 keypair (for receiving encrypted messages)
- Fingerprint: `SHA3-256(ml_dsa_pubkey)` — unique identifier

### 3.2 Profile Data

The DNA profile is a signed, versioned document containing:

| Field            | Type            | Notes                              |
|------------------|-----------------|------------------------------------|
| fingerprint      | 32 bytes        | Derived from ML-DSA pubkey         |
| ml_dsa_pubkey    | bytes           | Public signing key                 |
| ml_kem_pubkey    | bytes           | Public encryption key              |
| display_name     | string          | Human-readable name                |
| bio              | string          | Free text                          |
| avatar           | blob            | Profile image                      |
| social_links     | list of {platform, handle} | Social media addresses  |
| wallet_addresses | list of {chain, address}   | Derivable from pubkey hash but explicit for multi-chain |
| sequence         | uint64          | Monotonically increasing version   |
| signature        | bytes           | ML-DSA signature over all above fields |

### 3.3 Profile Updates

- Increment `sequence` number
- Sign the new profile with ML-DSA private key
- Push to the network
- Nodes accept only if `sequence` is higher than stored version
- Prevents replay attacks (can't push an older version)

---

## 4. Network — Simplified DHT

### 4.1 Why Simplified

Full Kademlia is designed for millions of nodes with O(log n) multi-hop routing.
Helix targets dozens to hundreds of nodes. Every node can maintain **full
membership** — no k-buckets, no iterative lookups.

XOR distance math is used to determine **responsibility**, not for routing.
Communication between nodes is direct (single hop).

### 4.2 Key Space

- 256-bit key space (SHA3-256 output)
- Node ID: `SHA3-256(node_ml_dsa_pubkey)`
- Distance between two IDs: `XOR(id_a, id_b)` interpreted as unsigned 256-bit integer

### 4.3 Responsibility

For a given key K, the R nodes with the smallest `XOR(node_id, K)` are
responsible for storing data associated with K.

- R = replication factor = `min(3, network_size - 1)`
- Any node can compute responsibility because every node knows all other nodes

### 4.4 Cryptographic Mailboxes

Messages are not stored under the recipient's raw fingerprint. Instead:

```
mailbox = SHA3-256(recipient_fingerprint || epoch_day)
```

- `epoch_day` = `unix_timestamp / 86400`
- Mailbox rotates daily
- Nodes responsible for a mailbox are determined by XOR distance to mailbox key
- **Nodes never see the recipient's real fingerprint** — only the mailbox hash
- Sender computes mailbox → knows which nodes to contact
- Recipient computes the same → knows where to check

### 4.5 DNA Profile Storage

DNA profiles are stored under a separate key:

```
profile_key = SHA3-256("dna:" || fingerprint)
```

The R closest nodes to `profile_key` store the DNA profile.
No daily rotation — profiles are persistent, not ephemeral.

### 4.6 Bootstrap

- 3-8 hardcoded bootstrap nodes: `0.bootstrap.cpunk.io` ... `N.bootstrap.cpunk.io`
- Bootstrap nodes are regular nodes with well-known DNS entries
- A new node connects to any bootstrap and receives the full membership list

---

## 5. Open Questions

1. **Membership protocol:** SWIM gossip? Simple periodic gossip? How do nodes
   learn about joins, failures, and health?

2. **Message flow:** Exact send/receive/delete protocol. How does recipient
   authenticate to fetch from a mailbox without revealing fingerprint?

3. **Replication details:** Semi-synchronous? How many acks before confirming
   to sender?

4. **Delete-after-fetch:** Tombstone propagation mechanism. Offline node
   recovery.

5. **Anti-spam:** Rate limiting, proof-of-work.

6. **Large file transfer:** Chunked protocol for blobs up to 50MB?

7. **Group messaging:** Deferred to later design.

8. **Transport:** REST endpoints, PQ TLS details.

9. **Tech stack:** C++, cpp-httplib, liboqs, libmdbx, jsoncpp, spdlog.
