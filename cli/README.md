# cdb

Post-quantum encrypted CLI client for chromatindb. All data is envelope-encrypted with ML-KEM-1024 before leaving the client. The node never sees plaintext.

## Hello World

First-time flow for a fresh identity on a running node:

```bash
cdb keygen                              # 1. generate ML-DSA-87 + ML-KEM-1024 identity
cdb publish 192.168.1.73                # 2. register pubkey on node (enables PUBK-first writes)
echo 'hello, world' > note.txt
cdb put note.txt 192.168.1.73           # 3. upload an envelope-encrypted blob
cdb ls 192.168.1.73                     # 4. enumerate blobs in your namespace
cdb get <hash-from-ls> 192.168.1.73     # 5. download and decrypt
```

`cdb publish` is not strictly required — `cdb put` auto-emits a PUBK blob on first write — but running it explicitly is recommended because it populates the node's `owner_pubkeys` registry and gives clearer feedback than a silent auto-PUBK during upload. Subsequent writes in the same namespace carry only a 32-byte `signer_hint` instead of the full 2592-byte signing pubkey; the node resolves the hint back through the registry. See [PROTOCOL.md §PUBK-First Invariant](../db/PROTOCOL.md#pubk-first-invariant) for the wire-level contract.

If auto-PUBK also fails, you'll see the error emitted by the CLI decoder — see [Auto-PUBK and First-Write Errors](#auto-pubk-and-first-write-errors) below.

## Build

```bash
cd cli && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

## Config File

Create `~/.cdb/config.json` to set a default node:

```json
{
  "default_node": "home",
  "nodes": {
    "home": "192.168.1.73:4200",
    "lab":  "10.0.0.5:4200"
  }
}
```

Then `cdb put file.txt` resolves `default_node` automatically; `cdb --node lab put file.txt` picks a different one. A legacy `{"host": "...", "port": ...}` shape is also honoured.

## Contacts

```bash
# Add a contact by their namespace (fetches their published pubkey)
cdb contact add alice c8afff59... 192.168.1.73

# Share a file with a contact by name
cdb put secret.pdf --share alice

# Download all files from a contact
cdb get --all --from alice -o ~/downloads

# List contacts
cdb contact list
```

## Contact Groups

```bash
# Create a group
cdb group create engineering

# Add contacts to a group
cdb group add engineering alice bob charlie

# Share a file with all group members
cdb put secret.pdf --share @engineering

# List all groups
cdb group list

# List members of a group
cdb group list engineering

# Remove a contact from a group
cdb group rm engineering charlie

# Delete a group entirely
cdb group rm engineering
```

## Bulk Import/Export

```bash
# Export contacts to JSON
cdb contact export > team.json

# Import contacts from JSON (fetches pubkeys from node)
cdb contact import team.json 192.168.1.73
```

Import format (JSON array):
```json
[
  {"name": "alice", "namespace": "c8afff59...64hex..."},
  {"name": "bob", "namespace": "a1b2c3d4...64hex..."}
]
```

## Mutable Names

`cdb put --name <name> <file>` uploads the file AND emits a NAME blob binding that name in your namespace to the content hash. `cdb get <name>` resolves it back. Names are 1..65535 bytes of opaque UTF-8 (the shell quotes it); uniqueness is per-namespace, not global.

```bash
# Publish a config under a stable name
cdb put --name prod-config config.yaml 192.168.1.73
# -> prints the content hash AND the NAME blob hash

# Retrieve by name (no hash needed)
cdb get prod-config 192.168.1.73 > config.yaml

# Overwrite in place: emits a new NAME blob + BOMB-of-1 tombstoning
# the previous content blob. Write order is content -> NAME -> BOMB,
# so a partial failure never leaves a deleted content blob without a
# pointer.
cdb put --name prod-config --replace config-v2.yaml 192.168.1.73

