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

## Commands

| Command | Description |
|---------|-------------|
| `keygen` | Generate ML-DSA-87 + ML-KEM-1024 identity |
| `whoami` | Print namespace (SHA3-256 of signing pubkey) |
| `export-key` | Export public keys (raw binary, hex, or base64) |
| `publish` | Register this identity's pubkey on a node (emits a PUBK blob). Required — directly or via auto-PUBK — before any other blob in this namespace can be written. See [PROTOCOL.md §PUBK-First Invariant](../db/PROTOCOL.md#pubk-first-invariant). |
| `put <file>...` | Upload file(s), envelope-encrypted. Flags: `--share <name\|@group\|file>`, `--ttl <duration>`, `--stdin`, `--name <name>`, `--replace`. Auto-chunks files ≥ 400 MiB into CDAT + CPAR (see [Chunked Large Files](#chunked-large-files)). |
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
