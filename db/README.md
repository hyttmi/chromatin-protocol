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
| **HKDF-SHA256** | Key derivation (session keys, at-rest encryption key) | RFC 5869 |

Every blob is cryptographically signed by its author using ML-DSA-87. Peers establish encrypted channels using post-quantum key exchange (ML-KEM-1024), then encrypt all traffic with ChaCha20-Poly1305 AEAD. No data travels in plaintext.

## Architecture

Each node generates an ML-DSA-87 keypair as its identity. The node's **namespace** is derived as `SHA3-256(public_key)`, providing a unique, verifiable identifier. **Blobs** are signed data units that belong to a namespace; each blob carries a signature, TTL (writer-controlled, per-blob), and timestamp. Blobs are content-addressed by their SHA3-256 hash and stored in libmdbx, an ACID key-value store.

**Sync** works via range-based set reconciliation: peers exchange XOR fingerprints over sorted hash ranges, recursively splitting mismatched ranges until differences are isolated, then transfer only the missing blobs. Sync cost is O(differences), not O(total blobs). One blob is in flight at a time per connection, bounding memory usage. **Transport** security begins with an ML-KEM-1024 handshake that establishes a shared secret, from which ChaCha20-Poly1305 session keys are derived. All subsequent messages are AEAD-encrypted. **Encryption at rest** protects stored blob payloads with ChaCha20-Poly1305 using a key derived from a node-local master key via HKDF-SHA256.

**Deletion** uses tombstones -- signed markers that permanently remove a target blob and replicate across the network via sync. Tombstones MUST have TTL=0 (permanent) -- the node rejects tombstones with non-zero TTL at ingest. **Namespace delegation** allows owners to grant write access to other identities by creating signed delegation blobs; revocation is done by tombstoning the delegation blob. **Pub/sub notifications** let connected peers subscribe to namespaces and receive real-time metadata when blobs are ingested or deleted. **Storage capacity management** enforces a configurable disk limit and signals peers when the node is full. **Rate limiting** protects against write-flooding abuse by enforcing per-connection throughput limits on Data and Delete messages.

## Building

### Prerequisites

- C++20 compiler (GCC 12+ or Clang 15+)
- CMake 3.20+
- Git

### Build

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

All dependencies are fetched automatically via CMake FetchContent on first build. This includes liboqs, libsodium, FlatBuffers, libmdbx, Standalone Asio, Catch2, spdlog, nlohmann/json, and xxHash. The first build takes longer as dependencies are downloaded and compiled.

### Sanitizer Builds

Build with AddressSanitizer, ThreadSanitizer, or UndefinedBehaviorSanitizer:

```bash
cmake .. -DSANITIZER=asan   # AddressSanitizer
cmake .. -DSANITIZER=tsan   # ThreadSanitizer
cmake .. -DSANITIZER=ubsan  # UndefinedBehaviorSanitizer
cmake --build .
```

The codebase is clean under all three sanitizers.

## Testing

### Unit Tests

647 unit tests covering all subsystems (crypto, storage, sync, ACL, delegation, pub/sub, rate limiting, quotas, config validation):

```bash
cd build
ctest --output-on-failure
```

### Docker Integration Tests

49 integration test scripts across 12 categories running real multi-node topologies in Docker containers:

```bash
cd deploy
./run-integration-tests.sh
```

Categories include: basic sync, ACL enforcement, delegation, deletion propagation, pub/sub, storage limits, rate limiting, namespace quotas, trusted peers, crash recovery, concurrent sync, and large blob transfer.

### Stress, Chaos & Fuzz Testing

- **Stress testing:** 5-node cluster with continuous SIGKILL churn, verifying data consistency after node recovery
- **Chaos testing:** 1000-namespace scaling with concurrent writes and deletions across multiple nodes
- **Protocol fuzzing:** Random byte injection and malformed message testing against the wire protocol parser

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

### Backup Database

```bash
chromatindb backup /backups/chromatindb.dat [--data-dir <path>]
```

