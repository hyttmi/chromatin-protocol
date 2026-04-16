# Feature Research: CLI Polish (v4.1.0)

**Domain:** Encrypted file-sharing CLI (comparable to age, GPG, croc, rclone, restic)
**Researched:** 2026-04-15
**Confidence:** HIGH

## Feature Landscape

### Table Stakes (Users Expect These)

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Short executable name (`cdb`) | Every serious CLI tool uses a short name. `chromatindb-cli` is 15 chars -- unusable for daily work. GPG is 3 chars, age is 3, croc is 4, psql is 4. Smallstep's naming guide: ergonomics of typing matters, avoid shift key, avoid "tool/kit/util" suffixes. | LOW | Rename binary in CMakeLists.txt, update all usage strings and README. `cdb` collides with tinycdb (Dan Bernstein's constant database tool) but it is not installed by default on any major distro and is a completely different domain. Acceptable collision. |
| Contact groups for batch sharing | GPG has `--group` in gpg.conf since GnuPG 2.0. age uses recipient files (`-R team.txt`). Every multi-recipient encryption tool provides some grouping mechanism. Typing `--share alice --share bob --share charlie` for every upload is painful and error-prone. | LOW | Purely local: new SQLite tables `groups` + `group_members`, new subcommands `cdb group create/add/rm/list/show`, resolve `--share @team` to member KEM pubkeys at encrypt time. Zero protocol changes. Zero node interaction. |
| Chunked large file upload/download (>500 MiB) | restic chunks all files via CDC (512 KiB-8 MiB). rclone supports `--transfers N` parallel files and multi-threaded single-file uploads. croc streams files of any size via relay. Users moving backup archives, disk images, or video files expect multi-GiB to work. Current 500 MiB hard limit blocks real-world use cases. | HIGH | Split file into fixed-size chunks (50 MiB default), each independently envelope-encrypted. Requires CPAR manifest blob (chunk hashes + metadata). Download reads manifest first, then fetches chunks. Needs: manifest format, split logic, reassembly logic, progress reporting. No node changes -- each chunk is a regular blob. |
| Request pipelining for parallel downloads | rclone defaults to `--transfers 4`. restic uses 2-5 concurrent file workers. croc uses multiple TCP ports for parallel data transfer. Sequential download of N chunks over a WAN link is unacceptably slow -- latency compounds linearly. | MEDIUM | The existing `Connection` class is synchronous (blocking `send`/`recv`). Pipelining requires sending N `ReadRequest` messages with distinct `request_id` values, then receiving responses in any order and dispatching by `request_id`. Needs a dedicated reader thread (simplest) or full async refactor (overkill). The wire format already has `request_id` -- this is HTTP/2-style multiplexing over a single AEAD channel. |
| Progress reporting for large transfers | croc shows transfer progress with speed. rclone shows per-file progress with ETA. restic shows ETA + throughput. age does not (pipe-oriented). For multi-GiB files, silent operation = dead UX. | LOW | Track bytes sent/received against known total, print `\r`-overwritten line to stderr. Trivial implementation, high UX impact. |

### Differentiators (Competitive Advantage)

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| PQ-encrypted chunked files with per-chunk envelope encryption | No other CLI tool offers post-quantum encrypted chunked file transfer. age uses X25519 (classical). GPG uses RSA/ECC. restic uses AES-256-CTR. croc uses NaCl. This is unique for compliance-sensitive environments (CNSA 2.0, FIPS 203/204 transition). | Included in chunking work | Each chunk gets its own ML-KEM-1024 envelope. Chunk compromise does not reveal other chunks. Per-chunk independent key encapsulation. |
| Named contact groups with `@group` syntax | GPG groups require editing gpg.conf manually with key fingerprints. age recipient files are loose text files with no validation. `cdb --share @engineering` with SQLite-backed groups and validated contacts is cleaner than either. | Included in groups work | Groups reference existing contacts in the SQLite database. Adding a contact to a group validates the contact exists and has a KEM pubkey. |
| Group-aware reshare | `cdb reshare <hash> --share @newteam` re-encrypts an existing blob for a new group. No other tool in the comparison set has server-stored encrypted blobs with recipient rotation. | LOW | Existing `reshare` command already fetches, decrypts, re-encrypts. Just needs group resolution in the `--share` flag parser. |
| Single-connection multiplexed pipelining | croc opens multiple TCP ports (9009-9013). rclone opens multiple backend connections. Both waste resources. `cdb` uses a single PQ-authenticated AEAD connection with `request_id` multiplexing -- zero extra handshakes, zero extra AEAD state. | Included in pipelining work | This is the HTTP/2 approach over a custom transport. More efficient than multi-connection, and preserves AEAD nonce ordering. |