# Re-resolve -> v2
cdb get prod-config 192.168.1.73 > config-v2.yaml
```

### Resolver rules

`cdb get <name>` enumerates NAME blobs in the namespace via `ListRequest` with a type filter (same server-side infrastructure as `cdb ls --type NAME`), reads each matching NAME's full `BlobData`, filters to entries whose payload `name` matches exactly, and picks the winner with the order: `timestamp DESC`, then `blob_hash DESC` as tiebreak. The winner's `target_hash` is the content blob fetched. See [PROTOCOL.md §NAME Blob Format](../db/PROTOCOL.md#name-blob-format) for the byte layout and ingest invariants.

This lookup is stateless on every call — there is no local name cache.

### Known quirk: sub-second overwrite tiebreak

NAME blob timestamps are at 1-second resolution. If you run `cdb put --name X --replace` within the same wall-clock second as the previous NAME blob, the two blobs share `timestamp` and the resolver falls back to `blob_hash DESC` — which may pick the older blob depending on how the hashes compare. Practical impact: if you need sub-second overwrite semantics, wait one second or re-issue the `--replace`. A future release may switch the winner to `max(seen+1, now)` or tiebreak on `target_hash DESC` instead.

### Constraint: chunked files + `--name`

`--name` currently rejects files that trigger chunking (files larger than the server's advertised blob cap). The NAME blob has to bind to the CPAR manifest hash, not the first CDAT chunk, and the wiring for that combination is not yet in the binary. Upload large files without `--name` for now (or split manually); future releases will remove the restriction.

## Batched Deletion and CPAR Cascade

`cdb rm <hash>` signs and sends a single-target tombstone. `cdb rm <hash1> <hash2> <hash3> ...` emits one BOMB blob covering all N targets — the node expands the batch on ingest and writes one tombstone per target, at one round-trip for arbitrarily many deletions. Separate invocations produce separate BOMBs; the CLI does not coalesce across calls.

```bash
# Single-target: classic tombstone
cdb rm 3f2a...1234 192.168.1.73

# Batched: one BOMB covers all three
cdb rm 3f2a...1234 9e5b...abcd 4c7d...beef 192.168.1.73
#   -> BOMB(count=3, size=104) submitted
```

### CPAR manifest cascade

If any target hash resolves to a CPAR manifest (the chunked-upload manifest emitted by `cdb put` on a file larger than the server's advertised blob cap), the CLI automatically expands the delete to include every CDAT chunk the manifest references. You get a summary line before submission. Example: deleting a 750 MiB file's CPAR hash (uploaded against a 16 MiB-cap node → ~48 chunks) removes 1 CPAR + ~48 CDAT chunks in a single BOMB. This is the D-06 cascade — live-verified in Phase 124 against a 48-chunk manifest (cross-node sync confirmed).

```bash
# Suppose cpar-hash points at a 750 MiB upload (48 chunks + 1 manifest)
cdb rm <cpar-hash> 192.168.1.73
#   -> classify: CPAR manifest; cascading to 48 CDAT chunks
#   -> BOMB(count=49) submitted
```

If classification fails (target not found locally, or node returns `NotFound`), the CLI warns and continues with the explicit targets — it does not abort the whole batch.

### Invariants and limits

- BOMB blobs MUST be permanent (`ttl=0`); the node rejects non-zero TTLs with error code `0x09` — surfaced as: `Error: batch deletion rejected (BOMB must be permanent).`
- Malformed BOMB payloads (bad magic, wrong length, non-multiple-of-32 target region) are rejected with error code `0x0A`: `Error: batch deletion rejected (malformed BOMB payload).`
- **Delegates cannot BOMB.** Only owner identities may emit BOMB blobs. Delegate accounts trying `cdb rm` on multiple targets get error code `0x0B`: `Error: delegates cannot perform batch deletion on this node.` Single-target `cdb rm` from a delegate still works (it takes the classic single-tombstone path).

See [PROTOCOL.md §BOMB Blob Format](../db/PROTOCOL.md#bomb-blob-format) for the byte layout and [PROTOCOL.md §ErrorResponse (Type 63)](../db/PROTOCOL.md#errorresponse-type-63) for the full error-code table.

## Chunked Large Files

`cdb put` auto-chunks any file strictly larger than the server's advertised blob cap. Chunking is transparent — no flag, no subcommand — and the upload prints the manifest hash (not the first chunk's hash) on stdout. There is no `--chunk-size` flag; chunk size is derived at connect time from the server's `NodeInfoResponse.max_blob_data_bytes`.

**Auto-tuning (v4.2.0):** On every connect, `cdb` sends a `NodeInfoRequest` and caches `max_blob_data_bytes` for the session. Chunking policy:

- Files **≤ server cap** upload as a single blob (no chunking).
- Files **> server cap** are split into `CDAT` chunks of exactly `server_cap` bytes each, plus one `CPAR` manifest blob.
- `chunk_size = server_cap`, not configurable.
- If the server is pre-v4.2.0 and its `NodeInfoResponse` omits the new cap fields, `cdb` refuses to connect with an error naming the version gap — no silent default.

```bash
# Against a node with blob_max_bytes = 4 MiB (default):
#   64 MiB upload -> 16 CDAT chunks + 1 CPAR manifest
cdb put big.iso 192.168.1.73
#   -> prints <cpar-hash>

