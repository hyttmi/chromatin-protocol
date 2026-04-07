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

## Encrypted Write and Read

chromatindb supports client-side envelope encryption. Data is encrypted
before leaving your machine -- the node stores ciphertext only
(zero-knowledge storage). `Identity.generate()` automatically creates
ML-KEM-1024 encryption keypairs alongside ML-DSA-87 signing keys.

```python
async with ChromatinClient.connect("192.168.1.200", 4201, identity) as client:
    # Encrypt to self only (no recipients = self-only)
    result = await client.write_encrypted(b"My secret data", ttl=3600)
    print(f"Encrypted blob: {result.blob_hash.hex()}")

    # Decrypt (only the sender can read it)
    plaintext = await client.read_encrypted(identity.namespace, result.blob_hash)
    print(plaintext)  # b"My secret data"
```

The blob stored on the node is an opaque envelope -- the node never sees
plaintext. Content-addressed deduplication does not apply to encrypted blobs
because identical plaintext produces different ciphertext each time.

To encrypt for multiple recipients, pass a list of identities:

```python
    # Encrypt for multiple recipients
    alice = Identity.generate()
    bob = Identity.generate()

    result = await client.write_encrypted(
        b"Shared secret", ttl=3600, recipients=[alice, bob]
    )

    # Any recipient (or the sender) can decrypt
    # Others get NotARecipientError
```

The sender is always auto-included as a recipient. Each recipient adds
1648 bytes of overhead to the envelope.

## Directory Setup

Before encrypting for other users, you need a directory -- a shared registry
of encryption public keys. The directory admin creates it, then delegates
write access so users can self-register.

```python
from chromatindb import Directory

# Admin creates a directory (backed by the admin's namespace)
admin = Identity.generate()

async with ChromatinClient.connect("192.168.1.200", 4201, admin) as client:
    directory = Directory(client, admin)

    # Delegate write access to a user so they can register
    user_identity = Identity.generate()
    await directory.delegate(user_identity)
```

The directory is a logical layer over the existing blob store. It uses the
admin's namespace for storage and delegation for access control.

## User Registration

Users register by publishing a UserEntry blob containing their signing key,
encryption key, and display name. The KEM public key is signed with ML-DSA-87
to prevent key substitution attacks.

```python
# User connects and registers in the directory
async with ChromatinClient.connect("192.168.1.200", 4201, user_identity) as client:
    user_dir = Directory(client, user_identity, directory_namespace=admin.namespace)

    await user_dir.register("alice")

    # List all registered users
    users = await user_dir.list_users()
    for u in users:
        print(f"User: {u.display_name}")
```

## Groups and Group Encryption

Groups are named member lists. The directory admin creates groups and manages
membership. Any user can then encrypt data for an entire group with a single
call.

```python
# Admin creates a group (needs admin client connection)
async with ChromatinClient.connect("192.168.1.200", 4201, admin) as client:
    admin_dir = Directory(client, admin)
    await admin_dir.create_group("engineering", members=[user_identity])

    # Encrypt for all group members
    result = await client.write_to_group(
        b"Team announcement", "engineering", admin_dir, ttl=3600
    )
    print(f"Group blob: {result.blob_hash.hex()}")

    # List groups
    groups = await admin_dir.list_groups()
    for g in groups:
        print(f"Group: {g.name}, members: {len(g.members)}")
```

`write_to_group` resolves group membership at call time. Members are
identified by SHA3-256 hash of their signing public key.

## Error Handling

```python
from chromatindb.exceptions import (
    ChromatinError,          # Base for all SDK errors
    HandshakeError,          # PQ handshake failure (timeout, auth, protocol)
    ConnectionError,         # Timeout or disconnect (inherits ProtocolError, NOT builtin)
    ProtocolError,           # Unexpected server response
    NotARecipientError,      # Not an envelope recipient
    MalformedEnvelopeError,  # Invalid envelope format
    DirectoryError,          # Group/directory operation failed
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

## Connection Resilience

The SDK automatically reconnects when the relay connection drops. Auto-reconnect
is enabled by default -- no configuration needed for basic resilience.

### Auto-Reconnect with Callbacks

Use `on_disconnect` and `on_reconnect` callbacks to track connection state:

```python
async def on_disconnect():
    print("Connection lost! Reconnecting...")

async def on_reconnect(attempt: int, downtime: float):
    print(f"Reconnected after {attempt} attempts ({downtime:.1f}s downtime)")
    # Good place to catch up on missed data

async with ChromatinClient.connect(
    "192.168.1.200", 4201, identity,
    on_disconnect=on_disconnect,
    on_reconnect=on_reconnect,
) as client:
    # Your application logic here
    await client.subscribe(identity.namespace)
    async for notif in client.notifications():
        print(f"New blob: {notif.blob_hash.hex()}")
```

The SDK uses jittered exponential backoff (1s base, 30s cap) and retries
indefinitely until `close()` is called. All active subscriptions are
automatically re-subscribed after a successful reconnect.

### Connection State

Check the current connection state at any time:

```python
from chromatindb import ConnectionState

print(client.connection_state)
# ConnectionState.CONNECTED, DISCONNECTED, CONNECTING, or CLOSING
```

### Waiting for Reconnection

If another part of your application detects the disconnect, use
`wait_connected()` to block until the connection is restored:

```python
# Block until reconnected (or timeout)
connected = await client.wait_connected(timeout=10.0)
if not connected:
    print("Still disconnected after 10s")
```

### Catch-Up Pattern

After reconnecting, your application may have missed notifications. Use
the `on_reconnect` callback to re-read data:

```python
async def on_reconnect(attempt: int, downtime: float):
    # Re-read any data that may have arrived during downtime
    blobs = await client.list_blobs(identity.namespace, limit=50)
    for ref in blobs.refs:
        print(f"Blob: {ref.blob_hash.hex()}")