### Anti-Features (Commonly Requested, Often Problematic)

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| Content-defined chunking (CDC) like restic | Deduplication across uploads, bandwidth savings on modified files | Massive complexity (Rabin fingerprint rolling hash, chunk index, dedup database). restic does CDC because dedup is its core value. For an encrypted blob store, each upload gets a fresh DEK + fresh ML-KEM encapsulation, so identical plaintext produces different ciphertext. ML-DSA-87 signatures are non-deterministic. Dedup is cryptographically impossible. | Fixed-size chunks (50 MiB). Simple, predictable, no dedup pretense. |
| Parallel upload of multiple files in one command | `cdb put file1 file2 file3` with concurrent uploads | Each put requires ML-DSA-87 sign + ML-KEM encapsulate per recipient, which is CPU-bound (~10ms per recipient per chunk). Parallelizing requires thread pool for crypto + async network. Upload is CPU-bottlenecked, not I/O-bottlenecked. Over-engineering for v4.1.0. | Sequential put for multiple files. Parallel download (pipelining) is more valuable since download is I/O-bound. |
| Auto-discovery of recipients from node | "Encrypt for all contacts on the node" | Dangerous default. Accidentally sharing with wrong people. Requires fetching all pubkeys from all namespaces, which is expensive and leaks who exists on the node. | Explicit `--share @group` or `--share name`. Never auto-share. |
| Recursive directory upload | `cdb put ./mydir/` | Would need tar/zip, directory structure manifest, or per-file upload with metadata. Huge scope creep. age, GPG, croc all do not handle directories natively -- they rely on tar pipes. | `tar cf - mydir \| cdb put --stdin --share @team`. Document the pattern. |
| Compression before encryption | Save bandwidth and storage | Standard practice (restic, rclone both compress before encrypting). However, adding zstd/brotli to CLI increases binary size and build deps. The node already has Brotli envelope support (suite 0x02) but CLI does not use it. Encrypted data is incompressible so compression must happen before encryption. | Defer to v4.2.0. Can be added as new envelope suite without protocol changes. Users can `zstd file && cdb put file.zst` today. |
| Multi-connection parallel transfer | Open N TCP connections for throughput | Each connection requires a full PQ handshake (ML-KEM-1024 encapsulate + ML-DSA-87 sign), ~50ms. N connections = N AEAD states, N nonce counters, N send/recv buffers. Complexity for marginal gain on a single link. | Single connection with `request_id` multiplexing. Same throughput, less overhead. |
| Streaming single-blob encryption (avoid buffering 500 MiB) | Memory efficiency for large single blobs | Blocked by FlatBuffer serialization requiring full blob data field at construction time. Would need a protocol change to support streaming writes (new message type for incremental data). The chunked approach solves this differently: each 50 MiB chunk fits in memory comfortably. | Chunked files. Each chunk is 50 MiB which is well within memory budget. The chunking approach removes the need for true streaming. |

## Feature Dependencies