# Download reassembles transparently
cdb get <cpar-hash> 192.168.1.73 > big.iso
```

**File-size ceiling:** `MAX_CHUNKS = 65536`, so the largest file `cdb put` accepts is `MAX_CHUNKS × server_cap`: 256 GiB at the 4 MiB default cap, 4 TiB at the 64 MiB hard ceiling. Anything larger is rejected up-front.

**Cap divergence across peered nodes:** if you `put` to Node A (`blob_max_bytes = 8 MiB`) and Node A is peered with Node B (`blob_max_bytes = 4 MiB`), any 6 MiB blob on A is silently skipped on sync to B (B cannot accept it). See [PROTOCOL.md §Sync Cap Divergence](../db/PROTOCOL.md#sync-cap-divergence) for the full semantics and the `chromatindb_sync_skipped_oversized_total{peer=...}` visibility counter.

`cdb rm <cpar-hash>` cascades to all referenced CDAT chunks — see [Batched Deletion and CPAR Cascade](#batched-deletion-and-cpar-cascade) above. See [PROTOCOL.md §Chunked Transport Framing](../db/PROTOCOL.md#chunked-transport-framing) for the on-wire sub-frame layout used by both single and chunked uploads.

## Request Pipelining

Multi-hash `cdb get h1 h2 h3 h4 ...` uses request pipelining over a single PQ connection — the CLI fires up to 8 in-flight `ReadRequest` messages at once and drains replies as they arrive. The same pipelining powers chunked uploads (parallel `CDAT` submissions) and bulk NAME listing. There is no user-visible flag; depth is fixed at `Connection::kPipelineDepth = 8`. Operator-visible only as "fast on high-latency links".

See [PROTOCOL.md §Transport Layer](../db/PROTOCOL.md#transport-layer) for the `request_id` correlation field that makes pipelining possible.

## Auto-PUBK and First-Write Errors

On the first write into a namespace on a given node, `cdb put` (and every other owner-write command) automatically emits a `PUBK` blob before the user blob, populating the node's `owner_pubkeys` registry. It is invisible when it succeeds — the user never sees the extra round-trip.

If auto-PUBK fails (network error, ACL rejection, node misconfiguration), you see the decoder's verbatim wording:

> `Error: namespace not yet initialized on node <host>. Auto-PUBK failed; try running 'cdb publish' first.`

Running `cdb publish <host>` explicitly is always safe — it emits the same PUBK blob, surfaces the real network error if any, and seeds the CLI's per-invocation cache so subsequent commands skip the auto-PUBK check. When a namespace is already owned by a different key, the node returns error `0x08` which surfaces as:

> `Error: namespace <ns-short> is owned by a different key on node <host>. Cannot write.`

Other PUBK-related errors (malformed BOMB payloads, delegate restrictions, unknown codes) produce similar one-line messages. The canonical table (codes `0x07`–`0x0B` with trigger conditions and CLI wording) lives in [PROTOCOL.md §ErrorResponse (Type 63)](../db/PROTOCOL.md#errorresponse-type-63) — not duplicated here because the [`error_decoder`] unit test in `cli/tests/test_wire.cpp` enforces byte-identical wording between the CLI and PROTOCOL.md; drift surfaces as a test failure.

## Commands

| Command | Description |
|---------|-------------|
| `keygen` | Generate ML-DSA-87 + ML-KEM-1024 identity |
| `whoami` | Print namespace (SHA3-256 of signing pubkey) |
| `export-key` | Export public keys (raw binary, hex, or base64) |
| `publish` | Register this identity's pubkey on a node (emits a PUBK blob). Required — directly or via auto-PUBK — before any other blob in this namespace can be written. See [PROTOCOL.md §PUBK-First Invariant](../db/PROTOCOL.md#pubk-first-invariant). |
| `put <file>...` | Upload file(s), envelope-encrypted. Flags: `--share <name\|@group\|file>`, `--ttl <duration>`, `--stdin`, `--name <name>`, `--replace`. Auto-chunks files larger than the server's advertised blob cap into CDAT + CPAR (see [Chunked Large Files](#chunked-large-files)). |
| `get <hash>...` | Download + decrypt by blob hash |
| `get <name>` | Download + decrypt by mutable name (see [Mutable Names](#mutable-names)) |
| `get --all --from <name>` | Download every blob in a contact's namespace |
| `rm <hash>` | Delete one blob (signed tombstone). Pre-checks existence unless `--force`. |
| `rm <hash1> <hash2>...` | Delete many blobs in a single batch via a BOMB blob. Cascades to CDAT chunks if a target is a CPAR manifest. See [Batched Deletion and CPAR Cascade](#batched-deletion-and-cpar-cascade). |
| `reshare <hash>` | Re-encrypt a blob's envelope for a new recipient set |
| `ls` | List user blobs in the namespace. Flags: `--namespace <name\|hex>`, `--raw`, `--type <TYPE>` (CENV, PUBK, TOMB, DLGT, CDAT, CPAR, NAME, BOMB) |
| `exists <hash>` | Check whether a blob exists (no download) |
| `info` | Node info: version, uptime, peer count, supported types |
| `stats` | Namespace statistics: blob count, bytes stored, quota usage |
| `delegate <target>` | Grant write access in this namespace. Target is a contact name, `@group`, or pubkey file path. |
| `revoke <target>` | Revoke a prior delegation (prompts unless `--yes`) |
| `delegations` | List active delegations for this namespace |
| `contact add/rm/list` | Manage local contacts (namespace → pubkey map) |
| `contact import/export` | JSON import/export for bulk onboarding |
| `group create/add/rm/list` | Named contact groups for `--share @group` |
| `version` | Print version |

Every subcommand accepts `--help` for per-command usage details.

## Global Flags

| Flag | Description |
|------|-------------|
| `--identity <path>` | Identity directory (default: `~/.cdb/`) |
| `--node <name>` | Select a named node from `~/.cdb/config.json` |
| `--host <addr>` | Target host, optionally `host:port` (overrides config) |
| `-p, --port <port>` | Port (default: 4200) |
| `--uds <path>` | UDS socket path (overrides host) |
| `-q, --quiet` | Minimal output |
| `-v, --verbose` | Info-level log output |
| `-h, --help` | Show help |

Global flags may appear in any position (`cdb --node home put file` and `cdb put --node home file` are equivalent).

## Connection

The CLI tries UDS first (default `/run/chromatindb/node.sock`), then falls back to TCP with full ML-KEM-1024 post-quantum key exchange. Both paths use ChaCha20-Poly1305 AEAD encryption.

Host resolution precedence:

1. `--host <addr>` or a positional `<host>` argument
2. `--node <name>` → `~/.cdb/config.json` named nodes map
3. `default_node` in `~/.cdb/config.json`
4. Legacy `host` / `port` fields in `~/.cdb/config.json`
5. Built-in default port 4200 applied to whichever host was resolved

`--uds <path>` set explicitly is never clobbered by the host-resolver path.

## Encryption

Every uploaded blob is envelope-encrypted:

- Random 32-byte DEK per blob
- DEK wrapped per-recipient via ML-KEM-1024 + HKDF-SHA256
- Data encrypted with ChaCha20-Poly1305
- `--share <name>` adds recipients (always includes self)

The node stores opaque ciphertext. Only recipients with the matching ML-KEM private key can decrypt.

## Dependencies

- liboqs (ML-DSA-87, ML-KEM-1024, SHA3-256)
- libsodium (ChaCha20-Poly1305, HKDF-SHA256)
- FlatBuffers (wire format)
- Standalone Asio (networking)
- SQLite3 (contacts database)
- spdlog, nlohmann/json

All except libsodium and SQLite3 are fetched via CMake FetchContent.
