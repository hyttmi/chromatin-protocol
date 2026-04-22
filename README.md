# chromatindb

A lightweight C++20 post-quantum secure blob storage node.

chromatindb stores cryptographically signed blobs organized into namespaces, replicates them across peers via O(diff) set reconciliation, and makes data censorship-resistant. Every blob is verified before storage, encrypted at rest, and encrypted in transit using post-quantum algorithms. Standalone daemon, not a library.

 * Post-quantum crypto: ML-DSA-87 signatures, ML-KEM-1024 key exchange, ChaCha20-Poly1305 AEAD
 * Content-addressed storage with ACID guarantees (libmdbx)
 * O(diff) sync via XOR-fingerprint set reconciliation
 * Namespace ownership via `SHA3-256(public_key)`, with a one-time PUBK registration that frees every subsequent blob from carrying an inline 2592-byte signing pubkey
 * Namespace delegation (owner grants write access to other identities)
 * Writer-controlled per-blob TTL with node-enforced expiry
 * Mutable names for blobs (publish by name, overwrite in place, resolve by name)
 * Batched deletion — one signed envelope can tombstone many targets in a single round-trip; deleting a large-file manifest cascades to every chunk
 * Chunked large files — files above 400 MiB automatically split into 16 MiB chunks plus a manifest; reassembly on download is transparent
 * Request pipelining for multi-blob fetches over a single PQ connection
 * Server-side blob type indexing for fast filtered listings (`ls --type BOMB`, `--type NAME`, etc.)
 * Configurable sync/peer constants with CLI peer management (`add-peer`, `remove-peer`, `list-peers`)
 * Pub/sub notifications for real-time change tracking
 * Per-namespace storage quotas and per-connection rate limiting
 * Prometheus `/metrics` endpoint
 * Encryption at rest (ChaCha20-Poly1305, HKDF-derived key)
 * SIGHUP hot-reload for ACLs, quotas, rate limits, metrics
 * Clean under AddressSanitizer, ThreadSanitizer, and UndefinedBehaviorSanitizer

## Documentation

- [db/PROTOCOL.md](db/PROTOCOL.md) — complete wire protocol specification (byte-level, for client implementers)
- [db/ARCHITECTURE.md](db/ARCHITECTURE.md) — internal implementation (DBIs, strand model, ingest pipeline)
- [db/README.md](db/README.md) — operator guide (configuration, deployment, testing)
- [cli/README.md](cli/README.md) — `cdb` command-line client (user guide)

## Quick Start

```bash
# Build (all dependencies fetched automatically via CMake FetchContent)
mkdir build && cd build
cmake ..
cmake --build .

# Generate identity
./db/chromatindb keygen

# Start a node
./db/chromatindb run
```

For the day-one client flow (keygen → publish → put → ls → get), see [cli/README.md](cli/README.md#hello-world).

### Two-Node Sync

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

Blobs written to either node replicate automatically.

## Building Your Own Client

chromatindb uses a binary protocol built on FlatBuffers. Wire schemas live in [`db/schemas/`](db/schemas/) — use them to generate bindings for any language.

After connecting and completing the PQ handshake, send a `NodeInfoRequest` to discover the node's version and supported message types. Client writes use the `BlobWrite = 64` envelope carrying a `BlobWriteBody{target_namespace, blob}`; the 5-field `Blob` table and `signer_hint` flow are documented in [PROTOCOL.md §Storing a Blob](db/PROTOCOL.md#storing-a-blob). See [PROTOCOL.md](db/PROTOCOL.md) for the full wire protocol specification — the handshake sequence, byte-level blob format, and the `BlobWrite` / `Delete` envelope types — and [`db/schemas/`](db/schemas/) for the canonical FlatBuffer definitions.

## Crypto Stack

| Algorithm | Purpose | Standard |
|-----------|---------|----------|
| **ML-DSA-87** | Digital signatures (blob signing, identity) | FIPS 204 |
| **ML-KEM-1024** | Key exchange (peer handshake) | FIPS 203 |
| **ChaCha20-Poly1305** | Authenticated encryption (transport, at-rest) | RFC 8439 |
| **SHA3-256** | Hashing (content addressing, namespace derivation) | FIPS 202 |
| **HKDF-SHA256** | Key derivation (session keys, encryption key) | RFC 5869 |

## Dependencies

All fetched automatically via CMake FetchContent:

- [liboqs](https://openquantumsafe.org/) -- post-quantum crypto (ML-DSA-87, ML-KEM-1024, SHA3-256)
- [libsodium](https://doc.libsodium.org/) -- symmetric crypto (ChaCha20-Poly1305, HKDF-SHA256)
- [libmdbx](https://gitflic.ru/project/erthink/libmdbx) -- ACID key-value storage
- [FlatBuffers](https://flatbuffers.dev/) -- wire format (deterministic encoding for signing)
- [Standalone Asio](https://think-async.com/Asio/) -- networking (C++20 coroutines)
- [xxHash](https://xxhash.com/) -- sync fingerprints (XXH3)
- [Catch2](https://github.com/catchorg/Catch2) -- testing
- [spdlog](https://github.com/gabime/spdlog) -- logging
- [nlohmann/json](https://github.com/nlohmann/json) -- configuration

Build environment: GCC 12+ or Clang 15+, CMake 3.20+.

## License

MIT