```
[Executable Rename (cdb)]
    (no dependencies -- build system only)

[Contact Groups]
    (no dependencies -- purely local SQLite)

[Chunked Large Files]
    requires --> [CPAR Manifest Format] (chunk metadata storage)
    requires --> [Request Pipelining] (parallel chunk download)
    requires --> [Progress Reporting] (unusable without progress for multi-GiB)

[Request Pipelining]
    requires --> [Reader Thread / Async Recv] (cannot pipeline with synchronous recv)
    enhances --> [Chunked Large Files] (parallel chunk download)
    enhances --> [get with multiple hashes] (parallel blob download)

[Progress Reporting]
    enhances --> [Chunked Large Files]
    enhances --> [get / put for any large blob]
```

### Dependency Notes

- **Chunked Large Files requires Request Pipelining:** Downloading a 2 GiB file split into 40 chunks (50 MiB each) sequentially over a WAN link with 50ms RTT adds 2 seconds of pure latency overhead (40 round-trips). With 4-way pipelining, that drops to 500ms. Without pipelining, chunked download is a performance regression vs. the current single-blob approach.
- **Request Pipelining requires async recv:** The current `Connection::recv()` blocks until one message arrives. To handle out-of-order responses for pipelined requests, the CLI needs a reader thread that receives messages and dispatches them by `request_id` to waiting callers. The send path stays synchronous (simpler, preserves AEAD nonce ordering).
- **Chunked Large Files requires CPAR manifest:** A chunked file needs metadata so `cdb get` knows which blobs to fetch and in what order. The manifest blob stores: original filename, total size, chunk size, chunk count, ordered list of chunk `blob_hash` values. The manifest itself is envelope-encrypted with the same recipients as chunks.
- **Contact Groups and Executable Rename have no dependencies:** Can be implemented first, independently, in any order. These are the quick wins.

## MVP Definition

### Launch With (v4.1.0)

- [ ] Executable rename to `cdb` -- removes daily typing friction
- [ ] Contact groups (`group create/add/rm/list/show`, `--share @groupname`) -- resolves most common multi-recipient pain
- [ ] Chunked large file upload with CPAR manifest (50 MiB default chunks) -- removes 500 MiB ceiling
- [ ] Request pipelining for parallel chunk download (reader thread + `request_id` dispatch, 4 concurrent requests) -- makes chunked download performant
- [ ] Progress reporting for put/get of large files -- table stakes UX

### Add After Validation (v4.2.0)

- [ ] Compression before encryption (zstd, new envelope suite 0x03) -- when storage costs become a complaint
- [ ] Configurable chunk size (`--chunk-size 50M`) -- when users hit specific network/memory profiles
- [ ] Configurable pipeline depth (`--parallel 8`) -- when default 4 is insufficient for high-latency links
- [ ] `cdb get --all <namespace>` bulk download -- when users need full namespace export

### Future Consideration (v5+)

- [ ] Parallel multi-file upload -- when users report slow batch uploads of many small files
- [ ] Interactive TUI mode -- when CLI becomes primary daily interface
- [ ] Recursive directory support via tar integration -- when directory workflows become common

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| Executable rename (`cdb`) | HIGH | LOW | P1 |
| Contact groups | HIGH | LOW | P1 |
| Progress reporting | MEDIUM | LOW | P1 |
| Request pipelining (reader thread) | HIGH | MEDIUM | P1 |
| Chunked large files (CPAR manifest) | HIGH | HIGH | P1 |
| Compression before encryption | MEDIUM | MEDIUM | P2 |
| Configurable chunk size | LOW | LOW | P2 |
| Parallel multi-file upload | LOW | MEDIUM | P3 |
| Recursive directory upload | LOW | HIGH | P3 |

## Competitor Feature Analysis

