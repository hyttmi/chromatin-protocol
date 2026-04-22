---
phase: 124-cli-adaptation-to-new-mvp-protocol
reviewed: 2026-04-21T00:00:00Z
depth: standard
files_reviewed: 16
files_reviewed_list:
  - cli/CMakeLists.txt
  - cli/src/chunked.cpp
  - cli/src/commands.cpp
  - cli/src/commands_internal.h
  - cli/src/error_decoder.cpp
  - cli/src/main.cpp
  - cli/src/pubk_presence.cpp
  - cli/src/pubk_presence.h
  - cli/src/wire.cpp
  - cli/src/wire.h
  - cli/tests/CMakeLists.txt
  - cli/tests/test_auto_pubk.cpp
  - cli/tests/test_wire.cpp
  - db/peer/peer_manager.cpp
  - db/peer/pex_manager.cpp
  - db/tests/peer/test_peer_manager.cpp
findings:
  critical: 0
  warning: 2
  info: 4
  total: 6
status: issues_found
---

# Phase 124: Code Review Report

**Reviewed:** 2026-04-21
**Depth:** standard
**Files Reviewed:** 16
**Status:** issues_found

## Summary

Phase 124 migrates the CLI wire layer from the pre-122 BlobData (inline pubkey)
shape to the post-122 `signer_hint`-only schema, adds the `BlobWrite=64`
envelope, the `build_owned_blob` central builder, an invocation-scoped
auto-PUBK presence module, the D-05 ErrorResponse decoder, and a D-06 BOMB
cascade in `cmd::rm_batch`. Node-side it ships an inline fix to
`peer_manager.cpp` + `pex_manager.cpp` preventing `peers.json`
ephemeral-port poisoning via a `connect_address()` gate, with a
`[peer][persistence]` regression test.

Overall the change is carefully written: hash-then-sign (`build_signing_input`)
is correct; big-endian encoding is consistent throughout; the
`build_owned_blob` invariant that `signer_hint = SHA3(signing_pubkey)` (never
delegate-controlled) is enforced at the single construction site and locked by
the golden-vector and delegate-mismatch unit tests; `ensure_pubk_impl` is
transport-agnostic and well-mocked; the D-05 decoder strings pass the
literal-equality gate (no phase leaks, no internal-token leaks); the BOMB
cascade expands CPAR manifests via the shared classifier (no
copy-pasted utility); and `submit_bomb_blob` correctly routes BOMBs via
`BlobWrite=64` while keeping single-target tombstones on `Delete=17`.
The node-side `connect_address()` gate is the right fix at the right layer
(ConnectCallback + PEX advertise filter, both places).

The findings below are minor: two warnings (a retry-edge-case and a stale
comment) and four info-level observations about duplication, over-permissive
ack matching, a dead default-argument, and a header-inlining observation.

No critical security issues were found. No signing-path correctness issues
were found. The FlatBuffer vtable offsets match `db/schemas/transport.fbs`
and `db/schemas/blob.fbs` byte-for-byte (verified against the round-trip and
hand-decode `TEST_CASE`s in `test_wire.cpp`).

## Warnings

### WR-01: `put_chunked` retry loop does not re-validate EOF after re-read

**File:** `cli/src/chunked.cpp:237-251`

**Issue:** When the initial `conn.send_async` for a chunk fails, the recovery
loop re-seeks to `off`, re-reads via `read_next_chunk`, and rebuilds
`flatbuf`. The initial-read path at line 214-219 explicitly errors out on
`got == 0` ("premature EOF"), but the retry path does not. If the file on
disk is truncated between the initial successful read and the retry's
re-read, `got` may be 0 on retry and `pt` becomes an empty span.
`build_cdat_blob_flatbuf` will then sign over an empty plaintext; the node
will accept it (the signature still binds target_ns || data || ttl ||
timestamp correctly); and a zero-length CDAT will land in the final
manifest's `chunk_hashes`. The whole-file `hasher` was only absorbed during
the initial successful read (`pt` from before the retry), so
`plaintext_sha3` would catch the mismatch at `get_chunked`'s defense-in-depth
check — but by then the blob is persisted on the node. The initial-read
"premature EOF" invariant is not re-asserted on retry.

Low probability in practice (requires external truncation mid-upload), but
the fix is trivial and the invariant is already elsewhere.

**Fix:**

```cpp
for (int attempt = 0; attempt < RETRY_ATTEMPTS; ++attempt) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(RETRY_BACKOFF_MS[attempt]));
    f.clear();
    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    got = read_next_chunk(f, buf, chunk_size);
    if (got == 0) {
        std::fprintf(stderr,
            "Error: premature EOF on retry of chunk %u of %s\n",
            next_chunk, path.c_str());
        return 1;
    }
    pt = std::span<const uint8_t>(buf.data(), got);
    flatbuf = build_cdat_blob_flatbuf(id, ns, pt, recipient_spans,
                                      ttl, timestamp);
    if (conn.send_async(MsgType::BlobWrite, flatbuf, this_rid)) {
        recovered = true;
        break;
    }
}
```

