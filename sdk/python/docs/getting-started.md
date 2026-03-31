# Getting Started with chromatindb

This tutorial walks you through using the chromatindb Python SDK to connect
to a relay, write and read blobs, run queries, and handle real-time
notifications. All examples assume you have a running chromatindb relay.

## Prerequisites

- Python >= 3.10
- `pip install chromatindb`
- A running chromatindb relay (host and port)

## Create an Identity

Every client needs an ML-DSA-87 identity. Your namespace (a 32-byte ID that
owns your blobs) is derived as SHA3-256 of your public key.

```python
from chromatindb import Identity

# Ephemeral (in-memory only -- lost when process exits)
identity = Identity.generate()

# Persistent (saved to disk as .key and .pub files)
identity = Identity.generate_and_save("my_identity.key")

# Load an existing identity from disk
identity = Identity.load("my_identity.key")

# Your namespace (32 bytes, derived from public key)
print(f"Namespace: {identity.namespace.hex()}")
```

## Connect to a Relay

The SDK connects to a chromatindb relay over TCP. A post-quantum handshake
(ML-KEM-1024 key exchange + ML-DSA-87 authentication) runs automatically,
establishing a ChaCha20-Poly1305 encrypted channel.

```python
import asyncio
from chromatindb import ChromatinClient, Identity

async def main():
    identity = Identity.generate()

    async with ChromatinClient.connect("192.168.1.200", 4201, identity) as client:
        await client.ping()
        print("Connected!")

asyncio.run(main())
```

The context manager sends a Goodbye message and closes the connection on exit.

## Write and Read a Blob

```python
async with ChromatinClient.connect("192.168.1.200", 4201, identity) as client:
    # Write a blob with 1-hour TTL (in seconds)
    result = await client.write_blob(b"Hello, chromatindb!", ttl=3600)
    print(f"Written: hash={result.blob_hash.hex()}, seq={result.seq_num}")

    # Read it back using your own namespace
    read = await client.read_blob(identity.namespace, result.blob_hash)
    if read is not None:
        print(f"Read: {read.data}")  # b"Hello, chromatindb!"
```

`write_blob` signs the data with your identity. The TTL is in seconds --
set `ttl=0` for permanent blobs. `read_blob` takes a namespace and blob hash,
returning `None` if the blob is not found.

## Check Blob Existence

```python
    found = await client.exists(identity.namespace, result.blob_hash)
    print(f"Exists: {found}")  # True
```

This checks existence without transferring blob data -- useful for
deduplication or verification.

## Query Blob Metadata

```python
    meta = await client.metadata(identity.namespace, result.blob_hash)
    if meta is not None:
        print(f"Size: {meta.data_size} bytes, TTL: {meta.ttl}s")
        print(f"Timestamp: {meta.timestamp}, Seq: {meta.seq_num}")
```

Metadata includes size, TTL, timestamp, sequence number, and signer public
key -- all without transferring the blob payload.

## List Namespaces

```python
    ns_list = await client.namespace_list()
    for ns in ns_list.namespaces:
        print(f"Namespace: {ns.namespace_id.hex()}, blobs: {ns.blob_count}")

    # Pagination: use cursor for next page
    if ns_list.cursor is not None:
        next_page = await client.namespace_list(after=ns_list.cursor)
```

## List Blobs in a Namespace

```python
    page = await client.list_blobs(identity.namespace)
    for blob in page.blobs:
        print(f"Hash: {blob.blob_hash.hex()}, seq: {blob.seq_num}")

    # Pagination: use cursor for next page
    if page.cursor is not None:
        next_page = await client.list_blobs(identity.namespace, after=page.cursor)
```

## Delete a Blob

Deletion creates a tombstone that replicates across the network.

```python
    del_result = await client.delete_blob(result.blob_hash)
    print(f"Deleted: tombstone={del_result.tombstone_hash.hex()}")

    # Verify it's gone
    found = await client.exists(identity.namespace, result.blob_hash)
    print(f"Exists after delete: {found}")  # False
```

Only the namespace owner can delete blobs. The tombstone is permanent
(`ttl=0`) and propagates to all nodes via sync.

## Real-time Notifications (Pub/Sub)

Subscribe to a namespace to receive notifications when new blobs arrive.

```python
    # Subscribe to your own namespace
    await client.subscribe(identity.namespace)

    # Notifications arrive as an async iterator
    # In a real application, run this in a separate task:
    #
    # async for notif in client.notifications():
    #     print(f"New blob: {notif.blob_hash.hex()}")
    #     print(f"  namespace: {notif.namespace.hex()}")
    #     print(f"  size: {notif.blob_size} bytes")
    #     print(f"  tombstone: {notif.is_tombstone}")

    await client.unsubscribe(identity.namespace)
```

A typical pattern is to run the notification listener in a background task
using `asyncio.create_task` while performing other operations in the main
coroutine. Subscriptions are automatically cleaned up when the context
manager exits.

## Error Handling

```python
from chromatindb.exceptions import (
    ChromatinError,      # Base for all SDK errors
    HandshakeError,      # PQ handshake failure (timeout, auth, protocol)
    ConnectionError,     # Timeout or disconnect (inherits ProtocolError, NOT builtin)
    ProtocolError,       # Unexpected server response
)

try:
    async with ChromatinClient.connect("bad-host", 4201, identity) as client:
        pass
except HandshakeError as e:
    print(f"Connection failed: {e}")
```

Note: `chromatindb.exceptions.ConnectionError` inherits from `ProtocolError`,
not from Python's builtin `ConnectionError`. Import it explicitly to avoid
confusion.

## Next Steps

- See the [API overview](../README.md) for the full list of available methods
- All operations are async -- use `asyncio.run()` or integrate with your async framework
- The SDK uses post-quantum cryptography (ML-DSA-87, ML-KEM-1024) automatically -- no configuration needed
- Blobs are signed with your identity and verified by the node before storage
