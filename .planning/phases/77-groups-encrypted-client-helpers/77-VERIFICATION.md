---
phase: 77-groups-encrypted-client-helpers
verified: 2026-04-02T03:10:00Z
status: passed
score: 15/15 must-haves verified
re_verification: false
---

# Phase 77: Groups & Encrypted Client Helpers Verification Report

**Phase Goal:** Users can manage named groups and encrypt/decrypt blobs with simple one-liner helpers on ChromatinClient
**Verified:** 2026-04-02T03:10:00Z
**Status:** PASSED
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Admin can create a named group with an initial member list and the group blob persists via write_blob | VERIFIED | `Directory.create_group` in `_directory.py:434` — checks `_is_admin`, calls `encode_group_entry`, calls `write_blob`, sets `_dirty=True` |
| 2 | Admin can add a member to an existing group via read-modify-write | VERIFIED | `Directory.add_member` in `_directory.py:460` — reads current group via `get_group`, appends member hash, calls `encode_group_entry` + `write_blob` |
| 3 | Admin can remove a member from an existing group via read-modify-write | VERIFIED | `Directory.remove_member` in `_directory.py:494` — reads current group, filters member hash out, encodes and writes new blob |
| 4 | User can list all groups in the directory via cached lookup | VERIFIED | `Directory.list_groups` in `_directory.py:528` — calls `_check_invalidation`, populates cache if needed, returns `list(self._groups.values())` |
| 5 | User can get a specific group by name via cached lookup | VERIFIED | `Directory.get_group` in `_directory.py:541` — returns `self._groups.get(group_name)` after cache check |
| 6 | Non-admin users cannot create, add to, or remove from groups | VERIFIED | All three mutation methods check `if not self._is_admin: raise DirectoryError(...)` as first statement |
| 7 | Directory cache populates both UENT and GRPE blobs in a single namespace scan | VERIFIED | `_populate_cache` in `_directory.py:569` — fallthrough: try `decode_user_entry`, if None try `decode_group_entry`, builds both `cache` and `groups_by_name` dicts |
| 8 | User can call write_encrypted(data, ttl, recipients) to encrypt and store a blob | VERIFIED | `ChromatinClient.write_encrypted` in `client.py:745` — calls `envelope_encrypt(data, all_recipients, self._identity)` then `write_blob(envelope, ttl)` |
| 9 | User can call write_encrypted(data, ttl) with no recipients to encrypt to self only | VERIFIED | `recipients=None` defaults to `all_recipients = []`, envelope_encrypt handles sender auto-inclusion |
| 10 | User can call read_encrypted(namespace, blob_hash) to fetch and decrypt a blob | VERIFIED | `ChromatinClient.read_encrypted` in `client.py:769` — calls `read_blob`, then `envelope_decrypt(result.data, self._identity)` |
| 11 | read_encrypted returns None if blob not found | VERIFIED | `if result is None: return None` at `client.py:787` |
| 12 | read_encrypted raises NotARecipientError if blob exists but caller is not a recipient | VERIFIED | `envelope_decrypt` raises `NotARecipientError` which passes through with no catch in `read_encrypted` |
| 13 | User can call write_to_group(data, group_name, directory, ttl) to encrypt for all group members | VERIFIED | `ChromatinClient.write_to_group` in `client.py:791` — resolves group, loops `get_user_by_pubkey`, delegates to `write_encrypted` |
| 14 | write_to_group raises DirectoryError if group not found | VERIFIED | `if group is None: raise DirectoryError(f"Group not found: {group_name}")` at `client.py:817-818` |
| 15 | GroupEntry is re-exported from chromatindb package | VERIFIED | `GroupEntry` in `__init__.py:8` import block and `__all__` at line 93; `encode_group_entry` and `decode_group_entry` also present |

**Score:** 15/15 truths verified

---

## Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `sdk/python/chromatindb/_directory.py` | GroupEntry dataclass, encode/decode_group_entry, 5 Directory group methods, _populate_cache GRPE extension | VERIFIED | All present: `GROUPENTRY_MAGIC`, `GROUPENTRY_VERSION`, `GROUPENTRY_MIN_SIZE`, `class GroupEntry` (frozen dataclass), `encode_group_entry`, `decode_group_entry`, `create_group`, `add_member`, `remove_member`, `list_groups`, `get_group`, extended `_populate_cache`, `refresh` clears `_groups` |
| `sdk/python/tests/test_directory.py` | Group codec tests and Directory group method tests | VERIFIED | `class TestGroupEntryCodec` (17 tests, line 780), `class TestDirectoryGroups` (18 tests, line 945) |
| `sdk/python/chromatindb/client.py` | write_encrypted, read_encrypted, write_to_group methods on ChromatinClient | VERIFIED | All three async methods present in "Encrypted helpers" section; `from chromatindb._envelope import envelope_decrypt, envelope_encrypt` at line 52 |
| `sdk/python/chromatindb/__init__.py` | GroupEntry and group codec re-exports | VERIFIED | `GroupEntry`, `encode_group_entry`, `decode_group_entry` in imports and `__all__` |
| `sdk/python/tests/test_client.py` | Unit tests for encrypted client helpers | VERIFIED | `class TestEncryptedHelpers` at line 424, 11 tests: `test_write_encrypted_with_recipients`, `test_write_encrypted_self_only_no_recipients`, `test_write_encrypted_self_only_explicit_none`, `test_write_encrypted_empty_list`, `test_read_encrypted_found`, `test_read_encrypted_not_found`, `test_read_encrypted_not_a_recipient`, `test_write_to_group_resolves_members`, `test_write_to_group_not_found`, `test_write_to_group_skips_unresolvable`, `test_write_to_group_empty_group` |

---

## Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `client.py write_encrypted` | `_envelope.py envelope_encrypt` | direct call | WIRED | `envelope_encrypt(data, all_recipients, self._identity)` at `client.py:766` |
| `client.py read_encrypted` | `_envelope.py envelope_decrypt` | direct call | WIRED | `envelope_decrypt(result.data, self._identity)` at `client.py:789` |
| `client.py write_to_group` | `_directory.py Directory.get_group` | parameter call | WIRED | `group = await directory.get_group(group_name)` at `client.py:816` |
| `client.py write_to_group` | `client.py write_encrypted` | self-call after resolution | WIRED | `return await self.write_encrypted(data, ttl, recipients)` at `client.py:824` |
| `_directory.py encode_group_entry` | `_directory.py decode_group_entry` | GRPE binary codec roundtrip | WIRED | Both functions use `GROUPENTRY_MAGIC`; `create_group`/`add_member`/`remove_member` call `encode_group_entry`; `_populate_cache` calls `decode_group_entry` at line 620 |
| `_directory.py Directory.create_group` | `_directory.py encode_group_entry` | group blob encoding | WIRED | `group_data = encode_group_entry(name, member_hashes)` at `_directory.py:455` |
| `_directory.py Directory._populate_cache` | `_directory.py decode_group_entry` | GRPE blob decoding | WIRED | `group_parsed = decode_group_entry(result.data)` at `_directory.py:620`; builds `groups_by_name` with latest-timestamp-wins; assigned to `self._groups` at line 644 |

---

## Data-Flow Trace (Level 4)

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|--------------------|--------|
| `_directory.py Directory.list_groups` | `self._groups` | `_populate_cache` scans live namespace via `list_blobs` + `read_blob` | Yes — real blob reads, not static | FLOWING |
| `_directory.py Directory.get_group` | `self._groups.get(group_name)` | Same as above | Yes | FLOWING |
| `client.py write_encrypted` | `envelope` | `envelope_encrypt(data, all_recipients, self._identity)` | Yes — calls real KEM + AEAD from `_envelope.py` | FLOWING |
| `client.py read_encrypted` | decrypted plaintext | `read_blob` + `envelope_decrypt` | Yes — real blob fetch + decryption | FLOWING |

---

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| Group codec tests pass (35 tests) | `pytest tests/test_directory.py -q -k "group"` | 35 passed, 46 deselected | PASS |
| Encrypted helper tests pass (11 tests) | `pytest tests/test_client.py -q -k "encrypted or write_to_group"` | 11 passed, 10 deselected | PASS |
| Full unit suite passes (479 tests) | `pytest tests/ -q --ignore=tests/test_integration.py` | 479 passed | PASS |
| GroupEntry importable from package | `python3 -c "from chromatindb import GroupEntry"` | Import succeeds (verified via pytest runner) | PASS |
| ChromatinClient has 3 new methods | `hasattr(ChromatinClient, 'write_encrypted')` etc. | All three present | PASS |

