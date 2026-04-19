---
phase: 119-chunked-large-files
reviewed: 2026-04-19T13:00:00Z
depth: standard
files_reviewed: 14
files_reviewed_list:
  - cli/CMakeLists.txt
  - cli/src/chunked.cpp
  - cli/src/chunked.h
  - cli/src/commands.cpp
  - cli/src/connection.cpp
  - cli/src/connection.h
  - cli/src/main.cpp
  - cli/src/pipeline_pump.h
  - cli/src/wire.cpp
  - cli/src/wire.h
  - cli/tests/CMakeLists.txt
  - cli/tests/pipeline_test_support.h
  - cli/tests/test_chunked.cpp
  - cli/tests/test_connection_pipelining.cpp
findings:
  critical: 0
  warning: 3
  info: 5
  total: 8
status: issues_found
---

# Phase 119: Code Review Report

**Reviewed:** 2026-04-19
**Depth:** standard
**Files Reviewed:** 14
**Status:** issues_found

## Summary

Phase 119 delivers chunked large-file upload, cascade-delete, and download
to `cdb`, riding Phase 120 pipelining primitives with a single PQ handshake
per command. The gap-closure plan 119-03 addressed all prior-review
blockers: `Connection::recv_next()` + `pipeline::pump_recv_any` replace the
leaky `recv()` drain (former CR-01), retry backoff uses the documented
250/1000/4000 ms ladder on the first retry (former WR-02), an
`UnlinkGuard` RAII ensures partial outputs are cleaned up even on
exceptions in `get_chunked` (former WR-03), and `main.cpp` narrows the
config.json catch and surfaces the error instead of silently swallowing
it (former IN-03).

The new `pipeline::pump_recv_any` helper is well-tested, underflow-guarded,
and single-sender-safe. The `classify_blob_data` / `refuse_if_exists` /
`plan_chunk_read_targets` / `plan_tombstone_targets` free functions are
pure and covered by unit tests. Manifest encode/decode has invariant
checks on every field it accepts. The `total_size` clamp at
connection.cpp:692 (added in 119-02 for T-119-06) is correct.

**No critical issues remain.** The issues below are lower-severity
correctness concerns (one that can mis-hash on file-mutation-during-upload,
one possible interaction with stale replies, one silent error) and five
Info-level quality items. None block the phase from shipping.

## Warnings

### WR-01: `put_chunked` retry path can send a different plaintext than was hashed

**File:** `cli/src/chunked.cpp:220-249`

**Issue:**
The first successful read at line 211 feeds plaintext into `hasher.absorb`
at line 221, establishing the whole-file `plaintext_sha3` that will be
written into the manifest. If the subsequent `send_async` at line 227
fails and the retry loop (lines 235-249) re-reads the same offset from
disk, the retry uses whatever the file contains NOW — which may not
match what was absorbed if the file was truncated or modified between
the two reads. The CDAT chunk sent to the node and the manifest's
`plaintext_sha3` then disagree, and a subsequent `cdb get` will fail
the final `verify_plaintext_sha3` check even though every chunk was
received intact.

Secondary: if the re-read returns `got == 0` (file shrank below `off`),
the retry silently encrypts and sends an empty plaintext as a valid
signed CDAT blob. The node accepts it; the manifest thinks that slot
holds 16 MiB of data.

**Fix:**
Hash AFTER the send succeeds rather than before, so a retry re-reads and
re-absorbs consistently. Alternatively, reject any retry where `got`
differs from the first read, and reject `got == 0` specifically.

```cpp
// Option A: absorb only on the send path that actually goes on the wire.
std::span<const uint8_t> pt(buf.data(), got);
auto first_got = got;
auto flatbuf = build_cdat_blob_flatbuf(id, ns, pt, recipient_spans,
                                        ttl, timestamp);
const uint32_t this_rid = rid++;
bool sent = conn.send_async(MsgType::Data, flatbuf, this_rid);
if (!sent) {
    bool recovered = false;
    for (int attempt = 0; attempt < RETRY_ATTEMPTS; ++attempt) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(RETRY_BACKOFF_MS[attempt]));
        f.clear();
        f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        got = read_next_chunk(f, buf, chunk_size);
        if (got == 0 || got != first_got) {
            std::fprintf(stderr,
                "Error: chunk %u file changed during retry (was %zu, now %zu)\n",
                next_chunk, first_got, got);
            return 1;
        }
        pt = std::span<const uint8_t>(buf.data(), got);
        flatbuf = build_cdat_blob_flatbuf(id, ns, pt, recipient_spans,
                                           ttl, timestamp);
        if (conn.send_async(MsgType::Data, flatbuf, this_rid)) {
            recovered = true;
            break;
        }
    }
    if (!recovered) { /* ... */ }
}
hasher.absorb(pt);   // absorb only after the bytes are queued
```

