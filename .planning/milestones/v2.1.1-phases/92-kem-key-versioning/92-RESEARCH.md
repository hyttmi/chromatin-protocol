# Phase 92: KEM Key Versioning - Research

**Researched:** 2026-04-07
**Domain:** ML-KEM-1024 key rotation, identity key ring management, directory versioned resolution
**Confidence:** HIGH

## Summary

Phase 92 adds KEM keypair rotation to the Python SDK. The core change is that an Identity holds a "key ring" -- a list of historical KEM keypairs -- and the directory's UserEntry format gains a `key_version` field so `resolve_recipient()` (new method) can discover the latest KEM public key for a given signing identity. Envelope decryption uses `kem_pk_hash` (already in stanzas) to do O(1) lookup into the key ring, requiring zero envelope format changes.

All changes are SDK-only Python in three files: `identity.py` (key ring storage, rotate_kem, save/load numbered files), `_directory.py` (UserEntry format with key_version, resolve_recipient, _populate_cache highest-version logic), and `_envelope.py` (envelope_decrypt key ring fallback). No C++ node changes, no new wire types, no envelope format changes.

**Primary recommendation:** Implement in three waves -- (1) Identity key ring + rotate_kem + persistence, (2) UserEntry format + directory resolution, (3) envelope_decrypt key ring lookup. Each wave is independently testable.

<user_constraints>

## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Numbered files on disk: `kem.0.sec`/`kem.0.pub` (oldest), `kem.1.sec`/`kem.1.pub`, etc. `kem.sec`/`kem.pub` are always copies of the latest version.
- **D-02:** `Identity.load()` globs `kem.*.sec` to discover all historical keys. Builds key ring ordered by version number.
- **D-03:** Lazy migration: pre-rotation identities (only `kem.sec`/`kem.pub`, no numbered files) are treated as version 0 in memory. No files created on load. First `rotate_kem()` writes `kem.0.*` (copy of original) + `kem.1.*` (new) + updates `kem.sec`/`kem.pub`.
- **D-04:** No backward compatibility. Single UserEntry format with `key_version` field. No v1/v2 distinction, no version byte gymnastics, no fallback parsing.
- **D-05:** Format: `[magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][name:N][key_version:4 BE][kem_sig:variable]`. `kem_sig` signs `(kem_pk || key_version_be)`.
- **D-06:** `register()` always writes this format. Fresh identities use `key_version=0`. After rotation, `key_version` increments.
- **D-07:** `resolve_recipient()` returns the entry with the highest `key_version` for a given signing key.
- **D-08:** No envelope format changes. Stanzas already carry `kem_pk_hash` which identifies which key encrypted the stanza. Decryptor builds a `hash -> secret_key` map from the ring and does O(1) lookup.
- **D-09:** `Identity.rotate_kem()` is offline-only. Generates new KEM keypair, moves old to ring, bumps `key_version`, saves numbered files. No network dependency.
- **D-10:** Caller publishes the updated key via `directory.register()` after rotation. Two-step process: rotate locally, then publish.
- **D-11:** Pre-MVP: no backward compat anywhere. Old UserEntry format blobs are simply not parsed.

### Claude's Discretion
- Key ring internal data structure (list of tuples, dict by version, etc.)
- Exact `rotate_kem()` implementation details (file write ordering, atomicity)
- Test fixture design and mock structure
- Error message wording

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope

</user_constraints>

<phase_requirements>

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| KEY-01 | Owner can rotate KEM keypair via `Identity.rotate_kem()`; old secret keys retained in identity key ring for backward decryption | Identity key ring architecture, numbered file persistence pattern, lazy migration from pre-rotation identities |
| KEY-02 | Directory tracks key version history; UserEntry includes `key_version` field; `resolve_recipient()` returns latest KEM public key | New UserEntry binary format with key_version, highest-version resolution in _populate_cache, new resolve_recipient() method |
| KEY-03 | `write_encrypted()` uses recipient's latest KEM public key; `read_encrypted()` falls back to older keys via `pk_hash` matching | envelope_decrypt key ring map ({pk_hash: kem_obj}), write_encrypted already uses recipient.kem_public_key (no change needed if resolve_recipient returns latest) |

</phase_requirements>

## Architecture Patterns