Note: `tests/test_integration.py::test_namespace_list` fails — this is a pre-existing live-server test where the test identity's namespace is not present on the KVM swarm. It is unrelated to phase 77 and was failing before this phase.

---

## Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|---------|
| GRP-01 | 77-01 | Admin can create a named group in the directory with an initial member list | SATISFIED | `Directory.create_group` in `_directory.py:434` — admin check + encode_group_entry + write_blob |
| GRP-02 | 77-01 | Admin can add or remove members from a group | SATISFIED | `Directory.add_member` and `Directory.remove_member` — read-modify-write pattern |
| GRP-03 | 77-01 | User can list groups and view group membership | SATISFIED | `Directory.list_groups` and `Directory.get_group` — cached lookup via `_groups` dict |
| GRP-04 | 77-01 | SDK resolves group name to member KEM pubkeys at encrypt-time | SATISFIED | `write_to_group` calls `directory.get_group` then `directory.get_user_by_pubkey` per member to resolve `entry.identity` before calling `write_encrypted` |
| CLI-01 | 77-02 | User can call write_encrypted(data, recipients) to encrypt and store a blob | SATISFIED | `ChromatinClient.write_encrypted(data, ttl, recipients)` in `client.py:745` |
| CLI-02 | 77-02 | User can call read_encrypted(blob_hash) to fetch, find stanza, decrypt and return plaintext | SATISFIED | `ChromatinClient.read_encrypted(namespace, blob_hash)` in `client.py:769` |
| CLI-03 | 77-02 | User can call write_to_group(data, group_name) to encrypt for all group members | SATISFIED | `ChromatinClient.write_to_group(data, group_name, directory, ttl)` in `client.py:791` |
| CLI-04 | 77-02 | User can call write_encrypted(data) with no recipients to encrypt to self only | SATISFIED | `recipients: list[Identity] | None = None` default, routes to `envelope_encrypt(data, [], self._identity)` |

All 8 requirement IDs from PLAN frontmatter accounted for. REQUIREMENTS.md confirms all 8 are marked `[x]` complete with Phase 77.

No orphaned requirements: REQUIREMENTS.md phase mapping shows no additional IDs assigned to Phase 77 beyond these 8.

---

## Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| — | — | None found | — | — |

No TODO, FIXME, placeholder comments, empty returns, or hardcoded empty data found in modified files (`_directory.py`, `client.py`, `__init__.py`, `test_directory.py`, `test_client.py`).

---

## Human Verification Required

None. All observable behaviors are verifiable via unit tests and static analysis. There are no visual, real-time, or external service components in this phase beyond the pre-existing integration test suite (which is unaffected by phase 77 changes).

---

## Summary

Phase 77 fully achieves its goal. All 15 observable truths are verified in the actual codebase:

- **Plan 01 (GRP-01 to GRP-04):** `_directory.py` has all required pieces — `GroupEntry` frozen dataclass, `GROUPENTRY_MAGIC`/`VERSION`/`MIN_SIZE` constants, `encode_group_entry`/`decode_group_entry` binary codec, 5 `Directory` group methods (`create_group`, `add_member`, `remove_member`, `list_groups`, `get_group`), extended `_populate_cache` with UENT-then-GRPE fallthrough and latest-timestamp-wins, `refresh()` clearing `_groups`. 35 tests covering codec roundtrip, edge cases, and method behavior all pass.

- **Plan 02 (CLI-01 to CLI-04):** `client.py` has the three encrypted helper methods wired directly to `envelope_encrypt`/`envelope_decrypt` from `_envelope.py`. `write_to_group` chains through `Directory.get_group` → `get_user_by_pubkey` → `write_encrypted`. `__init__.py` re-exports `GroupEntry`, `encode_group_entry`, `decode_group_entry` in both import block and `__all__`. 11 tests with patched envelope functions verify all call paths.

- **Full unit suite:** 479 tests pass. The one failing test (`test_integration.py::test_namespace_list`) is a pre-existing live-server test unrelated to this phase.

---

_Verified: 2026-04-02T03:10:00Z_
_Verifier: Claude (gsd-verifier)_
