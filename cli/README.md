# chromatindb-cli

Post-quantum encrypted CLI client for chromatindb. All data is envelope-encrypted with ML-KEM-1024 before leaving the client. The node never sees plaintext.

## Quick Start

```bash
# Build
cd cli && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Generate identity (ML-DSA-87 signing + ML-KEM-1024 encryption)
./chromatindb-cli keygen

# Publish your public key to the node
./chromatindb-cli publish 192.168.1.73

# Upload a file (envelope-encrypted for yourself)
./chromatindb-cli put secret.pdf 192.168.1.73

# Download and decrypt
./chromatindb-cli get <hash> 192.168.1.73
```

## Config File

Create `~/.chromatindb/config.json` to set default host:

```json
{"host": "192.168.1.73", "port": 4200}
```

Then just: `chromatindb-cli put file.txt`

## Contacts

```bash
# Add a contact by their namespace (fetches their published pubkey)
chromatindb-cli contact add alice c8afff59... 192.168.1.73

# Share a file with a contact by name
chromatindb-cli put secret.pdf --share alice

# Download all files from a contact
chromatindb-cli get --all --from alice -o ~/downloads

# List contacts
chromatindb-cli contact list
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

## Global Flags

| Flag | Description |
|------|-------------|
| `--identity <path>` | Identity directory (default: `~/.chromatindb/`) |
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