### Modified Project Structure (SDK only)
```
sdk/python/chromatindb/
  identity.py          # Key ring, rotate_kem(), numbered file save/load
  _directory.py        # New UserEntry format, resolve_recipient(), highest-version cache
  _envelope.py         # envelope_decrypt() key ring fallback
  exceptions.py        # (no changes expected)
sdk/python/tests/
  test_identity.py     # Key ring persistence, rotate_kem, lazy migration
  test_directory.py    # New UserEntry format, resolve_recipient
  test_envelope.py     # Key ring decrypt tests
```

### Pattern 1: Identity Key Ring

**What:** Identity holds an ordered list of all KEM keypairs (historical + current). The "current" keypair is always the last/highest-version entry.

**Recommended data structure:**
```python
# Internal to Identity.__init__
self._kem_ring: list[tuple[int, bytes, oqs.KeyEncapsulation | None]]
#                     version, kem_public_key, kem_obj_or_none
```

A list of `(version, public_key, kem_object_or_none)` tuples, sorted by version ascending. The last entry is always the "current" keypair. For recipient-only identities (from_public_keys), the kem object is None since there's no secret key.

**Why list of tuples over dict:** The ring is small (typically 1-3 entries), ordered access by version is natural, and iteration for building the pk_hash map is straightforward. A dict would add no benefit for this size.

**Backward compatibility of Identity API:** The existing `identity.kem_public_key` property returns the current (latest) KEM public key. This must continue to work -- it returns `self._kem_ring[-1][1]` (the public key of the highest-version entry). The existing `identity._kem` for decapsulation returns `self._kem_ring[-1][2]`.

**When to use:** Always -- Identity always has a key ring. Pre-rotation identities have a ring of length 1 with version 0.

### Pattern 2: Lazy Migration (D-03)

**What:** Pre-rotation identity directories on disk have only `kem.sec`/`kem.pub` (no numbered files). On `load()`, treat as version 0 in memory with ring of length 1. No numbered files are created until first `rotate_kem()`.

**Implementation flow:**
```python
# In Identity.load():
key_path = Path(key_path)
parent = key_path.parent
stem = key_path.stem  # e.g., "identity"

# Glob for numbered KEM files
numbered_files = sorted(parent.glob(f"{stem}.kem.*.sec"))  # Wait -- see naming note below
```

**CRITICAL naming detail:** CONTEXT.md D-01 says `kem.0.sec`/`kem.0.pub`. But the current file convention uses suffixes: `.key`, `.pub`, `.kem`, `.kpub`. The key_path is e.g., `path/to/identity.key`, so sibling files are `identity.pub`, `identity.kem`, `identity.kpub`.

For numbered KEM files, the natural extension pattern is:
- `identity.kem.0` (secret key, version 0)
- `identity.kpub.0` (public key, version 0)
- `identity.kem.1` (secret key, version 1)
- `identity.kpub.1` (public key, version 1)

The glob pattern: `key_path.with_suffix('.kem.*')` won't work with pathlib. Instead, use `parent.glob(f"{key_path.stem}.kem.*")` and filter for numeric suffixes.

Actually, re-reading D-01 more literally: "kem.0.sec / kem.0.pub". This likely means the file names are literally `kem.0.sec` and `kem.0.pub` in the identity directory. But existing code uses `key_path` as the `.key` file path, and derives siblings. The planner should decide on the exact naming convention that fits with the existing `key_path.with_suffix()` pattern.

**Recommendation:** Use `{stem}.kem.{N}` for secret and `{stem}.kpub.{N}` for public, where N is the version number. This keeps the existing suffix convention (`.kem` = secret, `.kpub` = public) and appends a version number. Glob: `parent.glob(f"{stem}.kem.[0-9]*")`.

### Pattern 3: UserEntry Format v2 (D-04, D-05)

**What:** New UserEntry binary format with `key_version` field inserted between name and kem_sig.

**Current format:**
```
[magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][name:N][kem_sig:variable]
```

**New format (D-05):**
```
[magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][name:N][key_version:4 BE][kem_sig:variable]
```

**Breaking change (D-04, D-11):** No backward compat. The `USERENTRY_VERSION` constant should be bumped to `0x02` to reflect the format change. Old entries with version `0x01` are rejected by `decode_user_entry()` (returns None).