### WR-02: Stale comment on `LIST_ENTRY_SIZE` contradicts the value

**File:** `cli/src/commands.cpp:109-111`

**Issue:** The comment reads "ListResponse entry stride: hash:32 + seq:8BE +
type:4 = 44 bytes (since Phase 117)" but the constant is defined as `60`.
The actual layout (correctly documented at line 1552 and implemented by the
parser at lines 2022-2026: `hash:32 + seq:8 + type:4 + size:8 + ts:8 = 60`)
is 60 bytes. The comment is a leftover from a pre-Phase-117 intermediate
shape. The runtime value is correct, the parser is correct, and every
consumer references the constant — only the comment misleads a future
reader auditing against the wire spec.

**Fix:**

```cpp
// ListResponse entry stride: hash:32 + seq:8BE + type:4 + size:8BE + ts:8BE = 60 bytes (since Phase 117).
// Kept as a named constant so every consumer references the same value.
static constexpr size_t LIST_ENTRY_SIZE = 60;
```

## Info

### IN-01: Duplicate `LIST_ENTRY_SIZE` constant without cross-check

**File:** `cli/src/commands.cpp:1550-1556`

**Issue:** `kNameListEntrySize = 60` in the anonymous namespace duplicates the
outer-file `LIST_ENTRY_SIZE = 60`. The comment at 1553-1555 argues that
keeping the helper "self-contained" justifies the duplication and that a
`static_assert` cross-check would be "redundant". But two literals for the
same wire invariant, in the same TU, with no guard, is exactly the shape
that bit-rots silently — and the project guideline is
`feedback_no_duplicate_code.md`. A `static_assert(kNameListEntrySize ==
LIST_ENTRY_SIZE, "...")` after the anonymous-namespace definition, or
promoting `LIST_ENTRY_SIZE` to file-scope (outside `cmd::`), would cost
nothing.

**Fix:** Either add the static_assert, or drop `kNameListEntrySize` and
reference `LIST_ENTRY_SIZE` directly (now visible after its definition at
line 111; `enumerate_name_blobs` is in the anonymous namespace at
line 1558+, which is after both declarations).

### IN-02: `submit_bomb_blob` accepts `DeleteAck` despite BlobWrite routing

**File:** `cli/src/commands.cpp:600-604`

**Issue:** `submit_bomb_blob` sends BOMBs via `MsgType::BlobWrite` (per the
Phase 124 Rule-1 fix documented in the body comment at lines 573-582). The
preceding comment explicitly states "The ack type is WriteAck (not
DeleteAck)". But the ack check accepts BOTH `WriteAck` and `DeleteAck`:

```cpp
if ((resp->type != static_cast<uint8_t>(MsgType::WriteAck) &&
     resp->type != static_cast<uint8_t>(MsgType::DeleteAck)) ||
    resp->payload.size() < 32) {
    return std::nullopt;
}
```

In correct node operation the `DeleteAck` branch is unreachable. Accepting
it silently hides a potential node-side protocol drift (a node that
incorrectly answered BlobWrite-for-BOMB with DeleteAck would pass instead
of surfacing a protocol mismatch).

**Fix:**

```cpp
if (resp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
    resp->payload.size() < 32) {
    return std::nullopt;
}
```

### IN-03: `submit_bomb_blob` default `host_for_errors = "node"` is dead

**File:** `cli/src/commands.cpp:567`

**Issue:** The default-argument stub `host_for_errors = "node"` is
defended in the comment at lines 593-594 as a "backward-compat" hook "for
any caller that hasn't wired host yet". But both production callers
(`cmd::put --replace` at line 890 and `cmd::rm_batch` at line 1512) pass
`opts.host` explicitly. A dead default that produces user-visible strings
like `"...on node node..."` is a paper-cut waiting to happen if a future
caller forgets the argument. The project is active-dev (no backward-compat
guarantees per memory rules), so removing the default is free.

**Fix:** Drop `= "node"` from the signature; the compiler will catch any
caller that forgets to thread `opts.host` through.

### IN-04: `pubk_presence.h` template body pulls `<ctime>` into every includer

**File:** `cli/src/pubk_presence.h:116-117`

**Issue:** `ensure_pubk_impl` computes `std::time(nullptr)` inline in the
header, which forces `<ctime>` (and transitively the full `wire.h`
include graph) into every translation unit that includes
`pubk_presence.h`. The testability motivation (comment at lines 62-73) is
valid and the cost is small, but a minor-TU hygiene improvement would be
to factor `uint64_t pubk_now_seconds()` into `pubk_presence.cpp` and call
it from the template — same testability (the test doesn't care about the
timestamp value), less include bleed.

**Fix:** Optional; accept as-is for Phase 124. If refactored later:

```cpp
// pubk_presence.cpp
uint64_t pubk_now_seconds() {
    return static_cast<uint64_t>(std::time(nullptr));
}
```

---

_Reviewed: 2026-04-21_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
