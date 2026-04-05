# chromatindb

A decentralized, post-quantum secure database node. Stores cryptographically signed blobs in namespaces, replicates them across peers, and makes data censorship-resistant. Standalone daemon, not a library.

## Architecture

Three-layer system:

- **Node** (`chromatindb`): Standalone C++20 daemon. Stores cryptographically signed blobs in namespaces. Every blob is verified via ML-DSA-87 signature before storage. Encrypted at rest with ChaCha20-Poly1305.
- **Relay**: PQ-authenticated message filter and UDS forwarder. Bridges client connections to the node over Unix domain sockets.
- **SDK**: Client libraries for application integration. Python SDK available (`pip install chromatindb`).

### Sync Model

Push-based event-driven sync (no polling timers):

1. Client writes blob to Node B
2. Node B sends BlobNotify to all connected peers
3. Peer checks local storage; if missing, sends BlobFetch
4. Node B responds with BlobFetchResponse containing the blob
5. Safety-net reconciliation runs every 600s as a correctness backstop
6. Full reconciliation on peer connect catches up missed blobs

```mermaid
sequenceDiagram
    participant Client
    participant NodeB
    participant NodeA

    Client->>NodeB: Data (write blob)
    NodeB->>NodeB: ingest + on_blob_ingested
    NodeB->>NodeA: BlobNotify (type 59)
    NodeB->>Client: Notification (type 21, if subscribed)
    NodeA->>NodeA: has_blob()? No
    NodeA->>NodeB: BlobFetch (type 60)
    NodeB->>NodeA: BlobFetchResponse (type 61)
    NodeA->>NodeA: ingest
```

### Keepalive

Bidirectional heartbeat -- Ping every 30s to all TCP peers, disconnect after 60s silence. Dead connection detection within one minute.

### Crypto Stack

All cryptographic operations use post-quantum algorithms:

| Algorithm | Purpose | Standard |
|-----------|---------|----------|
| **ML-DSA-87** | Digital signatures | FIPS 204 |
| **ML-KEM-1024** | Key exchange | FIPS 203 |
| **ChaCha20-Poly1305** | Authenticated encryption | RFC 8439 |
| **SHA3-256** | Hashing | FIPS 202 |
| **HKDF-SHA256** | Key derivation | RFC 5869 |

Every blob is signed by its author using ML-DSA-87. Peers establish encrypted channels using ML-KEM-1024 key exchange, then encrypt all traffic with ChaCha20-Poly1305 AEAD. No data travels in plaintext.

## Quick Start

### Prerequisites

- C++20 compiler (GCC 12+ or Clang 15+)
- CMake 3.20+
- Git

### Build

```bash
git clone <repo-url>
cd chromatin-protocol
mkdir build && cd build
cmake ..
cmake --build .
```

All dependencies are fetched automatically via CMake FetchContent.

### Run a Node

```bash
./chromatindb run --config config.json
```

See [db/README.md](db/README.md) for full configuration reference and deployment options.

### Connect with Python SDK

```bash
pip install chromatindb
```

```python
import asyncio
from chromatindb import ChromatinClient, Identity

identity = Identity.generate()

async def main():
    async with ChromatinClient.connect("localhost", 4201, identity) as client:
        result = await client.write_blob(b"Hello, chromatindb!", ttl=3600)
        blob = await client.read_blob(identity.namespace, result.blob_hash)
        print(blob.data)  # b"Hello, chromatindb!"

asyncio.run(main())
```

## Documentation

| Document | Description |
|----------|-------------|
| [Protocol Specification](db/PROTOCOL.md) | Wire protocol, message types, byte formats |
| [Node README](db/README.md) | Build, configuration, testing, deployment |
| [Python SDK](sdk/python/README.md) | API reference, installation |
| [Getting Started Tutorial](sdk/python/docs/getting-started.md) | Step-by-step walkthrough |

## License

MIT