**kem_sig signed data change (D-05):** Currently `kem_sig = sign(kem_pk)`. New: `kem_sig = sign(kem_pk || key_version_be)`. This means `verify_user_entry` must also take `key_version` as a parameter, or the signed message must be `kem_pk + struct.pack(">I", key_version)`.

### Pattern 4: Directory Resolution with Key Versioning (D-07)

**What:** `_populate_cache` must pick the highest `key_version` UserEntry for each signing key. New `resolve_recipient()` method provides public API.

**Current bug (by omission):** `_populate_cache` iterates blobs sequentially and `by_pubkey_hash[hash] = entry` overwrites -- last-blob-wins, not latest-version-wins. After D-05, the comparison becomes explicit: keep the entry with the highest `key_version`.

**Implementation:**
```python
# In _populate_cache, after parsing UserEntry:
pk_hash = sha3_256(signing_pk)
existing = by_pubkey_hash.get(pk_hash)
if existing is None or key_version > existing_key_version:
    by_pubkey_hash[pk_hash] = entry
    by_name[display_name] = entry
```

This requires `DirectoryEntry` to carry `key_version` as a field.

**resolve_recipient():** New public method on Directory:
```python
async def resolve_recipient(self, display_name: str) -> Identity | None:
    """Resolve a user's latest KEM public key by display name."""
    entry = await self.get_user(display_name)
    return entry.identity if entry is not None else None
```

This is essentially a thin wrapper around `get_user()` that returns just the Identity. The `get_user()` path already goes through cache which picks highest key_version.

### Pattern 5: Envelope Decrypt Key Ring Fallback (D-08)

**What:** `envelope_decrypt` currently does binary search for `sha3_256(identity.kem_public_key)` (single key). With key ring, it must search for ANY key in the ring.

**Current code flow (envelope_decrypt):**
```python
my_hash = sha3_256(identity.kem_public_key)
idx = bisect.bisect_left(pk_hashes, my_hash)
if idx >= len(pk_hashes) or pk_hashes[idx] != my_hash:
    raise NotARecipientError(...)
# decapsulate with identity._kem
```

**New code flow:**
```python
# Build pk_hash -> kem_obj map from identity's key ring
ring_map = identity._build_kem_ring_map()  # {sha3_256(pk): kem_obj}

# Search each stanza pk_hash against ring_map
matched_idx = None
matched_kem = None
for i, pk_hash in enumerate(pk_hashes):
    if pk_hash in ring_map:
        matched_idx = i
        matched_kem = ring_map[pk_hash]
        break

if matched_idx is None:
    raise NotARecipientError(...)
```

**Performance note:** Stanzas are sorted by pk_hash. The ring is typically 1-3 entries. Linear scan of stanzas with set lookup is O(N) where N is recipient count. This is fine -- ML-KEM decapsulation dominates. Alternatively, for each ring key, do a binary search against stanzas -- O(R * log N) where R is ring size. Either approach works; the planner can choose.

**Recommended approach:** Build `ring_map: dict[bytes, oqs.KeyEncapsulation]` from Identity, iterate stanza pk_hashes linearly checking `if pk_hash in ring_map`. This is simplest and O(N) where N is recipient count (typically < 256).

### Anti-Patterns to Avoid

- **Anti-pattern: Modifying envelope format.** D-08 is explicit: no envelope changes. The `kem_pk_hash` in each stanza already uniquely identifies the key. Do NOT add key_version to envelope stanzas.
- **Anti-pattern: Storing key ring as JSON/manifest.** D-01 is explicit: numbered binary files. No JSON, no SQLite, no manifest file.
- **Anti-pattern: Creating numbered files on load.** D-03 is explicit: lazy migration. Load creates ring in memory only. First rotate_kem() creates numbered files.
- **Anti-pattern: Backward compat for old UserEntry.** D-04/D-11: bump version, old entries rejected.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| KEM key generation | Custom key generation | `oqs.KeyEncapsulation("ML-KEM-1024").generate_keypair()` | Already proven in existing codebase |
| KEM object reconstruction | Manual bytes-to-key | `oqs.KeyEncapsulation("ML-KEM-1024", secret_key=sk_bytes)` | Already used in Identity.load() |
| File globbing for key ring | Manual directory listing | `pathlib.Path.glob()` with numeric suffix filter | Standard library, tested |
| Hash computation | Inline hashlib | `chromatindb.crypto.sha3_256()` | Already used everywhere |