Creates a live compacted copy of the database at the given path. Does not block reads or writes.

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
  "allowed_client_keys": [],
  "allowed_peer_keys": [],
  "trusted_peers": [],
  "max_peers": 32,
  "safety_net_interval_seconds": 600,
  "log_level": "info",
  "max_storage_bytes": 0,
  "rate_limit_bytes_per_sec": 0,
  "rate_limit_burst": 0,
  "sync_namespaces": [],
  "full_resync_interval": 10,
  "cursor_stale_seconds": 3600,
  "namespace_quota_bytes": 0,
  "namespace_quota_count": 0,
  "namespace_quotas": {},
  "worker_threads": 0,
  "sync_cooldown_seconds": 30,
  "max_sync_sessions": 1,
  "log_file": "",
  "log_max_size_mb": 10,
  "log_max_files": 3,
  "log_format": "text",
  "inactivity_timeout_seconds": 120,
  "expiry_scan_interval_seconds": 60,
  "compaction_interval_hours": 6,
  "uds_path": ""
}
```

- **bind_address** -- address and port to listen on (default: `0.0.0.0:4200`)
- **data_dir** -- directory for identity keys and blob storage (default: `./data`)
- **bootstrap_peers** -- list of `host:port` strings to connect to on startup
- **allowed_client_keys** -- namespace hashes (hex) of clients allowed to connect via UDS; empty means no restriction
- **allowed_peer_keys** -- namespace hashes (hex) of peers allowed to connect via TCP for sync; empty means any peer that completes PQ handshake can sync
- **trusted_peers** -- IP addresses (no ports) whose connections use a lightweight handshake without ML-KEM-1024 key exchange; localhost (127.0.0.1, ::1) is always trusted implicitly (default: `[]`)
- **max_peers** -- maximum number of simultaneous peer connections (default: `32`)
- **safety_net_interval_seconds** -- interval between safety-net reconciliation rounds in seconds; correctness backstop (default: `600`, minimum: `60`)
- **log_level** -- log verbosity (default: `info`)
- **max_storage_bytes** -- global storage capacity limit in bytes; the node rejects new blobs when exceeded and sends a StorageFull signal to peers (default: `0` = unlimited)
- **rate_limit_bytes_per_sec** -- per-connection write rate limit for Data and Delete messages in bytes per second; peers exceeding the limit are disconnected immediately (default: `0` = disabled)
- **rate_limit_burst** -- token bucket burst capacity in bytes; allows short bursts above the sustained rate (default: `0` = disabled)
- **sync_namespaces** -- list of namespace hashes (64-char hex) to replicate; the node only syncs listed namespaces and silently drops Data/Delete messages for unlisted namespaces (default: `[]` = replicate all)
- **full_resync_interval** -- full reconciliation resync every Nth sync round, overriding cursor-based skip (default: `10`)
- **cursor_stale_seconds** -- force full resync with a peer after this many seconds since last sync (default: `3600`)
- **namespace_quota_bytes** -- global default maximum bytes per namespace; the node rejects new blobs when a namespace exceeds this limit and sends QuotaExceeded to the peer (default: `0` = unlimited)
- **namespace_quota_count** -- global default maximum blob count per namespace (default: `0` = unlimited)
- **namespace_quotas** -- per-namespace quota overrides as a JSON object mapping namespace hash (64-char hex) to `{"max_bytes": N, "max_count": N}`; overrides the global defaults for listed namespaces (default: `{}` = use global defaults)
- **worker_threads** -- number of thread pool workers for CPU-bound crypto offload (ML-DSA-87 verify, SHA3-256 hash); clamped to `hardware_concurrency()` if set higher (default: `0` = auto-detect via `hardware_concurrency()`)
- **sync_cooldown_seconds** -- minimum seconds between incoming sync requests per peer; peers syncing more frequently receive SyncRejected (default: `30`, `0` = disabled)
- **max_sync_sessions** -- maximum concurrent sync sessions per peer (default: `1`)
- **log_file** -- path for rotating log file output; the node logs to both console and file when set (default: `""` = console only)
- **log_max_size_mb** -- maximum size per log file in MiB before rotation (default: `10`)
- **log_max_files** -- maximum number of rotated log files to retain (default: `3`)
- **log_format** -- log output format: `"text"` for human-readable or `"json"` for structured machine-parseable output (default: `"text"`)
- **inactivity_timeout_seconds** -- disconnect peers that send no messages within this many seconds; set to `0` to disable (default: `120`, minimum `30` when enabled)
- **expiry_scan_interval_seconds** -- interval between periodic expired-blob scan passes in seconds; minimum is 10 seconds (default: `60`)
- **compaction_interval_hours** -- interval between sync cursor compaction passes in hours; set to `0` to disable (default: `6`, minimum `1` when enabled)
- **uds_path** -- path for Unix domain socket listener; external services connect via this path for trusted local communication (default: `""` = disabled)

## Signals

chromatindb responds to the following Unix signals at runtime:

- **SIGTERM** -- Graceful shutdown. Stops accepting new connections, drains in-flight coroutines, saves the persistent peer list, and exits cleanly. The shutdown is bounded; if draining takes too long, the process exits with a non-zero exit code.

- **SIGHUP** -- Configuration reload. Re-reads the config file and updates `allowed_client_keys`, `allowed_peer_keys`, `trusted_peers`, `rate_limit_bytes_per_sec`, `rate_limit_burst`, `sync_namespaces`, `full_resync_interval`, `cursor_stale_seconds`, `namespace_quota_bytes`, `namespace_quota_count`, and `namespace_quotas` without restarting the daemon. Peers that are no longer in the updated access control lists are disconnected immediately. SIGHUP also resets all ACL reconnection backoff counters, allowing immediate retry to peers that were previously rejecting connections.

- **SIGUSR1** -- Metrics dump. Logs a snapshot of current runtime metrics via spdlog, including: active connections, storage bytes used, total blobs ingested, total sync rounds completed, total rejections, rate-limited disconnections, and uptime.

Metrics are also logged automatically every 60 seconds without requiring a signal.

## Wire Protocol

chromatindb uses a binary protocol built on [FlatBuffers](https://flatbuffers.dev/). Every message after the initial handshake is a `TransportMessage` envelope containing a one-byte type field and a variable-length payload, encrypted with ChaCha20-Poly1305 AEAD.

Frames are length-prefixed: a 4-byte big-endian `uint32` declares the ciphertext length, followed by that many bytes of AEAD ciphertext (including the 16-byte authentication tag). Each direction maintains a separate nonce counter starting at zero. The maximum frame size is 110 MiB.

The protocol defines 62 message types covering handshake, keepalive, blob storage, sync (with range-based set reconciliation), peer exchange, deletion, pub/sub, storage signaling, trusted peer handshake, namespace quota enforcement, sync rate limiting, client queries, node capability discovery, and extended query operations (namespace enumeration, storage status, blob metadata, batch operations, peer info, time-range queries). Wire schemas are in [`schemas/`](schemas/) for client codegen in any language. See [PROTOCOL.md](PROTOCOL.md) for a complete walkthrough of the wire protocol, including the PQ handshake sequence, blob signing format, sync phases, and all message types.

## TTL Enforcement

Every blob has a TTL (time-to-live) in seconds. `TTL = 0` means permanent.

**Tombstones MUST have TTL = 0.** Tombstones are permanent delete markers. A tombstone
with a non-zero TTL is rejected at ingest. This is enforced by the node -- tombstones
must outlive the blobs they delete.

Expired blobs are never served through any query, fetch, or sync path. The node
filters expired blobs at every exit point. Storage-level cleanup happens asynchronously
via the expiry scanner.

Expiry arithmetic uses saturation: if `timestamp + ttl` would overflow, the blob is
treated as effectively permanent (will never expire).

See the [TTL Enforcement](PROTOCOL.md#ttl-enforcement) section in PROTOCOL.md for
the full specification, including per-handler behavior tables.

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

Populate `allowed_client_keys` and/or `allowed_peer_keys` with the namespace hashes of permitted connections. Client keys restrict UDS connections; peer keys restrict TCP connections (from other nodes). Each list is independent -- a node can restrict which clients connect via relay while keeping peer sync open (or vice versa).

```json
{
  "allowed_client_keys": [
    "a1b2c3d4e5f6...64-char-hex-namespace-hash..."
  ],
  "allowed_peer_keys": [
    "a1b2c3d4e5f6...64-char-hex-namespace-hash...",
    "f6e5d4c3b2a1...64-char-hex-namespace-hash..."
  ]
}
```

The node enters closed mode for each list independently when the list is non-empty. Reload the config at runtime by sending `SIGHUP` -- the node re-reads both key lists without restarting.

### Rate-Limited Public Node

For an open node that protects against write-flooding abuse, set storage and rate limits:

```json
{
  "bind_address": "0.0.0.0:4200",
  "data_dir": "./data",
  "bootstrap_peers": ["seed1.example.com:4200"],
  "max_peers": 64,
  "max_storage_bytes": 10737418240,
  "rate_limit_bytes_per_sec": 1048576,
  "rate_limit_burst": 5242880
}
```

This configures a node with 10 GiB storage capacity, 1 MiB/s sustained write rate per connection, and 5 MiB burst tolerance. Peers exceeding the rate are disconnected immediately. When storage is full, the node signals StorageFull to connected peers, which suppress further sync pushes until reconnection.

### Trusted Local Peers

For multi-node setups on a trusted LAN, skip the PQ handshake overhead between nodes:

```json
{
  "data_dir": "./data",
  "bootstrap_peers": ["192.168.1.10:4200", "192.168.1.11:4200"],
  "trusted_peers": ["192.168.1.10", "192.168.1.11"]
}
```

Connections between listed peers use a lightweight handshake with ML-DSA-87 identity verification but no ML-KEM-1024 key exchange. Localhost connections are always trusted implicitly. Reload the trusted peer list at runtime with `SIGHUP`.

### Logging Configuration

Production monitoring setup with JSON-formatted rotating file logs:

```json
{
  "data_dir": "./data",
  "log_level": "info",
  "log_file": "/var/log/chromatindb/node.log",
  "log_max_size_mb": 50,
  "log_max_files": 5,
  "log_format": "json"
}
```

### Resilient Node

Hostile network configuration with auto-reconnect, dead peer detection, and rate limiting:

```json
{
  "data_dir": "./data",
  "bootstrap_peers": ["peer1.example.com:4200", "peer2.example.com:4200"],
  "max_peers": 64,
  "inactivity_timeout_seconds": 120,
  "sync_cooldown_seconds": 30,
  "rate_limit_bytes_per_sec": 1048576,
  "rate_limit_burst": 5242880
}
```

## Deployment

chromatindb includes a production deployment kit in the `dist/` directory for bare-metal Linux systems.

### Quick Start

Build the binary, then run the install script:

```bash
mkdir build && cd build
cmake ..
cmake --build .
cd ..
sudo dist/install.sh build/db/chromatindb
```

The install script creates a `chromatindb` system user and group, installs the binary to `/usr/local/bin`, copies default config to `/etc/chromatindb`, installs the systemd unit, creates data and log directories, and generates an identity key.

### Start Services

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now chromatindb
```

