# chromatindb

Python SDK for chromatindb -- a PQ-secure decentralized database node.

Connect to a chromatindb relay with post-quantum authenticated encryption
(ML-KEM-1024 + ML-DSA-87), write and read signed blobs, query namespaces,
and receive real-time notifications. All transport is AEAD-encrypted with
ChaCha20-Poly1305.

## Installation

```bash
pip install chromatindb
```

Requires Python >= 3.10.

## Quick Start

```python
import asyncio
from chromatindb import ChromatinClient, Identity

identity = Identity.generate()

async def main():
    async with ChromatinClient.connect("relay-host", 4201, identity) as client:
        result = await client.write_blob(b"Hello, chromatindb!", ttl=3600)
        blob = await client.read_blob(identity.namespace, result.blob_hash)
        print(blob.data)  # b"Hello, chromatindb!"

asyncio.run(main())
```

## API Overview

### Data Operations

| Method | Description | Returns |
|--------|-------------|---------|
| `write_blob(data, ttl)` | Write a signed blob | `WriteResult` |
| `read_blob(namespace, blob_hash)` | Read a blob by hash | `ReadResult \| None` |
| `delete_blob(blob_hash)` | Delete via tombstone | `DeleteResult` |
| `list_blobs(namespace, after=, limit=)` | Paginated blob listing | `ListPage` |
| `exists(namespace, blob_hash)` | Check blob existence | `bool` |

### Query Operations

| Method | Description | Returns |
|--------|-------------|---------|
| `metadata(namespace, blob_hash)` | Blob metadata without payload | `MetadataResult \| None` |
| `batch_exists(namespace, hashes)` | Batch existence check | `dict[bytes, bool]` |
| `batch_read(namespace, hashes, cap_bytes=)` | Multi-blob fetch | `BatchReadResult` |
| `time_range(namespace, start_ts, end_ts, limit=)` | Timestamp-filtered query | `TimeRangeResult` |
| `namespace_list(after=, limit=)` | Paginated namespace enumeration | `NamespaceListResult` |
| `namespace_stats(namespace)` | Per-namespace statistics | `NamespaceStats` |
| `storage_status()` | Node disk usage and quotas | `StorageStatus` |
| `node_info()` | Version, capabilities, peers | `NodeInfo` |
| `peer_info()` | Peer connection details (trust-gated) | `PeerInfo` |
| `delegation_list(namespace)` | Active delegations | `DelegationList` |

### Pub/Sub

| Method | Description | Returns |
|--------|-------------|---------|
| `subscribe(namespace)` | Subscribe to namespace notifications | `None` |
| `unsubscribe(namespace)` | Unsubscribe from notifications | `None` |
| `notifications()` | Async iterator for real-time events | `AsyncIterator[Notification]` |

### Utility

| Method | Description | Returns |
|--------|-------------|---------|
| `ping()` | Verify connection is alive | `None` |

## Tutorial

See [Getting Started Tutorial](docs/getting-started.md) for a complete walkthrough
from installation through writing, reading, querying, and deleting blobs.

## License

MIT
