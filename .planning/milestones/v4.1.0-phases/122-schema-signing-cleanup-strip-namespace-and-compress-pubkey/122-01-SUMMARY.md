---
phase: 122
plan: 01
subsystem: wire-schema
tags:
  - flatbuffers
  - schema
  - protocol-break
  - blob
  - transport
dependency-graph:
  requires: []
  provides:
    - "post-122 Blob FlatBuffer (signer_hint + data + ttl + timestamp + signature)"
    - "BlobWrite=64 TransportMsgType"
    - "BlobWriteBody table {target_namespace, blob}"
    - "regenerated db/wire/blob_generated.h"
    - "regenerated db/wire/transport_generated.h"
  affects:
    - db/wire/codec.h
    - db/wire/codec.cpp
    - db/engine/engine.cpp
    - db/engine/engine.h
    - db/sync/sync_protocol.cpp
    - db/peer/message_dispatcher.cpp
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/tests/test_helpers.h
tech-stack:
  added: []
  patterns:
    - "FlatBuffers schema-first wire format (existing pattern, applied to new BlobWriteBody)"
    - "include directive in transport.fbs to reference Blob from blob.fbs"
key-files:
  created: []
  modified:
    - db/schemas/blob.fbs
    - db/schemas/transport.fbs
    - db/wire/blob_generated.h
    - db/wire/transport_generated.h
decisions:
  - "Added include \"blob.fbs\"; to transport.fbs (was missing) to allow BlobWriteBody.blob:Blob reference"
  - "Used new BlobWrite=64 TransportMsgType (D-08 recommendation) over repurposing Data=8; Data=8 dispatcher branch removal is owned by Plan 05"
metrics:
  duration: 6min
  completed: 2026-04-20T04:43:32Z
  tasks_completed: 3
  files_modified: 4
  commits: 3
requirements:
  - SC#1
  - SC#2
  - "D-07"
  - "D-08"
---

# Phase 122 Plan 01: Schema Signing Cleanup — FlatBuffers Edits + Header Regen Summary

## One-liner

Landed the post-122 FlatBuffers schema break: stripped `namespace_id` and inline `pubkey` from `Blob`, added 32-byte `signer_hint`, added `BlobWrite=64` TransportMsgType + `BlobWriteBody{target_namespace, blob}` envelope; regenerated and committed C++ headers via flatc 25.2.10.

## What Was Built

### Task 1: blob.fbs rewritten to post-122 shape

`db/schemas/blob.fbs` now declares a 5-field Blob table:

```fbs
namespace chromatindb.wire;

table Blob {
  signer_hint:[ubyte];  // 32 bytes: SHA3-256(signing pubkey); key into owner_pubkeys DBI
  data:[ubyte];
  ttl:uint32;
  timestamp:uint64;
  signature:[ubyte];
}
root_type Blob;
```

- `namespace_id:[ubyte]` removed (now derived at the transport envelope per D-07).
- `pubkey:[ubyte]` (2592 bytes) removed (now resolved via `signer_hint` lookup in the new `owner_pubkeys` DBI per D-05; that DBI is owned by Plan 02).
- `signer_hint:[ubyte]` added as the first field, sized 32 bytes per comment.
- `data`, `ttl`, `timestamp`, `signature` byte-for-byte unchanged.
- No `deprecated`-annotated fields, no commented-out lines, no reserved slots — clean break per `feedback_no_backward_compat.md`.

**Commit:** `6a532e61`

### Task 2: transport.fbs — BlobWrite=64 + BlobWriteBody envelope

Three additions to `db/schemas/transport.fbs`:

1. `include "blob.fbs";` at the top (was missing) so `BlobWriteBody.blob:Blob` resolves at codegen.
2. `BlobWrite = 64` appended to the `TransportMsgType` enum (with the required comma added after the previous terminator `ErrorResponse = 63`).
3. New `BlobWriteBody` table appended after `TransportMessage`:

   ```fbs
   table BlobWriteBody {
     target_namespace:[ubyte];  // 32 bytes
     blob:Blob;
   }
   ```