| Feature | age | GPG | croc | rclone | restic | cdb (planned) |
|---------|-----|-----|------|--------|--------|---------------|
| **Recipient groups** | `-R file` (text files, no named groups) | `--group` in gpg.conf (named key ID aliases) | N/A (ephemeral code pairing) | N/A (config-level remotes) | N/A (single-user backup) | SQLite contact groups, `@name` syntax |
| **Large file handling** | 64 KiB STREAM chunks, constant memory | Streams natively, no size limit | Streaming TCP relay, parallel ports | Multi-threaded per-file, `--transfers N` | CDC 512 KiB-8 MiB, pack files | 50 MiB envelope-encrypted chunks, CPAR manifest |
| **Parallel transfers** | No (pipe-oriented) | No (single operation) | Yes (multi-port TCP, 9009-9013) | Yes (`--transfers 4` default) | Yes (2-5 concurrent workers) | Yes (`request_id` pipelining, single connection) |
| **Streaming encryption** | Yes (64 KiB chunks, constant memory) | Yes (native streaming) | Yes (relay-mediated) | Yes (NaCl SecretBox 64 KiB chunks) | Yes (CDC + pack buffering) | Per-chunk (50 MiB in memory per chunk) |
| **Post-quantum crypto** | No (X25519) | No (RSA/ECC) | No (PAKE + NaCl) | No (NaCl SecretBox) | No (AES-256) | Yes (ML-KEM-1024, ML-DSA-87, SHA3-256) |
| **Executable name** | 3 chars | 3 chars | 4 chars | 6 chars | 6 chars | 3 chars |
| **Progress reporting** | No | No | Yes (speed + progress) | Yes (detailed, per-file) | Yes (ETA + speed) | Planned |
| **Contact/key management** | None (external files) | Keyring + trust model | None (ephemeral codes) | Config-level | None (repo keys) | SQLite contacts + named groups |

## Design Decisions from Ecosystem Research

### Chunk Size: 50 MiB (not 64 KiB like age, not CDC like restic)

age uses 64 KiB because it targets streaming to stdout with constant memory and seekable random access. restic uses CDC (512 KiB-8 MiB) for dedup. Neither applies here.

chromatindb stores blobs on a node -- each chunk is a separate blob with full envelope overhead: 20-byte header + 1648 bytes per recipient stanza + 16 bytes AEAD tag per 1 MiB segment. At 64 KiB per chunk, a 1 GiB file produces 16,384 blobs requiring 16,384 separate ML-KEM encapsulations (~10ms each = 164 seconds of pure CPU time for a single recipient). At 50 MiB per chunk, 1 GiB produces 20 blobs with 20 encapsulations (~200ms). The overhead difference is 3 orders of magnitude.

50 MiB fits comfortably in memory (well under the 500 MiB single-blob limit the node already handles). The per-chunk envelope overhead (1648 bytes/recipient) is negligible relative to 50 MiB of data.

**Decision: 50 MiB default chunk size.** Balances blob count, crypto overhead, and memory use.

### Pipelining: Request ID Multiplexing Over Single Connection

croc opens multiple TCP ports (9009-9013). rclone opens N backend connections (default 5). HTTP/2 multiplexes streams over a single TCP connection.

The chromatindb wire format already has a `request_id` field in every `TransportMessage`. This is purpose-built for multiplexing. The AEAD nonce counter is per-connection and monotonically increasing -- sending multiple requests is safe as long as sends are serialized (which they are on a single thread). Responses arrive asynchronously and are dispatched by `request_id`.

**Decision: Single connection, `request_id` multiplexing, dedicated reader thread.** Reader thread runs `recv()` in a loop, places responses into per-request-id queues (or a concurrent map). Sender thread fires N ReadRequests, then waits on its specific response queue. Default concurrency: 4 in-flight requests (matches rclone default).

### Group Storage: SQLite Tables (not flat files like age)

age uses flat text files for recipient lists. GPG uses gpg.conf lines with key fingerprints. Both are fragile: no validation, no referential integrity, easy to have stale entries.

chromatindb already has a SQLite contact database at `~/.chromatindb/contacts.db`. Groups are a natural extension:

```sql
CREATE TABLE groups (
    name TEXT PRIMARY KEY
);
CREATE TABLE group_members (
    group_name TEXT REFERENCES groups(name) ON DELETE CASCADE,
    contact_name TEXT REFERENCES contacts(name) ON DELETE CASCADE,
    PRIMARY KEY (group_name, contact_name)
);
```