### WR-02: `pump_recv_any` decrements `in_flight_` on unknown-rid replies

**Files:**
- `cli/src/pipeline_pump.h:126-143` (`pump_recv_any`)
- `cli/src/chunked.cpp:272-277` (`put_chunked` unknown-rid branch)
- `cli/src/chunked.cpp:415-420` (`rm_chunked` unknown-rid branch)
- `cli/src/chunked.cpp:646-651` (`get_chunked` unknown-rid branch)
- `cli/src/commands.cpp:638-645` (`cmd::put` unknown-rid branch)
- `cli/src/commands.cpp:751-757` (`cmd::get` unknown-rid branch)

**Issue:**
`pump_recv_any` decrements `in_flight_` every time it returns a message,
regardless of whether the message's `request_id` is known to the caller.
Callers log-and-continue on unknown rids (e.g. "stray rid" at
chunked.cpp:273-276) without adjusting `in_flight_`. That means a
duplicate or spurious reply (server bug, or a reply that arrives for a
rid the caller already timed out and abandoned) silently lowers
`in_flight_` below the true number of in-flight requests. The next
`send_async` then sees a window slot that isn't really free and pushes
the actual count above `kPipelineDepth`. The pipeline depth ceases to be
a ceiling.

The live-node E2E would not have caught this — a well-behaved node
returns exactly one reply per rid — but the comment at pipeline_pump.h:112
explicitly frames this counter as "defense-in-depth against the CR-01
class of bug," so it deserves to be robust to stray replies too.

**Fix:**
Decrement `in_flight_` only when the returned message's `request_id` is
owned by the caller. Two options:

1. Push the decrement into the caller by making `pump_recv_any` leave
   the counter alone and exposing a dedicated `release_slot()` on
   Connection (cleanest, matches how `pump_recv_for` only decrements on
   target match).
2. Have callers re-credit `in_flight_` when they discover an unknown
   rid and discard.

```cpp
// At each unknown-rid branch, e.g. chunked.cpp:273-277:
auto it = rid_to_chunk_index.find(resp->request_id);
if (it == rid_to_chunk_index.end()) {
    spdlog::debug("put_chunked: discarding reply for unknown rid {}",
                  resp->request_id);
    // pump_recv_any already decremented in_flight_; re-credit so the
    // pipeline window reflects actual outstanding requests.
    // (Requires Connection::credit_in_flight() accessor.)
    conn.credit_in_flight();
    continue;
}
```

### WR-03: `get_chunked` `expected_len` underflows on a malformed manifest

**File:** `cli/src/chunked.cpp:691-700`

**Issue:**
If `decode_manifest_payload` accepted a manifest where
`chunk_idx * chunk_size_bytes > total_plaintext_bytes` for some non-last
chunk — specifically: a manifest with a large `segment_count` but a tiny
`total_plaintext_bytes` where the last chunk calculation underflows —
then `expected_len = manifest.total_plaintext_bytes - expected_off`
wraps around to a multi-EiB `uint64_t`. The comparison
`pt->size() != expected_len` at line 697 then catches the mismatch (so
no memory corruption), but the user-facing error is "chunk plaintext
length mismatch" which masks the real problem (a malformed manifest
that should have been rejected at decode).

`decode_manifest_payload` at wire.cpp:396 does check
`total_plaintext_bytes > max_plain` (upper bound) but does not enforce
the lower bound `total_plaintext_bytes > (segment_count - 1) *
chunk_size_bytes` — which is the invariant that guarantees every
non-last chunk is fully `chunk_size_bytes` long.

**Fix:**
Add the lower-bound check to `decode_manifest_payload`:

```cpp
// wire.cpp: after the upper-bound check
const uint64_t min_plain = (m.segment_count == 0)
    ? 0
    : (static_cast<uint64_t>(m.segment_count - 1) *
       static_cast<uint64_t>(m.chunk_size_bytes) + 1);
if (m.total_plaintext_bytes < min_plain)                                  return std::nullopt;
```

Alternatively, guard the subtraction in `get_chunked`:

```cpp
if (expected_off > manifest.total_plaintext_bytes) {
    return fail_unlink("manifest offset exceeds total bytes",
                       std::to_string(chunk_idx));
}
uint64_t expected_len = manifest.total_plaintext_bytes - expected_off;
if (chunk_idx + 1 != N) expected_len = manifest.chunk_size_bytes;
```

## Info

### IN-01: `rm_chunked` has no retry policy for tombstone sends

**File:** `cli/src/chunked.cpp:391-403`