### FHS Paths

| Artifact | Location |
|----------|----------|
| Binary | `/usr/local/bin/chromatindb` |
| Config | `/etc/chromatindb/node.json` |
| Data directory | `/var/lib/chromatindb` |
| Log directory | `/var/log/chromatindb` |
| Runtime (UDS) | `/run/chromatindb` |
| Systemd unit | `/usr/lib/systemd/system/chromatindb.service` |

### Security Hardening

The systemd unit includes hardening directives: `ProtectSystem=strict`, `NoNewPrivileges=yes`, `MemoryDenyWriteExecute=yes`, `PrivateTmp=yes`, `PrivateDevices=yes`, `ProtectHome=yes`, `ProtectKernelTunables=yes`, `ProtectKernelModules=yes`, `ProtectControlGroups=yes`, `ProtectClock=yes`, `ProtectHostname=yes`, `RestrictRealtime=yes`, `RestrictSUIDSGID=yes`, `RestrictNamespaces=yes`, `LockPersonality=yes`, `SystemCallArchitectures=native`.

### Reinstall / Upgrade

Running `install.sh` again is safe -- config files are preserved (not overwritten), binaries and service files are updated. Keys are only generated if missing.

### Uninstall

```bash
sudo dist/install.sh --uninstall
```

Stops and disables services, removes binaries, systemd units, and sysusers/tmpfiles configs. Data and config directories are preserved.