## Common Pitfalls

### Pitfall 1: File Write Ordering in rotate_kem()
**What goes wrong:** If the process crashes mid-rotation, some numbered files exist but `kem.sec`/`kem.pub` still point to old key, or vice versa.
**Why it happens:** Multiple files need to be written atomically.
**How to avoid:** Write in this order: (1) new numbered files first (`kem.N.sec`, `kem.N.pub`), (2) old key's numbered files if not already present (`kem.0.sec`, `kem.0.pub`), (3) update `kem.sec`/`kem.pub` last. This way, if crash happens after step 1 but before 3, load() will see the numbered files and reconstruct correctly. The `kem.sec`/`kem.pub` always point to a valid key.
**Warning signs:** Test with simulated crash (delete files mid-rotation) to verify load() recovery.

### Pitfall 2: Glob Pattern Matching Non-Numeric Suffixes
**What goes wrong:** `glob("identity.kem.*")` matches `identity.kem.bak` or `identity.kem.old`.
**Why it happens:** Glob doesn't filter by numeric suffix.
**How to avoid:** After globbing, filter: `int(path.suffix.lstrip('.'))` and catch ValueError to skip non-numeric.
**Warning signs:** Test with extraneous files in the identity directory.

### Pitfall 3: UserEntry kem_sig Signed Data Mismatch
**What goes wrong:** encode writes `sign(kem_pk || key_version_be)` but verify still uses `verify(kem_pk, kem_sig, signing_pk)` without key_version.
**Why it happens:** verify_user_entry() not updated to include key_version in the verified message.
**How to avoid:** Update both `encode_user_entry` and `verify_user_entry` signatures simultaneously. The signed message MUST be `kem_pk + struct.pack(">I", key_version)`.
**Warning signs:** Existing tests will fail because kem_sig format changes.

### Pitfall 4: _populate_cache Last-Wins vs Highest-Version-Wins
**What goes wrong:** Without explicit version comparison, the cache picks whichever UserEntry blob was iterated last, not the one with highest key_version.
**Why it happens:** Current code just overwrites `by_pubkey_hash[hash] = entry`.
**How to avoid:** Store key_version in DirectoryEntry, compare before overwriting.
**Warning signs:** Test with multiple UserEntry blobs for same signing key with different key_versions.

### Pitfall 5: Identity Constructor Signature Change
**What goes wrong:** Adding `kem_ring` parameter to Identity.__init__ breaks all existing callers (generate, load, from_public_key, from_public_keys, and tests that construct Identity directly).
**Why it happens:** Identity.__init__ is called from multiple places.
**How to avoid:** Keep backward-compatible constructor: if `kem_ring` is not provided, build a ring of length 1 from the existing `kem_public_key` and `kem` parameters. All existing callers continue to work.
**Warning signs:** Run full test suite after constructor change.

### Pitfall 6: envelope_decrypt Accessing Private _kem_ring
**What goes wrong:** `_envelope.py` needs access to the key ring, but it's a private attribute on Identity.
**Why it happens:** envelope_decrypt currently accesses `identity._kem` directly (already a private access pattern).
**How to avoid:** Add a method to Identity: `def _build_kem_ring_map(self) -> dict[bytes, oqs.KeyEncapsulation]` that returns `{sha3_256(pk): kem_obj}` for all ring entries with non-None kem objects. This follows the existing pattern of envelope_decrypt accessing `identity._kem`.
**Warning signs:** Type checkers may warn about private access.

### Pitfall 7: USERENTRY_VERSION Bump Breaking All Existing Tests
**What goes wrong:** Bumping USERENTRY_VERSION from 0x01 to 0x02 means ALL existing test_directory.py tests that encode/decode UserEntry will fail.
**Why it happens:** Tests hardcode expected version byte, magic checks, etc.
**How to avoid:** Update all tests simultaneously. Since D-04 says no backward compat, this is the right approach -- just update everything at once.
**Warning signs:** Run test suite after version bump to find all affected tests.

## Code Examples

