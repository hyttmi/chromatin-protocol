---
phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
plan: 03
subsystem: wire-codec
tags: [flatbuffers, sha3-256, ml-dsa-87, signing, canonical-form, catch2, phase122]

# Dependency graph
requires:
  - phase: 122-01
    provides: Regenerated blob_generated.h with post-122 schema (signer_hint, no namespace_id/pubkey)
  - phase: 122-02
    provides: Storage owner_pubkeys DBI + delegate-hint resolver (will consume extract_pubk_signing_pk)
provides:
  - BlobData struct post-122 shape (signer_hint:32B, data, ttl, timestamp, signature)
  - build_signing_input(target_namespace, ...) -- parameter rename, byte-identical output (D-01)
  - extract_pubk_signing_pk(data) helper -- returns span<const uint8_t,2592> into PUBK body
  - encode_blob / decode_blob rebuilt against post-122 FlatBuffer Blob (signer_hint-only)
  - [codec][phase122] golden-vector test pinning SHA3-256 byte-output (Pitfall #8 defense)
  - [codec][phase122] cross-namespace differs test (D-13 defense at codec layer)
  - [phase122][schema] test suite locking post-122 BlobData + Blob FB layout + size-shrink invariant
affects: [122-04, 122-05, 122-06, 122-07]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Golden-vector test: pin SHA3-256 byte-output to a pre-computed hex literal to defend wire-protocol invariants against silent refactors"
    - "Cross-namespace differs assertion at codec layer: lock D-13 replay defense before engine verify path consumes it"
    - "Strict decode with no fallback path (feedback_no_backward_compat)"

key-files:
  created:
    - db/tests/test_schema_phase122.cpp
  modified:
    - db/wire/codec.h
    - db/wire/codec.cpp
    - db/tests/wire/test_codec.cpp
    - db/CMakeLists.txt

key-decisions:
  - "build_signing_input parameter rename namespace_id -> target_namespace produces BYTE-IDENTICAL SHA3-256 output (D-01). Zero wire-format change. Pure naming clarity: signer commits to target_namespace, not signer_hint -- delegate writes with multi-namespace authority cannot replay cross-namespace."
  - "Golden vector for Pitfall #8 defense baked as hex literal (9cca5c30990ceaddb06f9e6019578162a4c9bbb2bbc6b72ea6ba1737d1836e9f) computed offline via Python hashlib.sha3_256 on canonical input (ns=0..31, data=0xDEADBEEF, ttl=86400 BE32, ts=1700000000 BE64). No REQUIRE(false) two-pass capture placeholder anywhere in the test."
  - "decode_blob throws std::runtime_error when signer_hint absent or not 32 bytes. Strict -- no zero-pad, no fallback, per feedback_no_backward_compat."
  - "Dropped the old 'decode_blob rejects wrong pubkey size' test: the post-122 schema has no pubkey field, so the test is meaningless. Replaced with 'decode_blob rejects missing signer_hint' covering the new invariant."
  - "Dropped #include db/crypto/signing.h from codec.cpp: no longer references Signer::PUBLIC_KEY_SIZE (post-122 has no inline pubkey). YAGNI cleanup."

patterns-established:
  - "Schema-level test file (test_schema_phase122.cpp) sits flat under db/tests/ (not db/tests/wire/) -- it locks the schema contract across the wire + blob layers, not a single component. Tagged [phase122][schema] for filter access."
  - "Standalone per-cpp compile verification (g++ -std=c++20 -c file.cpp) when chromatindb_lib target link is blocked by co-dependent plans. Keeps Plan 03 shippable ahead of the test_helpers.h cascade owned by Plan 04."

requirements-completed: [SC#2, SC#3, D-01, D-13]

# Metrics
duration: 10min
completed: 2026-04-20
---

# Phase 122 Plan 03: Codec Refactor (signer_hint + build_signing_input) Summary

**Post-122 wire-codec landed: BlobData shape flipped to signer_hint-only, build_signing_input renamed to target_namespace (byte-identical SHA3 output), extract_pubk_signing_pk helper added, and the byte-identity invariant locked by a pre-computed golden-vector test.**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-04-20T07:18:00Z
- **Completed:** 2026-04-20T07:29:02Z
- **Tasks:** 3
- **Files modified:** 4 (+1 created)

## Accomplishments

- `db/wire/codec.h`: BlobData drops `namespace_id` + `pubkey`, adds `signer_hint` (32B). `build_signing_input` parameter renamed to `target_namespace` (D-01 byte-identity preserved). `extract_pubk_signing_pk(data) -> span<const uint8_t,2592>` helper inlined adjacent to `is_pubkey_blob` for reuse by Plan 04's engine registration path.
- `db/wire/codec.cpp`: `encode_blob` writes `signer_hint` to the FlatBuffer builder (no more namespace_id/pubkey vectors). `decode_blob` throws on missing or wrong-size `signer_hint` (strict, no fallback). `build_signing_input` sponge order preserved (target_namespace, data, ttl_be32, timestamp_be64).
- `db/tests/wire/test_codec.cpp`: round-trip + ForceDefaults + independence tests updated to post-122 shape. NEW: golden-vector test pinning SHA3-256 output to `9cca5c30990ceaddb06f9e6019578162a4c9bbb2bbc6b72ea6ba1737d1836e9f`. NEW: cross-namespace differs assertion (D-13). NEW: `decode_blob rejects missing signer_hint`.
- `db/tests/test_schema_phase122.cpp`: 4 TEST_CASEs tagged `[phase122][schema]` — BlobData shape lock (via static_assert + runtime defaults), FlatBuffer accessor surface (build+decode via generated API), size-shrink invariant (< 500 bytes for minimal blob vs 2750+ pre-122), and PUBK body size pin (4164 bytes unchanged).
- Standalone `g++ -std=c++20 -c` verified for `codec.cpp`, `test_codec.cpp`, and `test_schema_phase122.cpp` against flatbuffers + catch2 + liboqs headers. All three compile cleanly.

## Task Commits

Each task was committed atomically (single-repo, `--no-verify` per parallel-executor contract):

1. **Task 1: codec.h BlobData + extract_pubk_signing_pk + renamed param** — `d00df1fc` (feat)
2. **Task 2: codec.cpp encode/decode/build_signing_input for post-122** — `7fb3622f` (feat)
3. **Task 3: golden-vector + cross-namespace differs + schema tests** — `88a0972c` (test)

## Files Created/Modified

- `db/wire/codec.h` — BlobData post-122 shape, renamed build_signing_input param, extract_pubk_signing_pk helper.
- `db/wire/codec.cpp` — encode_blob/decode_blob/build_signing_input implementations for post-122 schema. Dropped unused #include db/crypto/signing.h.
- `db/tests/wire/test_codec.cpp` — make_test_blob + round-trip updated; NEW golden-vector + cross-namespace differs + missing-signer_hint tests.
- `db/tests/test_schema_phase122.cpp` — NEW. 4 TEST_CASEs under `[phase122][schema]`.
- `db/CMakeLists.txt` — added `tests/test_schema_phase122.cpp` to the `chromatindb_tests` source list.

## Decisions Made

- **Golden vector derivation:** Plan pre-supplied the expected SHA3-256 hex literal (`9cca5c30...6e9f`). Verified independently against Python `hashlib.sha3_256` on the canonical input — matches byte-for-byte. No REQUIRE(false) two-pass capture placeholder committed at any point (the plan's defensive grep gate `! grep -q "REQUIRE(false)" db/tests/wire/test_codec.cpp` passes).
- **Strict decode_blob (feedback_no_backward_compat):** `decode_blob` now throws when `fb_blob->signer_hint()` is absent or wrong size. No fallback to zero-initialized signer_hint, no commented-out branches. Replacement test `decode_blob rejects missing signer_hint` covers this explicitly by building a FlatBuffer Blob with `signer_hint = 0` via the generated CreateBlob API and asserting the throw.
- **Dropped `#include db/crypto/signing.h` from codec.cpp:** the pre-122 code referenced `Signer::PUBLIC_KEY_SIZE` in the pubkey size check; that code is deleted, the include is now dead weight. YAGNI cleanup alongside the field removal.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 — Blocking] Added `#include <flatbuffers/flatbuffers.h>` and `#include "db/wire/blob_generated.h"` to test_codec.cpp**
- **Found during:** Task 3 (adding `decode_blob rejects missing signer_hint` test)
- **Issue:** The new test constructs a FlatBuffer Blob with a zero `signer_hint` offset via the generated `CreateBlob` API. Previously `test_codec.cpp` only included `db/wire/codec.h` + `db/crypto/hash.h` and relied on the codec API; the new test requires the raw FlatBuffers builder API.
- **Fix:** Added `#include <flatbuffers/flatbuffers.h>` and `#include "db/wire/blob_generated.h"` at the top of `test_codec.cpp`.
- **Files modified:** `db/tests/wire/test_codec.cpp`
- **Verification:** Standalone `g++ -std=c++20 -c db/tests/wire/test_codec.cpp ...` compiles cleanly.
- **Committed in:** `88a0972c` (Task 3 commit).

**2. [Rule 1 — Bug / Scope] Replaced `decode_blob rejects wrong pubkey size` test with `decode_blob rejects missing signer_hint`**
- **Found during:** Task 3
- **Issue:** The pre-122 test built a blob with a wrong-sized `pubkey` vector to assert the codec's size check throws. Post-122 there is no `pubkey` field — the test is meaningless and would not even compile (references `blob.pubkey` and `blob.namespace_id`).
- **Fix:** Replaced with an analogous negative test for the new invariant: `decode_blob` must throw when `signer_hint` is absent or not 32 bytes. Constructed manually via the generated FlatBuffer API to bypass the `BlobData` struct (which always carries a 32-byte `signer_hint`).
- **Files modified:** `db/tests/wire/test_codec.cpp`
- **Verification:** Standalone compile; grep gate `! grep -q "rejects wrong pubkey size"` passes.
- **Committed in:** `88a0972c` (Task 3 commit).

**3. [Rule 1 — Bug / Cleanup] Removed `#include "db/crypto/signing.h"` from codec.cpp**
- **Found during:** Task 2
- **Issue:** The only consumer of the include was `chromatindb::crypto::Signer::PUBLIC_KEY_SIZE` inside the pre-122 `decode_blob` pubkey size check. That branch is deleted in Task 2; the include is dead weight and should not be left around (project YAGNI guideline).
- **Fix:** Dropped the `#include "db/crypto/signing.h"` line.
- **Files modified:** `db/wire/codec.cpp`
- **Verification:** Standalone compile clean; `grep -n "Signer" db/wire/codec.cpp` returns empty.
- **Committed in:** `7fb3622f` (Task 2 commit).

---

**Total deviations:** 3 auto-fixed (1 blocking-include, 1 test-scope replacement, 1 dead-include cleanup). All three are mechanical correctness fixes directly caused by the post-122 schema flip; none introduce new behavior outside the plan. No Rule 4 architectural decisions.

**Impact on plan:** Zero scope creep. The plan explicitly anticipated standalone per-file compile validation ("g++ -std=c++20 -c" as fallback when `chromatindb_tests` link can't close until Plan 04 lands). All three auto-fixes were necessary to make those compiles pass.

## Issues Encountered

- **chromatindb_lib / chromatindb_tests full-target link blocked by Plan 04/05:** expected and noted in the plan. `db/engine/engine.cpp`, `db/peer/*`, `db/sync/*`, and `db/tests/test_helpers.h` still reference the pre-122 `BlobData::namespace_id` / `BlobData::pubkey` fields. Plan 04 (engine refactor) and Plan 05 (sync refactor) carry that cascade. For Plan 03, per-TU standalone compile is the acceptance ceiling — all three edited/new TUs compile cleanly.
- **Handoff note for verifier / Plan 04 executor:** After Plan 04 lands the `test_helpers.h` cascade and the engine refactor, run:
  ```
  ./build/chromatindb_tests "[phase122][schema],[phase122][codec],[codec]" --reporter compact
  ```
  All new tests (golden-vector, cross-namespace differs, missing-signer_hint, shape lock, FB accessor surface, size-shrink, PUBK-size pin) should execute green. The codec round-trip + ForceDefaults + independence tests under `[codec]` should also pass.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- **Plan 04 (engine refactor):** unblocked on the codec layer. `extract_pubk_signing_pk(data)` is ready to be called from the PUBK registration path; `build_signing_input(target_namespace, ...)` is the verified hot-path signature-input builder. Engine must now: (a) resolve `blob.signer_hint` via `Storage::get_owner_pubkey` (owner write) or `delegation_map` (delegate write), (b) call `build_signing_input(target_namespace_from_transport, data, ttl, timestamp)`, and (c) verify ML-DSA-87 against the resolved pubkey.
- **Plan 05 (sync refactor):** same codec-layer contract. PUBK-first gate on the replicated ingest path must reuse the same helper pattern as engine.cpp.
- **Test gate:** Defensive grep `! grep -q "REQUIRE(false)" db/tests/wire/test_codec.cpp` passes — no placeholder capture ever committed.

## Self-Check

Files verified to exist on disk:
- FOUND: db/wire/codec.h (modified)
- FOUND: db/wire/codec.cpp (modified)
- FOUND: db/tests/wire/test_codec.cpp (modified)
- FOUND: db/tests/test_schema_phase122.cpp (created)
- FOUND: db/CMakeLists.txt (modified, wires new test file)

Commits verified via `git log --oneline 30b5ae9c..HEAD`:
- FOUND: d00df1fc (Task 1 — codec.h)
- FOUND: 7fb3622f (Task 2 — codec.cpp)
- FOUND: 88a0972c (Task 3 — tests)

Golden-vector Python cross-check:
- computed = `9cca5c30990ceaddb06f9e6019578162a4c9bbb2bbc6b72ea6ba1737d1836e9f`
- literal in test = `9cca5c30990ceaddb06f9e6019578162a4c9bbb2bbc6b72ea6ba1737d1836e9f`
- FOUND: match

Standalone compile checks:
- FOUND: /tmp/codec.o (g++ -c db/wire/codec.cpp clean)
- FOUND: /tmp/test_codec.o (g++ -c db/tests/wire/test_codec.cpp clean)
- FOUND: /tmp/test_schema_phase122.o (g++ -c db/tests/test_schema_phase122.cpp clean)

Plan <automated> gates:
- Task 1: PASSED (signer_hint OK, no namespace_id / pubkey, target_namespace OK, extract_pubk_signing_pk OK)
- Task 2: PASSED (no blob.namespace_id / pubkey, blob.signer_hint OK, target_namespace OK, fb_blob->signer_hint OK)
- Task 3: PASSED (test_schema_phase122.cpp exists, wired in CMakeLists, both new TEST_CASEs present, no REQUIRE(false) placeholder)

## Self-Check: PASSED

---
*Phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey*
*Plan: 03 (Wave 2, depends on Plan 01)*
*Completed: 2026-04-20*