## Features

**Signed Blob Storage** -- Every blob is cryptographically signed by its author using ML-DSA-87. The node verifies the signature against the blob's namespace before accepting it. Blobs are content-addressed by SHA3-256 hash and stored in an ACID key-value store (libmdbx).

**Post-Quantum Transport** -- All peer-to-peer traffic is encrypted with ChaCha20-Poly1305 AEAD using session keys derived from an ML-KEM-1024 key exchange. The 4-step handshake provides mutual authentication and forward secrecy against quantum adversaries.

**Sync Replication** -- Peers automatically replicate blobs via range-based set reconciliation. XOR fingerprints over sorted hash ranges isolate differences in O(diff) wire traffic, not O(total blobs). One blob is in flight at a time per connection, bounding memory usage regardless of dataset size.

**Blob Deletion** -- Namespace owners permanently delete blobs by issuing signed tombstones. Tombstones replicate across the network via sync and block future arrival of the deleted blob.

**Namespace Delegation** -- Owners grant write access to other identities by creating signed delegation blobs. Delegates sign with their own key; revocation is done by tombstoning the delegation blob.

**Pub/Sub Notifications** -- Connected peers subscribe to namespaces and receive real-time notifications when blobs are ingested or deleted. Notifications carry metadata (namespace, hash, sequence number, size) so subscribers can decide whether to fetch the full blob.

