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
    async with ChromatinClient.connect([("relay-host", 4201)], identity) as client:
        # Plaintext write/read
        result = await client.write_blob(b"Hello, chromatindb!", ttl=3600)
        blob = await client.read_blob(identity.namespace, result.blob_hash)
        print(blob.data)  # b"Hello, chromatindb!"

        # Encrypted write/read (self-only, zero-knowledge storage)
        enc_result = await client.write_encrypted(b"Secret data", ttl=3600)
        plaintext = await client.read_encrypted(identity.namespace, enc_result.blob_hash)
        print(plaintext)  # b"Secret data"

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

### Encryption

**Encryption Operations:**

| Method | Description | Returns |
|--------|-------------|---------|
| `write_encrypted(data, ttl, recipients=)` | Encrypt and write a blob (self-only if no recipients) | `WriteResult` |
| `read_encrypted(namespace, blob_hash)` | Fetch and decrypt an encrypted blob | `bytes \| None` |
| `write_to_group(data, group_name, directory, ttl)` | Encrypt for all group members and write | `WriteResult` |

**Directory & Groups:**

| Method | Description | Returns |
|--------|-------------|---------|
| `Directory(client, identity)` | Create directory (admin mode) | `Directory` |
| `Directory(client, identity, directory_namespace=)` | Create directory (user mode) | `Directory` |
| `directory.delegate(identity)` | Grant write access to a user | `WriteResult` |
| `directory.register(display_name)` | Self-register in the directory | `bytes` |
| `directory.list_users()` | List all registered users | `list[DirectoryEntry]` |
| `directory.get_user(display_name)` | Look up user by name | `DirectoryEntry \| None` |
| `directory.get_user_by_pubkey(pubkey_hash)` | Look up user by pubkey hash | `DirectoryEntry \| None` |
| `directory.create_group(name, members)` | Create a named group (admin) | `WriteResult` |
| `directory.add_member(group_name, member)` | Add member to group (admin) | `WriteResult` |
| `directory.remove_member(group_name, member)` | Remove member from group (admin) | `WriteResult` |
| `directory.list_groups()` | List all groups | `list[GroupEntry]` |
| `directory.get_group(group_name)` | Look up group by name | `GroupEntry \| None` |
| `directory.refresh()` | Force cache invalidation | `None` |

`Directory`, `DirectoryEntry`, and `GroupEntry` are all importable from `chromatindb`.

### Utility

| Method | Description | Returns |
|--------|-------------|---------|
| `ping()` | Verify connection is alive | `None` |

### Connection Resilience

| Method/Property | Description | Type |
|----------------|-------------|------|
| `auto_reconnect` | Enable auto-reconnect on connection loss | `bool` (default: `True`) |
| `on_disconnect` | Callback when connection is lost | `() -> None \| Awaitable[None]` |
| `on_reconnect` | Callback after successful reconnect | `(cycle_count: int, downtime: float, host: str, port: int) -> None \| Awaitable[None]` |
| `connection_state` | Current connection state | `ConnectionState` (property) |
| `current_relay` | Currently connected relay address | `tuple[str, int]` (property) |
| `wait_connected(timeout=None)` | Wait until connected (after reconnect) | `async -> bool` |

**ConnectionState enum:**

| State | Description |
|-------|-------------|
| `DISCONNECTED` | Not connected (initial or after connection loss) |
| `CONNECTING` | Reconnection attempt in progress |
| `CONNECTED` | Active connection established |
| `CLOSING` | Shutdown initiated via `close()` |

**Reconnect behavior:**

- Cycles through all relays in list order on connection failure
- Jittered exponential backoff between full relay cycles: 1s base, 30s cap (AWS Full Jitter)
- No delay between individual relay attempts within a cycle
- Subscriptions automatically re-subscribed after reconnect
- Initial connection rotates through all relays before raising (no auto-reconnect until first success)
- `close()` suppresses reconnection (intentional disconnect)
- Pending operations fail with `ConnectionError` during reconnect

**Example:**

```python
async def on_disconnect():
    print("Connection lost, reconnecting...")

async def on_reconnect(cycle_count: int, downtime: float, host: str, port: int):
    print(f"Reconnected to {host}:{port} after {cycle_count} cycle(s) ({downtime:.1f}s)")

async with ChromatinClient.connect(
    [("relay-host", 4201)], identity,
    auto_reconnect=True,
    on_disconnect=on_disconnect,
    on_reconnect=on_reconnect,
) as client:
    print(client.connection_state)  # ConnectionState.CONNECTED
    print(client.current_relay)     # ("relay-host", 4201)
    connected = await client.wait_connected(timeout=10.0)
```

### Multi-Relay Failover

Pass multiple relays for automatic failover. The SDK tries each relay in list
order. If the current relay goes down during operation, auto-reconnect cycles
through all relays with jittered backoff between full cycles.

```python
# Connect with failover relays (tries each in order)
async with ChromatinClient.connect(
    [("relay1.example.com", 4201), ("relay2.example.com", 4201)],
    identity,
) as conn:
    await conn.ping()
    print(f"Connected to {conn.current_relay}")
```

## Tutorial

See [Getting Started Tutorial](docs/getting-started.md) for a complete walkthrough
from installation through writing, reading, querying, and deleting blobs.

## License

MIT
