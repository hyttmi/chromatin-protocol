# Phase 78: Documentation & Polish - Research

**Researched:** 2026-04-02
**Domain:** Technical documentation for PQ envelope encryption SDK features
**Confidence:** HIGH

## Summary

Phase 78 is a pure documentation phase -- no code changes, no new features. Three existing files must be updated: `db/PROTOCOL.md` (envelope binary format spec + HKDF label registry), `sdk/python/README.md` (encryption API section), and `sdk/python/docs/getting-started.md` (encryption workflow tutorial). All source code that needs to be documented is already shipped and tested (Phases 75-77 complete, all 23 non-doc requirements done).

The research focused on (1) reading the source code to extract exact API signatures, binary format constants, and HKDF labels, (2) studying the existing document formatting patterns so new content integrates seamlessly, and (3) identifying discrepancies between PROTOCOL.md and the implementation that the HKDF label registry must address accurately.

**Primary recommendation:** Write documentation directly from source code (not from memory or CONTEXT.md summaries) -- the implementation is the single source of truth. Match existing document styles exactly. Each of the three files is an independent deliverable with no cross-dependencies.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Byte-level format tables matching existing PROTOCOL.md style (offset/size/field/description columns)
- **D-02:** New section "Client-Side Envelope Encryption" after "SDK Client Notes" or as a peer section to existing transport/message docs
- **D-03:** Document the complete envelope binary format: [magic:4][version:1][suite:1][recipient_count:2 BE][data_nonce:12][N x (kem_pk_hash:32 + kem_ct:1568 + wrapped_dek:48)][ciphertext+tag]
- **D-04:** HKDF Label Registry section listing all four domain labels: two transport, one DARE, one envelope KEK
- **D-05:** Document the KEM-then-Wrap pattern: ML-KEM-1024 encapsulation produces shared secret, HKDF derives KEK, KEK wraps random DEK via ChaCha20-Poly1305
- **D-06:** Document AEAD parameters for data encryption: random 12-byte nonce (NOT counter-based), ChaCha20-Poly1305, full header as AD
- **D-07:** Add "Encryption" section to API Overview with sub-tables: Encryption Operations, Directory & Groups
- **D-08:** Table format matching existing method tables (Method | Description | Returns columns)
- **D-09:** Cover write_encrypted, read_encrypted, write_to_group, Directory (delegate, register, list_users, get_user, create_group, add_member, remove_member, list_groups, get_group), GroupEntry re-export
- **D-10:** Quick Start example updated to show encrypted write/read alongside existing plaintext example
- **D-11:** Extend existing tutorial (sdk/python/docs/getting-started.md) with new sections at the end, not a separate file
- **D-12:** Step-by-step narrative: Identity with KEM -> Directory setup (admin) -> User registration -> Group creation -> write_encrypted -> read_encrypted -> write_to_group
- **D-13:** Show both self-encrypt (recipients=None) and multi-recipient patterns
- **D-14:** Match existing tutorial style: explanation paragraph -> code block -> brief notes

### Claude's Discretion
- Exact placement of envelope spec section within PROTOCOL.md
- Whether to add a "Concepts" subsection explaining envelope encryption at a high level before the byte-level spec
- Exact ordering of encryption methods in README tables
- Whether tutorial encryption section includes error handling examples (NotARecipientError)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| DOC-01 | PROTOCOL.md updated with envelope format spec and HKDF label registry | Full binary format extracted from `_envelope.py` constants; all 4 HKDF labels identified from source; PROTOCOL.md structure mapped (line 829-859 = "SDK Client Notes" section, new content goes after it) |
| DOC-02 | SDK README updated with encryption API section | All method signatures extracted from `client.py` (write_encrypted, read_encrypted, write_to_group) and `_directory.py` (Directory class: 9 methods); existing README table format documented |
| DOC-03 | Getting started tutorial updated with encryption workflow example | Existing tutorial structure analyzed (187 lines, ends at "Next Steps"); Identity.generate() already produces KEM keypairs; all encrypted ops have clear async signatures |
</phase_requirements>