- `Data = 8` enum slot **preserved** (verified: still present and untouched). Removal of the `TransportMsgType_Data` dispatcher branch is owned by Plan 05.
- `BlobTransfer = 13` enum slot untouched. (Note: the plan's done-criterion sanity grep referenced `BlobTransfer = 15`, which is stale text in the plan — the actual value is 13. The intent — "other enum entries unchanged" — is satisfied.)
- All other 63 pre-existing enum entries unchanged.

**Commit:** `55bfc572`

### Task 3: Regenerated FlatBuffers C++ headers

Configured CMake (`cmake -B build -DCMAKE_BUILD_TYPE=Debug`) and built the two regen targets:

```
cmake --build build -j$(nproc) --target flatbuffers_blob_generated flatbuffers_transport_generated
```

This built `flatc` (25.2.10) from the FetchContent FlatBuffers tree, then ran it against both `.fbs` files. Resulting headers:

- `db/wire/blob_generated.h` (now 174 lines, was 134): contains `signer_hint()` accessor on `Blob`; `namespace_id()` and `pubkey()` accessors are gone.
- `db/wire/transport_generated.h` (now 515 lines, was 414): contains `TransportMsgType_BlobWrite = 64`; new `BlobWriteBody` struct with `target_namespace()` and `blob()` accessors; native-type pair `BlobWriteBodyT`, builder, and unpack/pack functions.

Both `_generated.h` files have modification timestamps newer than the `.fbs` files (verified: 2026-04-20 07:42:52 vs. 07:36:57 / 07:37:24), confirming flatc actually ran end-to-end.

**Commit:** `b626f0bf`

## Decisions Made

| Decision                                                                | Rationale                                                                                                                                                                                                                                              |
| ----------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Added `include "blob.fbs";` to transport.fbs                            | Was not present before; required for `BlobWriteBody.blob:Blob` to resolve at codegen. Direct, mechanical addition with no side effects.                                                                                                                |
| New `BlobWrite=64` over repurposing `Data=8`                            | Per D-08: dispatcher auditability + wire-log clarity. The 64-th enum slot is free; the `Data=8` slot is left intact for Plan 05 to delete its dispatcher branch as a separate, isolated change.                                                        |
| Used CMake regen path (vs. invoking flatc manually from another worktree) | The plan explicitly calls for the CMake build path. CMake configure took 270s (asio + Catch2 + flatbuffers FetchContent), then flatc build was incremental on the FetchContent cache. Headers are deterministic regardless of which path generates them. |

## Deviations from Plan

None of the auto-fix rules (Rule 1-3) fired; the plan executed cleanly as written. One pre-existing minor inaccuracy in the plan text was noted and worked around without code change:

**Note: Plan done-criterion text was stale on `BlobTransfer = 15` sanity check**

- **Found during:** Task 2 verification.
- **Issue:** Plan 122-01 done criterion for Task 2 specified `grep -c "BlobTransfer = 15" db/schemas/transport.fbs returns 1`. The actual schema has `BlobTransfer = 13` (verified by reading `db/schemas/transport.fbs` directly). This is a typo in the plan, not a bug in the schema.
- **Action:** Treated the *intent* of the criterion (other enum entries unchanged) as satisfied by direct verification that `BlobTransfer = 13` is present and untouched, plus all 63 other entries are intact. No change to plan or schema.
- **Files modified:** None.
- **Commit:** N/A.

## Authentication Gates

None. This was a pure schema + codegen task with no network/auth surface.

## Verification Results

### Task 1 — blob.fbs

```
signer_hint OK
no namespace_id OK
no pubkey OK
root_type OK
```

### Task 2 — transport.fbs

```
BlobWrite = 64 OK
BlobWriteBody table OK
target_namespace OK
blob:Blob OK
BlobTransfer untouched OK    (value 13, not the stale "15" in plan text)
comma after ErrorResponse OK
Data = 8 preserved OK
include blob.fbs OK
```

### Task 3 — regenerated headers

```
signer_hint accessor OK
no namespace_id accessor OK
no pubkey() accessor OK
TransportMsgType_BlobWrite OK
BlobWriteBody OK
blob() accessor on BlobWriteBody OK
target_namespace() accessor on BlobWriteBody OK
header timestamps newer than .fbs OK
```

### Plan-level success criteria

- [x] **SC#1** (schema-level): `Blob.namespace_id` field is gone (verified by grep + regenerated header inspection).
- [x] **SC#2** (schema-level): `signer_hint` (32 bytes per comment) replaces inline `pubkey`.
- [x] **D-07**: `BlobWriteBody { target_namespace, blob }` table present in `transport.fbs` and reflected in `transport_generated.h`.
- [x] **D-08**: `BlobWrite = 64` `TransportMsgType` added (not repurposing `Data = 8`).
- [x] **Pitfall #2 avoided**: regenerated `_generated.h` headers committed alongside `.fbs` edits in this same plan.

## Threat Flags

None new beyond the threat register already documented in the plan. No files outside the planned set were created or modified, and no new network/auth/file/schema surface was introduced.

## Known Stubs

None. This plan is pure schema definition + codegen; no UI/data-flow stubs are possible at this layer. The intentional downstream compile breakage in `codec.cpp` / `engine.cpp` / `sync_protocol.cpp` is the coordination mechanism for Plans 02-07 and is documented in the plan's Task 3 acceptance criteria — it is not a stub.

## Expected Downstream Compile State

As designed, `chromatindb_lib` will fail to compile after this plan because dependent C++ files still reference `Blob::namespace_id()` and `Blob::pubkey()` accessors that no longer exist. This breakage is the intended coordination signal for downstream plans:

- `db/wire/codec.{h,cpp}` — owned by Plan 02 / 03 (BlobData struct shape + signing input rename).
- `db/engine/engine.cpp` — owned by Plan 03 (verify path rewrite).
- `db/sync/sync_protocol.cpp` — owned by Plan 04/05 (sync wire envelope updates).
- `db/peer/message_dispatcher.cpp` — owned by Plan 05 (BlobWrite handler wiring + Data=8 branch removal).
- `db/storage/storage.{h,cpp}` — owned by Plan 02 (owner_pubkeys DBI + namespace_id rename per D-10).
- `db/tests/**` — owned by Plan 06/07 (test_helpers shape churn + new PUBK-first test cases per D-12).

## Performance Metrics

- **Tasks completed:** 3 / 3
- **Files modified:** 4 (`db/schemas/blob.fbs`, `db/schemas/transport.fbs`, `db/wire/blob_generated.h`, `db/wire/transport_generated.h`)
- **Commits:** 3 (`6a532e61`, `55bfc572`, `b626f0bf`)
- **Duration:** ~6 minutes (most of which was first-time CMake configure: ~270s for asio + Catch2 + flatbuffers FetchContent)
- **Lines diff:** schema +13/-7; generated headers +154/-43

## Self-Check: PASSED

Files claimed exist:
- FOUND: `db/schemas/blob.fbs`
- FOUND: `db/schemas/transport.fbs`
- FOUND: `db/wire/blob_generated.h`
- FOUND: `db/wire/transport_generated.h`
- FOUND: `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-01-SUMMARY.md`

Commits claimed exist:
- FOUND: `6a532e61` (Task 1: blob.fbs)
- FOUND: `55bfc572` (Task 2: transport.fbs)
- FOUND: `b626f0bf` (Task 3: regenerated headers)
