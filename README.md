# chromatindb

A decentralized, post-quantum secure database node with signed blob storage and peer-to-peer replication.

chromatindb is a standalone daemon that stores cryptographically signed blobs organized into namespaces. Every blob is verified before storage, encrypted in transit using post-quantum algorithms, and replicated automatically across connected peers. Nodes form a self-organizing network where data is censorship-resistant and cryptographically authenticated end-to-end.

## Crypto Stack

All cryptographic operations use post-quantum algorithms from [liboqs](https://openquantumsafe.org/) and [libsodium](https://doc.libsodium.org/):

| Algorithm | Purpose | Standard |
|-----------|---------|----------|
| **ML-DSA-87** | Digital signatures (blob signing, identity) | FIPS 204 |
| **ML-KEM-1024** | Key exchange (peer handshake) | FIPS 203 |
| **ChaCha20-Poly1305** | Authenticated encryption (transport) | RFC 8439 |
| **SHA3-256** | Hashing (content addressing, namespace derivation) | FIPS 202 |
| **HKDF-SHA256** | Key derivation (session keys from KEM output) | RFC 5869 |

Every blob is cryptographically signed by its author using ML-DSA-87. Peers establish encrypted channels using post-quantum key exchange (ML-KEM-1024), then encrypt all traffic with ChaCha20-Poly1305 AEAD. No data travels in plaintext.

## Architecture

Each node generates an ML-DSA-87 keypair as its identity. The node's **namespace** is derived as `SHA3-256(public_key)`, providing a unique, verifiable identifier. **Blobs** are signed data units that belong to a namespace; each blob carries a signature, TTL (7-day protocol constant), and timestamp. Blobs are content-addressed by their SHA3-256 hash and stored in libmdbx, an ACID key-value store.

**Sync** works via a hash-list diff protocol: peers exchange lists of what they have, compute the difference, and request what they are missing. One blob is in flight at a time per connection, bounding memory usage. **Transport** security begins with an ML-KEM-1024 handshake that establishes a shared secret, from which ChaCha20-Poly1305 session keys are derived. All subsequent messages are AEAD-encrypted.

## Building

### Prerequisites

- C++20 compiler (GCC 12+ or Clang 15+)
- CMake 3.20+
- Git

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

All dependencies are fetched automatically via CMake FetchContent on first build. This includes liboqs, libsodium, FlatBuffers, libmdbx, Standalone Asio, Catch2, spdlog, nlohmann/json, and xxHash. The first build takes longer as dependencies are downloaded and compiled.

### Test

```bash
cd build
ctest --output-on-failure
```

## Usage

### Generate Identity

```bash
chromatindb keygen [--data-dir <path>] [--force]
```

Generates an ML-DSA-87 keypair and writes `node.key` and `node.pub` to the data directory. The node's namespace (SHA3-256 of the public key) is printed to stdout. Use `--force` to overwrite an existing identity.

### Start Daemon

```bash
chromatindb run [--config <path>] [--data-dir <path>] [--log-level <lvl>]
```

Starts the chromatindb daemon. The node loads or generates its identity, opens its storage database, binds to the configured address, and begins accepting peer connections. If bootstrap peers are configured, the node connects to them and initiates sync.

Log levels: `trace`, `debug`, `info`, `warn`, `error` (default: `info`).

### Print Version

```bash
chromatindb version
```

## Configuration

Create a JSON config file and pass it with `--config`:

```json
{
  "bind_address": "0.0.0.0:4200",
  "data_dir": "./data",
  "bootstrap_peers": ["192.168.1.10:4200"],
  "allowed_keys": [],
  "max_peers": 32,
  "sync_interval_seconds": 60,
  "log_level": "info"
}
```

- **bind_address** -- address and port to listen on (default: `0.0.0.0:4200`)
- **data_dir** -- directory for identity keys and blob storage (default: `./data`)
- **bootstrap_peers** -- list of `host:port` strings to connect to on startup
- **allowed_keys** -- namespace hashes (hex) of peers allowed to connect; empty means open mode
- **max_peers** -- maximum number of simultaneous peer connections (default: `32`)
- **sync_interval_seconds** -- interval between periodic sync rounds (default: `60`)
- **log_level** -- log verbosity (default: `info`)

## Scenarios

### Single Node

Generate keys and start the daemon with default settings. The node listens for inbound connections on port 4200.

```bash
chromatindb keygen
chromatindb run
```

### Two-Node Sync

Start Node A with default config. Start Node B with `bootstrap_peers` pointing to Node A. Blobs written to either node replicate automatically during periodic sync.

Node A:
```bash
chromatindb keygen --data-dir ./node-a
chromatindb run --data-dir ./node-a
```

Node B (`config-b.json`):
```json
{
  "data_dir": "./node-b",
  "bootstrap_peers": ["192.168.1.10:4200"]
}
```

```bash
chromatindb keygen --data-dir ./node-b
chromatindb run --config config-b.json
```

### Closed Mode with ACLs

Populate `allowed_keys` with the namespace hashes of permitted peers. Only listed peers can establish connections; all others are silently rejected after handshake.

```json
{
  "allowed_keys": [
    "a1b2c3d4e5f6...64-char-hex-namespace-hash...",
    "f6e5d4c3b2a1...64-char-hex-namespace-hash..."
  ]
}
```

The node enters closed mode implicitly when `allowed_keys` is non-empty. Reload the config at runtime by sending `SIGHUP` -- the node re-reads `allowed_keys` without restarting.

## v3.0 Features

**Blob Deletion** -- Namespace owners can permanently delete blobs by issuing signed tombstones. A tombstone removes the target blob from storage and replicates across the network via sync, ensuring the deletion propagates to all peers. Tombstones are permanent (TTL=0) and block future arrival of the deleted blob.

**Namespace Delegation** -- Owners can grant write access to other identities by creating signed delegation blobs. Delegates sign with their own key; the node verifies that a valid delegation blob exists before accepting the write. Revocation is done by tombstoning the delegation blob.

**Pub/Sub Notifications** -- Connected peers can subscribe to namespaces and receive real-time notifications when blobs are ingested or deleted. Notifications carry metadata (namespace, sequence number, hash, size) so subscribers can decide whether to fetch the full blob.

## Performance

Results measured on AMD Ryzen 9 / Linux 6.18. Your numbers will vary.

### Crypto Operations

| Benchmark | Iterations | Total (ms) | Avg (us) | Ops/sec |
|-----------|------------|------------|----------|---------|
| SHA3-256 (64 B) | 1000 | 0.3 | 0.3 | 3,018,394 |
| SHA3-256 (1 KiB) | 1000 | 2.5 | 2.5 | 400,595 |
| SHA3-256 (64 KiB) | 100 | 15.5 | 155.3 | 6,441 |
| SHA3-256 (1 MiB) | 100 | 230.4 | 2,304.2 | 434 |
| ML-DSA-87 keygen | 10 | 0.7 | 66.6 | 15,008 |
| ML-DSA-87 sign (64 B) | 100 | 12.5 | 124.6 | 8,024 |
| ML-DSA-87 verify (64 B) | 100 | 6.2 | 62.0 | 16,139 |
| ML-KEM-1024 keygen | 10 | 0.2 | 17.5 | 57,068 |
| ML-KEM-1024 encaps | 100 | 1.8 | 17.8 | 56,199 |
| ML-KEM-1024 decaps | 100 | 2.1 | 20.9 | 47,774 |
| ChaCha20-Poly1305 encrypt (64 B) | 1000 | 0.5 | 0.5 | 2,153,492 |
| ChaCha20-Poly1305 encrypt (1 KiB) | 1000 | 2.4 | 2.4 | 422,826 |
| ChaCha20-Poly1305 encrypt (64 KiB) | 100 | 12.7 | 127.3 | 7,856 |
| ChaCha20-Poly1305 encrypt (1 MiB) | 100 | 203.7 | 2,036.6 | 491 |
| ChaCha20-Poly1305 decrypt (64 B) | 1000 | 0.5 | 0.5 | 1,967,172 |
| ChaCha20-Poly1305 decrypt (1 KiB) | 1000 | 2.4 | 2.4 | 422,393 |
| ChaCha20-Poly1305 decrypt (64 KiB) | 100 | 12.6 | 126.0 | 7,938 |
| ChaCha20-Poly1305 decrypt (1 MiB) | 100 | 204.0 | 2,040.2 | 490 |

### Data Path

| Benchmark | Iterations | Total (ms) | Avg (us) | Ops/sec |
|-----------|------------|------------|----------|---------|
| Blob ingest (1 KiB) | 100 | 24.9 | 249.4 | 4,010 |
| Blob ingest (64 KiB) | 100 | 94.3 | 942.6 | 1,061 |
| Blob retrieve (1 KiB) | 1000 | 0.3 | 0.3 | 2,905,659 |
| Blob retrieve (64 KiB) | 1000 | 1.4 | 1.4 | 726,372 |
| Blob encode (1 KiB) | 1000 | 0.2 | 0.2 | 4,384,849 |
| Blob decode (1 KiB) | 1000 | 0.2 | 0.2 | 5,958,233 |
| Sync throughput (100x1KiB) | 10 | 149.2 | 149.2 | 6,704 |

### Network Operations

| Benchmark | Iterations | Total (ms) | Avg (us) | Ops/sec |
|-----------|------------|------------|----------|---------|
| PQ handshake (full) | 10 | 4.9 | 488.4 | 2,047 |
| Notification dispatch | 100 | 12.5 | 125.1 | 7,996 |

Post-quantum handshakes complete in under 500 us. Blob ingest (sign + validate + store) takes ~250 us for 1 KiB payloads. Sync transfers ~6,700 blobs/sec between two nodes (in-process, no TCP overhead). Notification callbacks fire within ~125 us of blob ingestion.

### Running Benchmarks

```bash
cd build
cmake .. && make -j$(nproc) chromatindb_bench
./chromatindb_bench
```

## License

MIT
