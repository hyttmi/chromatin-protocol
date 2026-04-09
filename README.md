# chromatindb

A lightweight C++20 post-quantum secure blob storage node.

chromatindb stores cryptographically signed blobs organized into namespaces, replicates them across peers via O(diff) set reconciliation, and makes data censorship-resistant. Every blob is verified before storage, encrypted at rest, and encrypted in transit using post-quantum algorithms. Standalone daemon, not a library.

 * Post-quantum crypto: ML-DSA-87 signatures, ML-KEM-1024 key exchange, ChaCha20-Poly1305 AEAD
 * Content-addressed storage with ACID guarantees (libmdbx)
 * O(diff) sync via XOR-fingerprint set reconciliation
 * Namespace ownership via `SHA3-256(public_key)`
 * Namespace delegation (owner grants write access to other identities)
 * Writer-controlled per-blob TTL with node-enforced expiry
 * Pub/sub notifications for real-time change tracking
 * Per-namespace storage quotas and per-connection rate limiting
 * Prometheus `/metrics` endpoint
 * Encryption at rest (ChaCha20-Poly1305, HKDF-derived key)
 * SIGHUP hot-reload for ACLs, quotas, rate limits, metrics
 * Clean under AddressSanitizer, ThreadSanitizer, and UndefinedBehaviorSanitizer

## Documentation

See [PROTOCOL.md](db/PROTOCOL.md) for the full wire protocol specification (62 message types, byte-level formats, handshake sequences).

See [db/README.md](db/README.md) for configuration reference, deployment guide, and testing.

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

chromatindb uses a binary protocol built on FlatBuffers. The wire schemas are in [`db/schemas/`](db/schemas/) -- use them to generate bindings for any language.

After connecting and completing the PQ handshake, send a `NodeInfoRequest` to discover the node's version and supported message types. See [PROTOCOL.md](db/PROTOCOL.md) for the handshake sequence, message formats, and signing rules.

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
