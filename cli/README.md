# cdb

Post-quantum encrypted CLI client for chromatindb. All data is envelope-encrypted with ML-KEM-1024 before leaving the client. The node never sees plaintext.

## Quick Start

```bash
# Build
cd cli && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Generate identity (ML-DSA-87 signing + ML-KEM-1024 encryption)
./cdb keygen

# Publish your public key to the node
./cdb publish 192.168.1.73

# Upload a file (envelope-encrypted for yourself)
./cdb put secret.pdf 192.168.1.73

# Download and decrypt
./cdb get <hash> 192.168.1.73
```

## Config File

Create `~/.cdb/config.json` to set default host:

```json
{"host": "192.168.1.73", "port": 4200}
```

Then just: `cdb put file.txt`

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
| `export-key` | Export public keys (raw binary) |
| `publish` | Publish pubkey to node for contact discovery |
| `put <file>...` | Encrypt and upload file(s) |
| `get <hash>...` | Download and decrypt file(s) |
| `rm <hash>` | Delete (tombstone) a blob |
| `reshare <hash>` | Re-encrypt for new recipients |
| `ls` | List blobs in namespace |
| `exists <hash>` | Check if blob exists |
| `info` | Node information |
| `stats` | Namespace statistics |
| `delegate <pubkey>` | Grant write access to another identity |
| `revoke <pubkey>` | Revoke write access |
| `delegations` | List active delegations |
| `contact add/rm/list` | Manage local contacts |
| `contact import/export` | Import/export contacts |
| `group create/add/rm/list` | Manage contact groups |

## Global Flags

| Flag | Description |
|------|-------------|
| `--identity <path>` | Identity directory (default: `~/.cdb/`) |
| `--uds <path>` | UDS socket path (tried first) |
| `-p, --port <port>` | Port (default: 4200) |
| `-q, --quiet` | Minimal output |
| `-v, --verbose` | Show connection info |

## Connection

The CLI tries UDS first (`/run/chromatindb/node.sock`), then falls back to TCP with full ML-KEM-1024 post-quantum key exchange. Both paths use ChaCha20-Poly1305 AEAD encryption.

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