**Storage Capacity Management** -- Operators set a `max_storage_bytes` limit. The node rejects new blobs when at capacity and sends a StorageFull signal to peers, which suppress sync pushes until reconnection.

**Rate Limiting** -- Per-connection token bucket rate limiting on Data and Delete messages. Peers exceeding the configured throughput are disconnected immediately.

**Namespace-Scoped Sync** -- Operators configure `sync_namespaces` to restrict which namespaces the node replicates. Unlisted namespaces are filtered at sync time and silently dropped on direct ingest.

**Graceful Shutdown** -- SIGTERM triggers a bounded shutdown that drains in-flight coroutines, saves the persistent peer list, and exits cleanly.

**Persistent Peer List** -- Known peer addresses are saved to disk and reloaded on restart, enabling reconnection without requiring bootstrap re-discovery.

**Runtime Metrics** -- Operators monitor the node via SIGUSR1 (on-demand metrics dump) or automatic periodic logging every 60 seconds. Metrics include connections, storage used, blobs ingested, sync rounds, rejections, and rate-limited disconnections.

**Encryption at Rest** -- All blob payloads are encrypted with ChaCha20-Poly1305 before writing to disk. The encryption key is derived from a node-local master key (`data_dir/master.key`) via HKDF-SHA256. The master key is auto-generated on first run with 0600 file permissions. Back up this file -- without it, stored data is unrecoverable.

**Lightweight Handshake** -- Connections from localhost (127.0.0.1, ::1) and addresses listed in `trusted_peers` skip the ML-KEM-1024 key exchange, completing the handshake without post-quantum crypto overhead. Both sides verify identity via ML-DSA-87 signatures over a shared challenge. Non-trusted peers always perform the full PQ handshake.

**Writer-Controlled TTL** -- Each blob carries its own TTL set by the writer in the signed data. TTL=0 means permanent (no expiry). Blobs with TTL>0 are expired by the node's event-driven expiry scanner. Tombstones MUST have TTL=0 (permanent) -- the node rejects tombstones with non-zero TTL at ingest. See [TTL Enforcement](#ttl-enforcement) for full details.

**Sync Resumption** -- Per-peer per-namespace sequence number cursors track sync progress. Cursor-hit namespaces (no new blobs) skip blob requests entirely. When new data exists, range-based reconciliation identifies only the missing blobs in O(diff) wire traffic. Cursors persist across node restarts via libmdbx. A periodic full reconciliation resync every Nth round (configurable) serves as a safety net against cursor drift.

**Thread Pool Crypto Offload** -- CPU-bound cryptographic operations (ML-DSA-87 signature verification, SHA3-256 content hashing) are dispatched to a configurable thread pool, freeing the event loop for I/O. Connection-scoped AEAD state is never accessed from worker threads (nonce safety by design).

**Concurrent Request Dispatch** -- Client requests are dispatched according to their cost. Lightweight operations (Subscribe, Unsubscribe, StorageFull, QuotaExceeded) execute inline on the IO thread. Storage queries (ReadRequest, ListRequest, StatsRequest, ExistsRequest, NodeInfoRequest) run as coroutines on the IO thread. Heavy operations (Data, Delete) offload cryptographic verification to the thread pool and transfer back to the IO thread before sending responses, maintaining AEAD nonce safety.

**Namespace Quotas** -- Per-namespace byte and blob count limits enforced at ingest. When a write would exceed the configured quota, the node rejects the blob and sends a QuotaExceeded signal to the writing peer. Global defaults apply to all namespaces, with per-namespace overrides for differentiated limits. Quota configuration is reloadable via SIGHUP.