async with ChromatinClient.connect(
    "192.168.1.200", 4201, identity,
    on_reconnect=on_reconnect,
) as client:
    await client.subscribe(identity.namespace)
    async for notif in client.notifications():
        process(notif)
```

### Disabling Auto-Reconnect

Pass `auto_reconnect=False` if you want to handle reconnection yourself:

```python
async with ChromatinClient.connect(
    "192.168.1.200", 4201, identity,
    auto_reconnect=False,
) as client:
    # Connection loss will raise ConnectionError
    pass
```

Note: Calling `close()` never triggers auto-reconnect, regardless of the
`auto_reconnect` setting. Intentional disconnection is always clean.

## Monitoring with Prometheus

For operators running chromatindb nodes, the built-in Prometheus endpoint
exposes health metrics for automated monitoring.

Enable it in your node's config.json:

```json
{
  "metrics_bind": "127.0.0.1:9090"
}
```

Then configure Prometheus to scrape it:

```yaml
scrape_configs:
  - job_name: 'chromatindb'
    static_configs:
      - targets: ['127.0.0.1:9090']
```

Available metrics include peer count, blob count, storage bytes, sync
rounds, ingestion counters, and uptime. See
[PROTOCOL.md](../../db/PROTOCOL.md) for the full list.

The endpoint is disabled by default (empty `metrics_bind`) and listens on
localhost only when enabled. Reload with SIGHUP to toggle without restart.

## Delegation Revocation

Namespace owners can revoke a delegate's write access. Revocation tombstones
the delegation blob -- the node immediately rejects subsequent writes from the
revoked delegate.

```python
from chromatindb import ChromatinClient, Identity, Directory
from chromatindb.exceptions import DelegationNotFoundError

admin = Identity.load("admin.key")
delegate = Identity.load("delegate.key")

async with ChromatinClient.connect("192.168.1.200", 4201, admin) as client:
    directory = Directory(client, admin)

    # Revoke a delegate's write access
    result = await directory.revoke_delegation(delegate)
    print(f"Revoked: tombstone={result.tombstone_hash.hex()}")

    # List remaining active delegates
    delegates = await directory.list_delegates()
    for d in delegates:
        print(f"Active delegate: {d.delegate_pk_hash.hex()}")
```

After revocation, the tombstone propagates to other nodes via sync. Connected
peers see the revocation near-instantly (via BlobNotify). Disconnected peers
see it on their next sync round (up to the safety-net interval, default 600s).

`revoke_delegation()` raises `DelegationNotFoundError` if no active delegation
exists for the given identity.

## KEM Key Rotation

Rotate your ML-KEM-1024 encryption keypair so that a compromised key cannot
decrypt future data. Old keys are retained in a key ring for decrypting
historical data.

```python
from chromatindb import ChromatinClient, Identity, Directory

identity = Identity.load("my_identity.key")

# Rotate KEM keypair (offline operation, no network needed)
identity.rotate_kem("my_identity.key")
print(f"New key version: {identity.key_version}")

# Publish the new key to the directory
async with ChromatinClient.connect("192.168.1.200", 4201, identity) as client:
    directory = Directory(client, identity, directory_namespace=admin_ns)
    await directory.register("alice")
```

After rotation:
- **Senders** who call `resolve_recipient("alice")` get the latest KEM public
  key and encrypt to it automatically.
- **Decryption** checks all keys in the ring -- data encrypted under old or
  new keys decrypts transparently via `read_encrypted()`.
- **Key files** are saved as numbered files (`my_identity.kem.0`,
  `my_identity.kem.1`, etc.) alongside the canonical `.kem` file.

Old keys are never deleted. Discarding a historical key permanently prevents
decryption of any data encrypted under that key.

## Group Membership Management

Directory admins can add and remove group members. After removal, new
encrypted group writes exclude the removed member.

```python
admin = Identity.load("admin.key")

async with ChromatinClient.connect("192.168.1.200", 4201, admin) as client:
    directory = Directory(client, admin)

    # Create a group with initial members
    await directory.create_group("engineering", members=[alice, bob, carol])

    # Add a member later
    await directory.add_member("engineering", dave)

    # Remove a member
    await directory.remove_member("engineering", bob)

    # New group writes exclude removed member
    result = await client.write_to_group(
        b"Team update", "engineering", directory, ttl=3600
    )
```

After `remove_member()`:
- `write_to_group()` forces a directory cache refresh before resolving
  membership, ensuring the removed member is excluded from encryption.
- The removed member gets `NotARecipientError` when trying to decrypt new
  group data.
- Old data remains readable -- removal is forward-only. The removed member
  can still decrypt any data encrypted before their removal.

## Next Steps

- See the [API overview](../README.md) for the full list of available methods
- All operations are async -- use `asyncio.run()` or integrate with your async framework
- The SDK uses post-quantum cryptography (ML-DSA-87, ML-KEM-1024) automatically -- no configuration needed
- Blobs are signed with your identity and verified by the node before storage
- Encrypt data with `write_encrypted()` for zero-knowledge storage
- Encrypted blobs are automatically Brotli-compressed before encryption for payloads >= 256 bytes (transparent, enabled by default)
- Auto-reconnect handles connection drops transparently -- customize with `on_disconnect` and `on_reconnect` callbacks
- Set up a directory for user discovery and group management
- Revoke delegate access with `directory.revoke_delegation()` -- tombstone-based, propagates via sync
- Rotate encryption keys with `identity.rotate_kem()` -- old data stays readable
- Manage group membership with `directory.add_member()` and `directory.remove_member()`
- See [PROTOCOL.md](../../db/PROTOCOL.md) for the envelope binary format specification
