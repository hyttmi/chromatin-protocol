---
phase: 119-chunked-large-files
reviewed: 2026-04-19T00:00:00Z
depth: standard
files_reviewed: 10
files_reviewed_list:
  - cli/src/chunked.h
  - cli/src/chunked.cpp
  - cli/src/commands.cpp
  - cli/src/connection.cpp
  - cli/src/main.cpp
  - cli/src/wire.h
  - cli/src/wire.cpp
  - cli/tests/test_chunked.cpp
  - cli/tests/CMakeLists.txt
  - cli/CMakeLists.txt
findings:
  critical: 1
  warning: 4
  info: 5
  total: 10
status: issues_found
---

# Phase 119: Code Review Report

**Reviewed:** 2026-04-19
**Depth:** standard
**Files Reviewed:** 10
**Status:** issues_found

## Summary

Phase 119 adds chunked large-file upload, delete, and download to `cdb`
built on top of Phase 120 pipeline primitives. The manifest encode/decode,
overwrite guard, pwrite reassembly, incremental SHA3 hasher, and DoS clamp
on `connection.cpp::recv()` (line 692) are all correctly implemented and
well covered by tests in `test_chunked.cpp`.

One critical correctness bug was found that fully explains the reported
live-node hang: every pipelined path that mixes `Connection::send_async`
with *direct* `Connection::recv()` (instead of `recv_for`) causes the
Connection's `in_flight_` counter to leak upward and never drain. Once the
counter reaches `kPipelineDepth = 8`, every subsequent `send_async` enters
the backpressure loop and never escapes — the client keeps consuming
replies into `pending_replies_` but never decrements the counter, so no new
sends ever leave. This matches the observed symptom ("8 writes on disk, no
more WriteAcks flowing back") in `put_chunked`, and is also latent in
`rm_chunked`, `get_chunked`, `cmd::put`, and `cmd::get`.

Secondary issues: inconsistent retry-backoff semantics between
`put_chunked` and `get_chunked`, `put_chunked` retry path double-counts
`in_flight_` on the failed first send, `get_chunked` leaks `fd` on one
error path, and a few Info-level quality items.

## Critical Issues

### CR-01: `Connection::in_flight_` leak — explains the phase-119 live-node hang

**Files:**
- `cli/src/connection.cpp:728-745` (`send_async` / `recv_for`)
- `cli/src/chunked.cpp:183-274` (`put_chunked` drain loop)
- `cli/src/chunked.cpp:365-408` (`rm_chunked` drain loop)
- `cli/src/chunked.cpp:589-674` (`get_chunked` drain loop)
- `cli/src/commands.cpp:580-633` (`cmd::put` drain loop)
- `cli/src/commands.cpp:696-744` (`cmd::get` drain loop)

**Issue:**
`Connection::send_async` increments `in_flight_` on every successful send
(line 743). `Connection::recv_for` decrements it via
`pipeline::pump_recv_for`. But plain `Connection::recv()` does NOT touch
`in_flight_` — it only reads the wire (line 674 in connection.cpp).

`put_chunked`, `rm_chunked`, `get_chunked`, `cmd::put`, and `cmd::get` all
use the two-phase pump shape where Phase A calls `send_async` and Phase B
drains with `conn.recv()`. Each Phase B drain consumes a reply from the
wire but leaves `in_flight_` at the value it reached at the end of Phase
A. On the very next Phase A iteration, `send_async` is called with
`in_flight_` already at `kPipelineDepth`; it enters

```cpp
while (in_flight_ >= Connection::kPipelineDepth) {
    if (!pipeline::pump_one_for_backpressure(...)) return false;
}
```

and `pump_one_for_backpressure` stashes the reply into `pending_replies_`
but never decrements `in_flight_`. The loop never exits until the wire
closes. From the outside this looks like "8 writes landed on disk but no
WriteAcks flowing back", because the client has quietly eaten them all
into `pending_replies_` and is spinning forever trying to drain a 9th.

All five call sites are affected. `put_chunked` is the one that will hang
first because it fills the window fastest.

**Fix:** pick one. Either

(a) Make `Connection::recv()` when it resolves a request aware of
`in_flight_`. Since `recv()` is the only path bypassing the accounting,
decrement there when the message type is an ack/response that correlates
to a send:

```cpp
std::optional<DecodedTransport> Connection::recv() {
    // ... existing body that returns a DecodedTransport ...
    // Whenever recv() produces a reply, it represents one in-flight slot
    // freeing. pump_recv_for / pump_one_for_backpressure must stop
    // owning this bookkeeping.
    if (msg && in_flight_ > 0) --in_flight_;
    return msg;
}
```

But this double-decrements if the caller then passes the same reply path
through `recv_for`. The cleaner option is (b):

(b) Stop using `conn.recv()` in the chunked/pipelined drain loops. Have
the callers track their own in-flight count (the `rid_to_chunk_index.size()`
they already maintain) and use `conn.recv_for` only for the single
"manifest ack" sync point at the end. For the arrival-order drain
semantics (D-08), expose a new primitive `Connection::recv_next_reply()`
that is `recv()` plus `--in_flight_` on success and plus popping from
`pending_replies_` first if non-empty — i.e. the counterpart of
`recv_for` for "any rid". All five call sites then switch to
`recv_next_reply()`.

Option (b) is the correct fix — it preserves the single-reader invariant
and keeps the counter correct. The reviewer recommends (b) with a helper
in `pipeline_pump.h`:

```cpp
template <typename Source>
std::optional<DecodedTransport> pump_recv_any(
    Source&& source,
    std::unordered_map<uint32_t, DecodedTransport>& pending,
    std::size_t& in_flight) {
    // If anything was stashed by backpressure, deliver it first.
    if (!pending.empty()) {
        auto it = pending.begin();
        DecodedTransport msg = std::move(it->second);
        pending.erase(it);
        if (in_flight > 0) --in_flight;
        return msg;
    }
    auto msg = source();
    if (!msg) return std::nullopt;
    if (in_flight > 0) --in_flight;
    return msg;
}
```

and a `Connection::recv_next()` public method wrapping it. Replace every
`conn.recv()` in the five call sites above with `conn.recv_next()`.

## Warnings

### WR-01: `put_chunked` retry path double-counts `in_flight_` on success

**File:** `cli/src/chunked.cpp:207-236`

**Issue:** When the initial `send_async` at line 207 fails (returns
false), `in_flight_` was never incremented because `send_async` only
increments on the `return true` path. The retry loop then calls
`send_async` again with the same `this_rid`; on success the retry
increments `in_flight_` correctly (once). But there is a subtle case: if
the first `send_async` failed *after* pumping backpressure (i.e., source
error inside `pump_one_for_backpressure`), the client already consumed
some wire bytes into `pending_replies_` for requests that were never
in-flight from put_chunked's perspective. Those entries stay in
`pending_replies_` for the lifetime of the Connection and never drain.

**Fix:** In the retry loop, on the final give-up path
(`if (!recovered) return 1;`), close the Connection before returning so
the stale `pending_replies_` entries are released. More importantly,
audit whether `send_async` should back out its counter bookkeeping
(already 0, so a no-op) and clear any inserted replies from the failed
backpressure pump. A simpler, localized fix is: don't retry by looping
`send_async` — if the transport broke, the connection is dead. Surface
the failure, abort the upload, and return 1.

### WR-02: `get_chunked` uses `RETRY_BACKOFF_MS[1..2]` only, never `[0]`

**File:** `cli/src/chunked.cpp:578-587`

**Issue:**
```cpp
auto retry_chunk = [&](uint32_t chunk_idx) -> bool {
    if (attempts[chunk_idx] >= RETRY_ATTEMPTS - 1) return false;
    ++attempts[chunk_idx];
    std::this_thread::sleep_for(
        std::chrono::milliseconds(RETRY_BACKOFF_MS[attempts[chunk_idx]]));
    ...
};
```

`attempts[chunk_idx]` is 0 on entry to the first retry; it is
pre-incremented to 1 before indexing, so the first retry sleeps
`RETRY_BACKOFF_MS[1] = 1000ms`. The second retry uses index 2 (4000ms).
`RETRY_BACKOFF_MS[0] = 250` is dead code on the read path. By contrast
`put_chunked` (lines 215-219) uses `RETRY_BACKOFF_MS[attempt]` with
`attempt = 0..2`, so its first retry sleeps 250ms. The two helpers
disagree on what a "first retry" waits.

D-15 documents "exponential backoff 250/1000/4000ms" as the policy.
`get_chunked` violates D-15.

**Fix:** change the pre-increment to post-index:
```cpp
auto retry_chunk = [&](uint32_t chunk_idx) -> bool {
    if (attempts[chunk_idx] >= RETRY_ATTEMPTS) return false;
    std::this_thread::sleep_for(
        std::chrono::milliseconds(RETRY_BACKOFF_MS[attempts[chunk_idx]]));
    ++attempts[chunk_idx];
    auto r = send_chunk_read(chunk_idx);
    ...
};
```
Now the first retry uses `RETRY_BACKOFF_MS[0] = 250` and the cap is 3
retries total, matching `put_chunked` and D-15.

### WR-03: `get_chunked` `fd` leaks if `verify_plaintext_sha3` fails after close

**File:** `cli/src/chunked.cpp:677-696`

**Issue:** After the main drain loop succeeds, the code does
`::fsync(fd)` (warn-only on failure) and `::close(fd)`. Then it runs
`verify_plaintext_sha3`, which re-opens the file via ifstream. If that
check returns false, it `::unlink`s the path. That's fine. But: if an
exception escapes between `::close(fd)` and `verify_plaintext_sha3`
(e.g. allocation in Sha3Hasher constructor throws bad_alloc), the fd is
already closed so there's no fd leak — but the partially-written file
remains on disk. D-12 requires the output file to be unlinked on *any*
failure.

**Fix:** wrap the re-read block in a try/catch that unlinks on
exception, or (cleaner) RAII the output path: a small guard struct whose
destructor `::unlink`s the path unless `release()` was called.
`get_chunked` would instantiate the guard right after the successful
`::open(fd)`, call `guard.release()` only on the successful return at
line 703. All the mid-function `::unlink` calls in `fail_unlink` and the
sha3-mismatch block become unnecessary.

### WR-04: `cmd::put` / `cmd::get` share the same `in_flight_` leak as CR-01

**Files:**
- `cli/src/commands.cpp:575-633` (cmd::put drain)
- `cli/src/commands.cpp:696-744` (cmd::get drain)

**Issue:** Same failure mode as CR-01 but for single-blob batches. A user
who runs `cdb put f1 f2 ... f9` where all files fit under
`CHUNK_THRESHOLD_BYTES` will hit the same hang on the 9th file. This
isn't observed today because cdb's typical batch is smaller, but it's
the same bug and fixing CR-01 fixes this too.

**Fix:** same as CR-01 — replace `conn.recv()` with a new
`conn.recv_next()` primitive that decrements `in_flight_`.

## Info

### IN-01: `put_chunked` retry never verifies it re-read the same bytes

**File:** `cli/src/chunked.cpp:215-224`

**Issue:** The retry loop re-reads the same slice from disk with
`f.seekg(off)` + `read_next_chunk`. It implicitly trusts that the number
of bytes returned equals `got` from the initial read — but
`read_next_chunk` can legitimately return fewer bytes at EOF. If the
file was truncated between the initial read and the retry read, the
retry would sign a short chunk without updating the manifest's
`total_plaintext_bytes`. The manifest would then be inconsistent.

**Fix:** assert that the retry got-length matches the initial
got-length; if not, abort with an error. The retry loop is supposed to
recover from transient transport failure, not from file mutation under
the feet of the upload.

### IN-02: Sha3Hasher destructor double-releases if `finalize()` was called

**File:** `cli/src/wire.cpp:313-333`

**Issue:** `Sha3Hasher::finalize()` calls
`OQS_SHA3_sha3_256_inc_ctx_release` and sets `impl_->finalized = true`.
The destructor (line 317) only releases if `!finalized`. That's correct.
But the class comment in `wire.h:176-179` says subsequent
`absorb()`/`finalize()` are undefined behavior — the implementation
does not enforce this. A second `finalize()` call after the first will
call `OQS_SHA3_sha3_256_inc_finalize` + `_release` on an already-released
context. Add a `throw` or `assert` on `impl_->finalized` in both
`absorb()` and `finalize()` to turn UB into a defined failure.

**Fix:**
```cpp
void Sha3Hasher::absorb(std::span<const uint8_t> data) {
    if (impl_->finalized) throw std::logic_error("Sha3Hasher: absorb after finalize");
    ...
}
std::array<uint8_t, 32> Sha3Hasher::finalize() {
    if (impl_->finalized) throw std::logic_error("Sha3Hasher: finalize twice");
    ...
}
```

### IN-03: `main.cpp` swallows malformed config.json silently

**File:** `cli/src/main.cpp:216-218`

**Issue:**
```cpp
} catch (...) {
    // Silently ignore malformed config
}
```

User memory rule: "Don't suppress errors with || true" — the spirit of
the rule is that errors the user can fix should be surfaced. A
misspelled JSON key or a bad port number in `~/.cdb/config.json` causes
`cdb` to silently fall back to defaults, which is confusing. Log a
warning to stderr instead.

**Fix:**
```cpp
} catch (const std::exception& e) {
    std::fprintf(stderr, "Warning: ignoring malformed %s: %s\n",
                 config_path.c_str(), e.what());
}
```

### IN-04: `put_chunked` shared timestamp can be racy across retries

**File:** `cli/src/chunked.cpp:164`

**Issue:** The per-upload timestamp is captured once with
`std::time(nullptr)`. All chunks and the manifest share it. This is
intentional per P-119-05 (so the manifest never outlives its chunks).
But note: for a 1 TiB upload over, say, 3 hours, every chunk's
`expiry = timestamp + ttl` is computed from the *start* time, so the
last chunk's effective lifetime is (ttl - 3h). At small ttl this could
cause the last chunks to expire almost immediately after upload. The
comment at line 163 correctly explains the rationale but doesn't warn
about this interaction with small ttls.

**Fix:** in `cmd::put`, if `ttl > 0` and the file size is large enough
that the upload might plausibly take >5% of ttl, warn the user that
their ttl will effectively be reduced. Or enforce a minimum ttl when
the chunked path is taken (e.g. max(ttl, 24h)). Either is a design
change — this finding flags it for consideration, not mandatory fix.

### IN-05: Dead/stale section banner in `wire.h`

**File:** `cli/src/wire.h:160-162, 192-194`

**Issue:** Two identical `// === Tombstone === ` banners appear in
`wire.h` — lines 160-162 and 192-194. The first banner is empty (no
following declarations; the actual declaration appears after the
second banner). Minor readability noise. Remove the first banner.

**Fix:** delete lines 160-162.

---

_Reviewed: 2026-04-19_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