**Config Validation** -- The node validates all configuration fields at startup and rejects invalid values with human-readable error messages. Validation checks types, ranges, and formats, accumulating all errors before reporting. Invalid configuration prevents startup.

**Structured Logging** -- Log output can be emitted in JSON format for machine parsing by setting `log_format` to `"json"`. Each log entry includes timestamp, level, logger name, and message fields.

**File Logging** -- The node can log to a rotating file in addition to stdout. Configure `log_file` with a path to enable. Files rotate at `log_max_size_mb` and the node retains `log_max_files` rotated files. If the file path is invalid, the node falls back to console-only logging with a warning.

**Cursor Compaction** -- Sync cursor entries for peers not currently connected are automatically pruned every 6 hours, preventing unbounded cursor storage growth over long uptimes.

**Startup Integrity Scan** -- On startup, the node performs a read-only scan of all sub-databases, logging entry counts and cross-reference consistency. The scan is informational and does not prevent startup.

**Auto-Reconnect** -- When an outbound peer disconnects, the node automatically reconnects with exponential backoff (1s to 60s) and random jitter. The first reconnect attempt is immediate. Discovered peers (via PEX) also survive disconnection.

**ACL-Aware Reconnection** -- When a peer rejects connections via ACL (connects, handshakes, disconnects with zero messages), the node enters extended backoff (600s) after 3 consecutive rejections. Sending SIGHUP resets all rejection counters for immediate retry.

**Inactivity Timeout** -- The node monitors all connected peers for message activity. Peers that send no messages within the configurable `inactivity_timeout_seconds` deadline are disconnected and their resources freed. This is receiver-side detection only (no Ping/Pong messages).

**Request Pipelining** -- Clients assign a `request_id` to each request message and the node echoes it on the corresponding response. Multiple requests can be sent without waiting, and responses are matched by `request_id` regardless of arrival order. The `request_id` is a `uint32` in the transport envelope, scoped per connection.

**Blob Existence Check** -- Clients send an ExistsRequest with a namespace and blob hash to check whether a blob exists without transferring its data. The node responds with a single-byte existence flag and the echoed blob hash. Tombstoned blobs are reported as not found.

**Node Capability Discovery** -- Clients send a NodeInfoRequest to retrieve the node's software version (e.g. `2.3.0-g784b1260` with a short git commit suffix when built from a git tree), uptime, peer count, namespace count, total blobs, storage usage, and a list of supported message types. Clients use the supported types list for feature detection.

**Namespace Enumeration** -- Clients list all namespaces stored on a node via NamespaceListRequest. Paginated response with after-cursor and configurable limit (max 1000 per page). Each entry includes the namespace ID and blob count.

**Storage Status Query** -- Clients query global storage status via StorageStatusRequest. Response includes total bytes used, max storage bytes, tombstone count, namespace count, and blob count.

**Per-Namespace Stats** -- Clients query detailed per-namespace counters via NamespaceStatsRequest. Response includes blob count, total bytes, latest sequence number, and per-namespace quota limits.

**Blob Metadata Query** -- Clients fetch blob metadata without transferring the blob payload via MetadataRequest. Response includes blob hash, timestamp, TTL, payload size, sequence number, and author public key hash.

**Batch Existence Check** -- Clients check existence of multiple blobs in a single request via BatchExistsRequest. Response is a compact boolean array (one byte per blob). Maximum 256 hashes per request.

**Delegation List** -- Clients list all active delegations for a namespace via DelegationListRequest. Response includes public key hash and delegation blob hash pairs.

**Batch Blob Fetch** -- Clients fetch multiple blobs in a single request via BatchReadRequest. Size-capped response with per-blob found/not-found status. A truncation flag indicates when results were limited by the byte cap.

**Peer Info Query** -- Clients query connected peer information via PeerInfoRequest. Trust-gated response: untrusted clients receive an 8-byte summary (peer count only), trusted clients receive full peer details including addresses, namespaces, and connection state.

**Time-Range Query** -- Clients fetch blobs within a timestamp range via TimeRangeRequest. Response includes blob hash, sequence number, and timestamp for each matching entry. Capped at 100 results with truncation flag.

## License

MIT