## Standard Stack

Not applicable -- this phase is pure documentation with no code or dependency changes.

## Architecture Patterns

### Document Structure Patterns

#### PROTOCOL.md Established Pattern

The existing doc uses consistent formatting throughout. Key patterns:

1. **Section hierarchy:** `##` for major sections (Transport Layer, Connection Lifecycle, Client Protocol, SDK Client Notes), `###` for sub-sections
2. **Byte-level format tables:** Offset/Size/Field/Description columns with pipe-delimited markdown tables
3. **Wire format diagrams:** Inline code blocks with bracket notation like `[field: N bytes]`
4. **ASCII art:** Used for handshake flows (initiator/responder arrows)
5. **AEAD parameter tables:** Property/Value format (used at line 18-26)

Example of established table format (from WriteAck, line 441-445):
```markdown
| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| blob_hash | 0 | 32 | raw bytes | SHA3-256 of the encoded blob |
```

#### README.md Established Pattern

1. **API tables:** Method | Description | Returns columns, grouped by category with `###` headers
2. **Quick Start:** Single code block showing minimal connect-write-read cycle
3. **Categories:** Data Operations, Query Operations, Pub/Sub, Utility
4. **Method signatures:** Backtick-wrapped with key params shown (e.g., `write_blob(data, ttl)`)

#### Tutorial Established Pattern

1. **Section flow:** Prerequisites -> Identity -> Connect -> Operations -> Advanced -> Error Handling -> Next Steps
2. **Each section:** Explanation paragraph -> code block -> brief notes paragraph
3. **Code blocks:** Show async context manager usage, use `192.168.1.200:4201` as example relay
4. **Progressive disclosure:** Simple operations first, complex later

### Recommended Placement for New Content

**PROTOCOL.md:** New "Client-Side Envelope Encryption" section AFTER "SDK Client Notes" (line 859 is EOF). This positions it as the final major section, logically after all transport/client protocol docs. The HKDF Label Registry can be a subsection within the envelope section.

**README.md:** New "Encryption" section with two sub-tables (Encryption Operations, Directory & Groups) after "Pub/Sub" and before "Utility". Update Quick Start to add 3-4 lines showing encrypted write/read. The Utility section (just `ping()`) stays last.

**Tutorial:** New encryption sections at the end, before "Next Steps". Sections: "Encrypted Write and Read" -> "Directory Setup" -> "User Registration" -> "Groups and Group Encryption". Update "Next Steps" to mention encryption capabilities.

### Anti-Patterns to Avoid
- **Documenting from memory instead of source code:** The implementation IS the spec. Read `_envelope.py`, `client.py`, `_directory.py` for exact constants, signatures, and behavior.
- **Inconsistent table formatting:** Use the exact same column headers and style as existing tables in each document.
- **Separate files for encryption docs:** D-11 explicitly says extend existing files, not create new ones.
- **Showing deprecated or missing APIs:** Only document public APIs that exist in `__init__.py` `__all__`.

## Don't Hand-Roll

Not applicable -- this is a documentation phase. No code to write.

## Common Pitfalls