### rotate_kem() Core Logic
```python
# Source: Derived from existing Identity.generate() pattern
def rotate_kem(self, key_path: str | Path) -> None:
    """Generate new KEM keypair, retain old in ring, save numbered files."""
    if self._signer is None:
        raise KeyFileError("Cannot rotate KEM on verify-only identity")
    
    key_path = Path(key_path)
    parent = key_path.parent
    stem = key_path.stem
    
    # If first rotation (no numbered files yet), save current as version 0
    current_version = self._kem_ring[-1][0]
    if current_version == 0 and not (parent / f"{stem}.kem.0").exists():
        # Lazy migration: write version 0 files
        old_kem = self._kem_ring[-1]
        (parent / f"{stem}.kem.0").write_bytes(old_kem[2].export_secret_key())
        (parent / f"{stem}.kpub.0").write_bytes(old_kem[1])
    
    # Generate new KEM keypair
    new_version = current_version + 1
    new_kem = oqs.KeyEncapsulation("ML-KEM-1024")
    new_kem_pk = bytes(new_kem.generate_keypair())
    
    # Write new numbered files
    (parent / f"{stem}.kem.{new_version}").write_bytes(new_kem.export_secret_key())
    (parent / f"{stem}.kpub.{new_version}").write_bytes(new_kem_pk)
    
    # Update canonical files (always latest)
    key_path.with_suffix(".kem").write_bytes(new_kem.export_secret_key())
    key_path.with_suffix(".kpub").write_bytes(new_kem_pk)
    
    # Update in-memory ring
    self._kem_ring.append((new_version, new_kem_pk, new_kem))
    self._kem_public_key = new_kem_pk
    self._kem = new_kem
```

### New UserEntry Encode
```python
# Source: Derived from existing encode_user_entry
def encode_user_entry(identity: Identity, display_name: str) -> bytes:
    name_bytes = display_name.encode("utf-8")
    key_version = identity.key_version  # New property
    
    # Sign (kem_pk || key_version_be) per D-05
    signed_data = identity.kem_public_key + struct.pack(">I", key_version)
    kem_sig = identity.sign(signed_data)
    
    return (
        USERENTRY_MAGIC
        + struct.pack("B", USERENTRY_VERSION)  # Now 0x02
        + identity.public_key
        + identity.kem_public_key
        + struct.pack(">H", len(name_bytes))
        + name_bytes
        + struct.pack(">I", key_version)
        + kem_sig
    )
```

### envelope_decrypt Key Ring Lookup
```python
# Source: Derived from existing envelope_decrypt
# Replace the single-key lookup with ring lookup:
ring_map = identity._build_kem_ring_map()
if not ring_map:
    raise NotARecipientError("Identity has no KEM keypair for decryption")

# Scan stanzas for any matching pk_hash
matched_idx = None
matched_kem = None
for i in range(recipient_count):
    offset = stanza_offset + i * _STANZA_SIZE
    pk_hash = data[offset : offset + _KEM_PK_HASH_SIZE]
    if pk_hash in ring_map:
        matched_idx = i
        matched_kem = ring_map[pk_hash]
        break

if matched_idx is None:
    raise NotARecipientError("Identity not in envelope recipient list")

# Extract stanza and decapsulate with matched_kem
stanza_base = stanza_offset + matched_idx * _STANZA_SIZE
kem_ct = data[stanza_base + _KEM_PK_HASH_SIZE : stanza_base + _KEM_PK_HASH_SIZE + _KEM_CT_SIZE]
kem_ss = bytes(matched_kem.decap_secret(kem_ct))
# ... rest of decryption unchanged
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest (version per SDK environment) |
| Config file | `sdk/python/` (pytest runs from here) |
| Quick run command | `cd sdk/python && python -m pytest tests/test_identity.py tests/test_directory.py tests/test_envelope.py -x -q` |
| Full suite command | `cd sdk/python && python -m pytest tests/ -x -q` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| KEY-01a | rotate_kem generates new KEM, retains old | unit | `pytest tests/test_identity.py::test_rotate_kem_generates_new_key -x` | No -- Wave 0 |
| KEY-01b | Key ring save/load roundtrip (numbered files) | unit | `pytest tests/test_identity.py::test_key_ring_save_load_roundtrip -x` | No -- Wave 0 |
| KEY-01c | Lazy migration: pre-rotation identity load as version 0 | unit | `pytest tests/test_identity.py::test_lazy_migration_pre_rotation -x` | No -- Wave 0 |
| KEY-01d | rotate_kem writes kem.0 on first rotation | unit | `pytest tests/test_identity.py::test_first_rotation_writes_version_zero -x` | No -- Wave 0 |
| KEY-02a | New UserEntry format with key_version field | unit | `pytest tests/test_directory.py::TestUserEntryCodec -x` | Yes -- needs update |
| KEY-02b | resolve_recipient returns highest key_version | unit | `pytest tests/test_directory.py::test_resolve_recipient_highest_version -x` | No -- Wave 0 |
| KEY-02c | register() writes new format with key_version | unit | `pytest tests/test_directory.py::TestDirectoryRegister -x` | Yes -- needs update |
| KEY-03a | envelope_decrypt with key ring fallback | unit | `pytest tests/test_envelope.py::test_decrypt_with_rotated_key_ring -x` | No -- Wave 0 |
| KEY-03b | write_encrypted uses latest KEM pubkey (via recipient identity) | unit | `pytest tests/test_client.py::TestEncryptedOps -x` | Yes -- needs update |

### Sampling Rate
- **Per task commit:** `cd sdk/python && python -m pytest tests/test_identity.py tests/test_directory.py tests/test_envelope.py -x -q`
- **Per wave merge:** `cd sdk/python && python -m pytest tests/ -x -q`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/test_identity.py` -- new tests for rotate_kem, key ring persistence, lazy migration (4+ tests)
- [ ] `tests/test_directory.py` -- update existing UserEntry tests for new format, add resolve_recipient tests (3+ tests)
- [ ] `tests/test_envelope.py` -- add key ring decrypt roundtrip test (1+ test)