Adding a contact to a group validates the contact exists. Deleting a contact cascades to group membership. Atomic operations. No stale references.

**Decision: SQLite `groups` + `group_members` tables.** Consistent with existing contact storage.

### Manifest Format: Envelope-Encrypted CPAR Blob

A chunked file needs a manifest so `cdb get` can discover and reassemble chunks. The manifest is a regular blob with CPAR magic (0x43504152) containing:

```
CPAR (4 bytes magic)
version (1 byte, 0x01)
chunk_size (4 bytes BE, e.g., 52428800 for 50 MiB)
total_size (8 bytes BE, original file size)
chunk_count (4 bytes BE)
filename_len (2 bytes BE)
filename (UTF-8, variable)
chunk_hashes (chunk_count * 32 bytes, ordered SHA3-256 blob hashes)
```

The manifest is then envelope-encrypted (CENV wrapping CPAR) with the same recipients as the chunks. This means:
- The file structure (name, size, chunk list) is encrypted -- not leaked to the node
- `cdb get <manifest_hash>` fetches and decrypts the manifest, then fetches chunks by hash
- The manifest is small (< 2 KiB for typical files) so the overhead is negligible

**Decision: CPAR magic, envelope-encrypted manifest.** Keeps metadata confidential while enabling chunk discovery.

## Sources

- [age specification (C2SP)](https://github.com/C2SP/C2SP/blob/main/age.md) -- STREAM encryption, 64 KiB chunks, nonce format
- [age GitHub](https://github.com/FiloSottile/age) -- recipient files, no built-in named groups
- [age streaming encryption (DeepWiki)](https://deepwiki.com/FiloSottile/age/3.3-key-management) -- chunk structure, constant memory
- [GPG Key-related Options](https://www.gnupg.org/documentation/manuals/gnupg/GPG-Key-related-Options.html) -- `--group` in gpg.conf
- [GPG group feature tutorial (GPGTools)](https://gpgtools.tenderapp.com/kb/how-to/how-to-use-the-group-feature-to-encrypt-content-to-multiple-public-keys-by-using-a-single-address) -- group encryption workflow
- [croc GitHub](https://github.com/schollz/croc) -- parallel multi-port TCP, streaming relay, PAKE
- [rclone crypt documentation](https://rclone.org/crypt/) -- NaCl SecretBox, 64 KiB chunks
- [rclone global flags](https://rclone.org/flags/) -- `--transfers` parallel file control
- [rclone multi-threaded transfers](https://rcloneview.com/support/blog/multi-threaded-transfers-parallel-streams-rcloneview) -- per-file threading
- [restic CDC foundation](https://restic.net/blog/2015-09-12/restic-foundation1-cdc/) -- content-defined chunking, 512 KiB-8 MiB
- [restic tuning documentation](https://restic.readthedocs.io/en/stable/047_tuning_backup_parameters.html) -- read concurrency, backend connections
- [restic restore concurrency issue](https://github.com/restic/restic/issues/5339) -- file-write concurrency discussion
- [magic-wormhole file transfer protocol](https://magic-wormhole.readthedocs.io/en/latest/file-transfer-protocol.html) -- Transit object, memory limitations
- [magic-wormhole large file issues](https://github.com/magic-wormhole/magic-wormhole/issues/327) -- RAM buffering for large files
- [tinycdb man page](https://man.archlinux.org/man/cdb.1.en) -- `cdb` name collision assessment
- [CLI naming conventions (Smallstep)](https://smallstep.com/blog/the-poetics-of-cli-command-names/) -- short names, ergonomics, typing feel
- [CLI design guidelines](https://clig.dev/) -- naming, structure, subcommand patterns
- [HTTP/2 multiplexing](https://blog.codavel.com/http2-multiplexing) -- stream muxing pattern for pipelining design

---
*Feature research for: chromatindb CLI Polish v4.1.0*
*Researched: 2026-04-15*