### Pitfall 1: HKDF Label Discrepancy Between PROTOCOL.md and Implementation
**What goes wrong:** PROTOCOL.md line 72 shows `info3 = "chromatin-session-fp-v1"` as an HKDF info string for session fingerprint derivation. But in both C++ (`db/net/handshake.cpp:41`) and Python (`_handshake.py:103`), the session fingerprint is computed as `SHA3-256(shared_secret + init_pk + resp_pk)`, NOT via HKDF. The HKDF Label Registry must accurately reflect the implementation.
**Why it happens:** The PROTOCOL.md diagram was written before implementation; the fingerprint derivation method changed.
**How to avoid:** The HKDF Label Registry should list only the four actual HKDF info strings. Add a note that the session fingerprint uses SHA3-256 directly, not HKDF. Do not "correct" the existing handshake diagram (that's a separate concern), but the new registry section must be accurate.
**Warning signs:** If the registry lists `chromatin-session-fp-v1` as an HKDF label, it is wrong.

### Pitfall 2: Transport HKDF Labels Use "chromatin" not "chromatindb" Prefix
**What goes wrong:** CONTEXT.md D-04 refers to transport labels as "chromatindb-session-keys-v1" (shorthand). The actual info strings are `chromatin-init-to-resp-v1` and `chromatin-resp-to-init-v1` (note: "chromatin" prefix, not "chromatindb"). The DARE and envelope labels use "chromatindb" prefix.
**Why it happens:** The transport labels were defined in v1.0.0, the DARE/envelope labels later. Different naming conventions evolved.
**How to avoid:** Copy labels verbatim from source code: `_handshake.py:101-102` for transport, `db/crypto/master_key.cpp:70` for DARE, `_envelope.py:41` for envelope.
**Warning signs:** Any HKDF label not matching source code exactly.

### Pitfall 3: write_to_group Signature Has Directory Parameter
**What goes wrong:** `write_to_group(data, group_name, directory, ttl)` takes a Directory instance as a parameter. README docs must show this clearly -- it's not like `write_encrypted` which only needs recipients.
**Why it happens:** Group resolution requires a cached Directory for member lookup.
**How to avoid:** Show the full signature with all parameters in the API table. Tutorial must show Directory instantiation before `write_to_group` call.

### Pitfall 4: write_encrypted Recipients=None Means Self-Only
**What goes wrong:** Documenting `recipients` as required. It's optional -- `None` (or omitted) means encrypt to self only (CLI-04).
**Why it happens:** Looking at signature without reading the docstring.
**How to avoid:** Explicitly document both patterns: `write_encrypted(data, ttl)` for self-only and `write_encrypted(data, ttl, recipients=[...])` for multi-recipient.

### Pitfall 5: Envelope Nonce Is Random, Not Counter-Based
**What goes wrong:** Readers might confuse envelope data nonce with transport AEAD nonce. Transport uses counter-based nonces (4 zero bytes + 8-byte BE counter). Envelope data nonce is `secrets.token_bytes(12)` -- random.
**Why it happens:** Both use ChaCha20-Poly1305 AEAD with 12-byte nonces, but the nonce generation strategy differs.
**How to avoid:** D-06 requires explicit documentation of random nonce. The PROTOCOL.md spec should have a note contrasting with transport nonce.

### Pitfall 6: DEK Wrapping Uses Zero Nonce (Safely)
**What goes wrong:** Documenting the DEK wrapping nonce as random or counter-based. It's all-zeros, which is safe because the KEK is unique per KEM encapsulation (never reused).
**Why it happens:** Zero nonce seems wrong without understanding the cryptographic rationale.
**How to avoid:** Document zero nonce explicitly in the spec with the safety rationale: "Zero nonce is safe because the HKDF-derived KEK is unique per KEM encapsulation -- the key is never reused."

### Pitfall 7: Content-Addressed Dedup Breaks on Encrypted Data
**What goes wrong:** Users expect content deduplication to work with encrypted blobs. It doesn't because identical plaintext produces different ciphertext (random DEK, random nonce).
**Why it happens:** This is a fundamental property of randomized encryption.
**How to avoid:** Add a one-liner note in the PROTOCOL.md envelope section. The user's specifics section (CONTEXT.md) explicitly requests this.

### Pitfall 8: GroupEntry Is a Public Re-Export
**What goes wrong:** Not documenting GroupEntry in the README. It's exported from `chromatindb.__init__` and is a user-facing type.
**Why it happens:** GroupEntry is defined in `_directory.py` (private module) but re-exported.
**How to avoid:** Check `__init__.py` `__all__` list for all public types. GroupEntry, DirectoryEntry, Directory are all public.

## Code Examples

### Verified API Signatures (from source code)

#### Encryption Operations (from client.py)

```python
# write_encrypted (client.py:745-767)
async def write_encrypted(
    self,
    data: bytes,
    ttl: int,
    recipients: list[Identity] | None = None,
) -> WriteResult:
    """Encrypt data and write as a blob."""

# read_encrypted (client.py:769-789)
async def read_encrypted(
    self, namespace: bytes, blob_hash: bytes
) -> bytes | None:
    """Fetch and decrypt an encrypted blob."""

# write_to_group (client.py:791-825)
async def write_to_group(
    self,
    data: bytes,
    group_name: str,
    directory: Directory,
    ttl: int,
) -> WriteResult:
    """Encrypt data for all group members and write as a blob."""
```

#### Directory Operations (from _directory.py)

```python
# Admin operations
async def delegate(self, delegate_identity: Identity) -> WriteResult
async def create_group(self, name: str, members: list[Identity]) -> WriteResult
async def add_member(self, group_name: str, member: Identity) -> WriteResult
async def remove_member(self, group_name: str, member: Identity) -> WriteResult

# User operations
async def register(self, display_name: str) -> bytes  # returns blob_hash
async def list_users(self) -> list[DirectoryEntry]
async def get_user(self, display_name: str) -> DirectoryEntry | None
async def get_user_by_pubkey(self, pubkey_hash: bytes) -> DirectoryEntry | None
async def list_groups(self) -> list[GroupEntry]
async def get_group(self, group_name: str) -> GroupEntry | None
def refresh(self) -> None
```

#### Directory Constructor (from _directory.py)

```python
# Admin mode
Directory(client, admin_identity)

# User mode (non-admin)
Directory(client, user_identity, directory_namespace=admin_ns)
```

### Verified Binary Format Constants (from _envelope.py)

```python
ENVELOPE_MAGIC = b"CENV"                      # 0x43454E56
ENVELOPE_VERSION = 0x01
CIPHER_SUITE_ML_KEM_CHACHA = 0x01
_STANZA_SIZE = 1648                            # 32 + 1568 + 48
_FIXED_HEADER_SIZE = 20                        # 4 + 1 + 1 + 2 + 12
_KEM_CT_SIZE = 1568                            # ML-KEM-1024 ciphertext
_KEM_PK_HASH_SIZE = 32                         # SHA3-256 of KEM pubkey
_WRAPPED_DEK_SIZE = 48                         # 32-byte DEK + 16-byte AEAD tag
_MAX_RECIPIENTS = 256
_HKDF_LABEL = b"chromatindb-envelope-kek-v1"
_ZERO_NONCE = b"\x00" * 12                     # DEK wrapping nonce
```

### Verified HKDF Labels (from source code)

| Label | Source File | Purpose | Salt | IKM | Output |
|-------|------------|---------|------|-----|--------|
| `chromatin-init-to-resp-v1` | `_handshake.py:101` | Transport send key (initiator perspective) | empty | ML-KEM shared secret | 32 bytes |
| `chromatin-resp-to-init-v1` | `_handshake.py:102` | Transport recv key (initiator perspective) | empty | ML-KEM shared secret | 32 bytes |
| `chromatindb-dare-v1` | `db/crypto/master_key.cpp:70` | Data-at-rest encryption key | empty | Node master key | 32 bytes |
| `chromatindb-envelope-kek-v1` | `_envelope.py:41` | Envelope KEK for DEK wrapping | empty | ML-KEM shared secret (per-recipient) | 32 bytes |

Note: Session fingerprint (`chromatin-session-fp-v1` shown in PROTOCOL.md handshake diagram) is NOT an HKDF derivation in the implementation. Both C++ (`db/net/handshake.cpp:41`) and Python (`_handshake.py:103`) compute it as `SHA3-256(shared_secret + init_pk + resp_pk)`.

### Envelope Binary Format (for PROTOCOL.md tables)

**Fixed header (20 bytes):**

| Offset | Size | Field | Encoding | Description |
|--------|------|-------|----------|-------------|
| 0 | 4 | magic | `CENV` (0x43454E56) | Envelope format identifier |
| 4 | 1 | version | uint8 | Format version (0x01) |
| 5 | 1 | suite | uint8 | Cipher suite (0x01 = ML-KEM-1024 + ChaCha20-Poly1305) |
| 6 | 2 | recipient_count | big-endian uint16 | Number of recipient stanzas (1-256) |
| 8 | 12 | data_nonce | raw bytes | Random nonce for data AEAD encryption |

**Per-recipient stanza (1648 bytes each, sorted by pk_hash):**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 32 | kem_pk_hash | SHA3-256 of recipient's ML-KEM-1024 public key |
| 32 | 1568 | kem_ciphertext | ML-KEM-1024 encapsulation ciphertext |
| 1600 | 48 | wrapped_dek | AEAD-encrypted DEK (32-byte key + 16-byte tag) |

**Data payload (variable):**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 20 + N*1648 | variable | ciphertext | ChaCha20-Poly1305 encrypted data + 16-byte tag |

**Total envelope size:** 20 + (N * 1648) + len(plaintext) + 16 bytes

### Encryption Exceptions (from exceptions.py)

```python
NotARecipientError    # Identity not in envelope recipient list
MalformedEnvelopeError  # Invalid magic, version, or truncated
DecryptionError       # AEAD authentication failed
DirectoryError        # Group not found, not admin, etc.
```

## State of the Art

Not applicable -- documentation phase. No library or technology changes.

## Open Questions

None. All source code is shipped and all format decisions are locked in CONTEXT.md. The source code provides every detail needed for accurate documentation.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest (existing) |
| Config file | `sdk/python/pyproject.toml` |
| Quick run command | `cd sdk/python && python -m pytest tests/ -x -q` |
| Full suite command | `cd sdk/python && python -m pytest tests/ -q` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DOC-01 | PROTOCOL.md has envelope spec + HKDF registry | manual-only | Manual review: verify byte tables match `_envelope.py` constants | N/A |
| DOC-02 | README has encryption API section | manual-only | Manual review: verify method names match `client.py` and `_directory.py` | N/A |
| DOC-03 | Tutorial has encryption workflow | manual-only | Manual review: verify code examples are syntactically correct and use current API | N/A |

**Justification for manual-only:** Documentation changes produce no executable artifacts. The verification is that written text accurately reflects source code. The existing 457 SDK tests validate the underlying code -- this phase validates the docs themselves.

### Sampling Rate
- **Per task commit:** Verify docs match source code APIs (manual diff)
- **Per wave merge:** Full SDK test suite (`cd sdk/python && python -m pytest tests/ -q`) to confirm no accidental code changes
- **Phase gate:** Manual review of all three updated documents against source code

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. Documentation-only phases do not generate new test files.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/_envelope.py` -- all envelope format constants, encrypt/decrypt signatures
- `sdk/python/chromatindb/client.py` -- write_encrypted, read_encrypted, write_to_group signatures
- `sdk/python/chromatindb/_directory.py` -- Directory class, GroupEntry, all method signatures
- `sdk/python/chromatindb/identity.py` -- Identity.generate(), save(), load(), from_public_keys()
- `sdk/python/chromatindb/crypto.py` -- HKDF labels not here but re-exports referenced
- `sdk/python/chromatindb/_handshake.py:101-103` -- transport HKDF labels
- `sdk/python/chromatindb/exceptions.py` -- full exception hierarchy
- `sdk/python/chromatindb/__init__.py` -- public API surface (__all__)
- `db/crypto/master_key.cpp:70` -- DARE HKDF label
- `db/PROTOCOL.md` -- existing format patterns, section structure (859 lines)
- `sdk/python/README.md` -- existing API table format (83 lines)
- `sdk/python/docs/getting-started.md` -- existing tutorial format (187 lines)

### Secondary (MEDIUM confidence)
- `.planning/phases/78-documentation-polish/78-CONTEXT.md` -- user decisions for doc structure

### Tertiary (LOW confidence)
None.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no dependencies to verify (documentation only)
- Architecture: HIGH -- all patterns observed directly from existing documents
- Pitfalls: HIGH -- all discrepancies verified by reading actual source code vs. PROTOCOL.md

**Research date:** 2026-04-02
**Valid until:** Indefinite (source code is frozen, documentation is stable)