**Issue:**
`put_chunked` and `get_chunked` both retry transient `send_async`
failures with the D-15 backoff ladder (250/1000/4000 ms). `rm_chunked`
does not — on any `send_async` failure it immediately increments
`errors` and `completed` and moves on (lines 394-400). For a 64-chunk
manifest, a single dropped packet during tombstone fan-out leaves a
chunk un-tombstoned, the final `errors != 0` check fires, and the user
is told to "idempotent retry." That works, but re-sending 63 already-
tombstoned requests is wasteful when a short retry would have closed
the gap.

**Fix:** Mirror the `put_chunked` retry pattern for the tombstone send,
or document explicitly that D-10 idempotency is the sole recovery
strategy for rm failures (no mid-run retry).

### IN-02: Hard-coded `payload.size() < 41` threshold repeated at five sites

**Files:**
- `cli/src/chunked.cpp:283` (put_chunked WriteAck check)
- `cli/src/chunked.cpp:338` (put_chunked manifest WriteAck check)
- `cli/src/chunked.cpp:426` (rm_chunked DeleteAck check)
- `cli/src/chunked.cpp:452` (rm_chunked manifest DeleteAck check)
- `cli/src/commands.cpp:653` (cmd::put WriteAck check)

**Issue:**
The magic number 41 is `hash:32 + seq:8BE + status:1 = 41 bytes` — the
minimum valid WriteAck/DeleteAck payload. It's repeated across five call
sites with no shared constant. Per the project memory rule ("Never
copy-paste utilities; extract to shared headers"), this should live as
a named constant in `wire.h`.

**Fix:**
```cpp
// cli/src/wire.h
inline constexpr size_t WRITE_ACK_MIN_SIZE = 32 + 8 + 1;   // hash + seq + status
inline constexpr size_t DELETE_ACK_MIN_SIZE = 32 + 8 + 1;  // same shape
```
Then every `< 41` becomes `< WRITE_ACK_MIN_SIZE`.

### IN-03: `put_chunked` printf format for 64-bit sizes uses `%llu`

**Files:**
- `cli/src/chunked.cpp:161-167`
- `cli/src/chunked.cpp:174-176`
- `cli/src/chunked.cpp:350-353`
- `cli/src/chunked.cpp:561-566`
- `cli/src/chunked.cpp:737-739`

**Issue:**
The code casts `uint64_t` to `unsigned long long` before passing to
`%llu`. That works on Linux x86_64 (where both are 64-bit) but is
spelled out more verbosely than needed. C++20 has `PRIu64` from
`<cinttypes>` for the same result with explicit intent, and this
codebase already uses spdlog's fmt formatters elsewhere. Not a bug, just
a legibility ding.

**Fix:** Either adopt `PRIu64` consistently or migrate these stderr
prints to `spdlog::warn`/`error` with native uint64_t formatting.

### IN-04: `Sha3Hasher::finalize` called twice is undefined behavior, not enforced

**File:** `cli/src/wire.cpp:313-333` + `cli/src/wire.h:179-190`

**Issue:**
The header comment (wire.h:176-178) documents that `finalize()` must be
called exactly once, and `Impl::finalized` tracks the state. But
`finalize()` itself never checks `impl_->finalized` — a second call
would invoke `OQS_SHA3_sha3_256_inc_finalize` on a released context.
The destructor guards against double-release via the `finalized` flag,
so the only unsafe case is calling `finalize` twice; all current
callers call it once and the object goes out of scope, so this is
latent, not active. Still, the invariant is cheap to enforce.

**Fix:**
```cpp
std::array<uint8_t, 32> Sha3Hasher::finalize() {
    if (impl_->finalized) {
        throw std::logic_error("Sha3Hasher::finalize called twice");
    }
    // ... existing body ...
}
```

### IN-05: `put_chunked` stdin path is unreachable but not documented

**File:** `cli/src/chunked.cpp:141-168` + `cli/src/commands.cpp:517-523`

**Issue:**
`put_chunked` accepts `path` and opens it with `std::ifstream`. The
caller `cmd::put` only dispatches to `put_chunked` when `fsize >=
CHUNK_THRESHOLD_BYTES` (400 MiB), and the stdin branch
(commands.cpp:517-523) caps stdin reads at `MAX_FILE_SIZE` (500 MiB)
without going through chunked. So `put_chunked` is only ever called for
regular files, and the `filename.empty()` fallback at
chunked.cpp:352-353 ("<stdin>") is dead code as long as that
architecture holds. Not a bug — just an opportunity to simplify or add a
comment so the next reader doesn't assume stdin streaming is already
supported.

**Fix:** Either add a `static_assert`-style comment at the top of
`put_chunked` stating that `path` is always a regular file, or remove
the `filename.empty()` handling.

---

_Reviewed: 2026-04-19T13:00:00Z_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