## Open Questions

1. **Exact file naming convention for numbered KEM files**
   - What we know: D-01 says `kem.0.sec`/`kem.0.pub`. But existing code uses `.kem`/`.kpub` suffixes derived from `key_path.with_suffix()`.
   - What's unclear: Whether to follow D-01 literally (`kem.0.sec`) or adapt to existing suffix convention (`identity.kem.0`/`identity.kpub.0`).
   - Recommendation: Use `{stem}.kem.{N}` and `{stem}.kpub.{N}` to stay consistent with existing suffix convention. D-01 describes the intent (numbered files), the exact naming should match the codebase convention. Claude's discretion area.

2. **DirectoryEntry needs key_version field**
   - What we know: `DirectoryEntry` is a frozen dataclass with `identity`, `display_name`, `blob_hash`. The cache needs key_version for comparison.
   - What's unclear: Whether to add `key_version: int` to DirectoryEntry (public-facing) or keep it internal to cache logic.
   - Recommendation: Add `key_version: int` to `DirectoryEntry`. It's useful metadata for callers (e.g., "which version of this user's key am I encrypting to?").

3. **Identity.key_version property scope**
   - What we know: Identity needs a `key_version` property for encode_user_entry.
   - What's unclear: Should it be derived from `len(self._kem_ring) - 1` or stored explicitly?
   - Recommendation: `self._key_version: int` stored explicitly, matching the version number of the current (latest) ring entry. `key_version` property returns it. This is clearer than deriving from ring length.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/identity.py` -- current Identity implementation, KEM key handling patterns
- `sdk/python/chromatindb/_directory.py` -- current UserEntry format, cache population logic
- `sdk/python/chromatindb/_envelope.py` -- current envelope_decrypt with single-key binary search
- `sdk/python/chromatindb/exceptions.py` -- exception hierarchy
- `sdk/python/chromatindb/types.py` -- DirectoryEntry, ReadResult dataclasses
- `.planning/phases/92-kem-key-versioning/92-CONTEXT.md` -- all locked decisions

### Secondary (MEDIUM confidence)
- `sdk/python/tests/test_identity.py` -- test patterns, fixture conventions
- `sdk/python/tests/test_envelope.py` -- envelope test patterns
- `sdk/python/tests/conftest.py` -- shared fixtures (tmp_dir, identity, load_vectors)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - pure Python SDK work, all libraries already in use (oqs, pathlib, struct)
- Architecture: HIGH - all decisions locked in CONTEXT.md, implementation patterns clear from existing code
- Pitfalls: HIGH - identified from direct code analysis, all verifiable

**Research date:** 2026-04-07
**Valid until:** 2026-05-07 (stable -- no external dependencies changing)
