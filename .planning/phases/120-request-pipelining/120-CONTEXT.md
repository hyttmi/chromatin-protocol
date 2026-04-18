# Phase 120: Request Pipelining - Context

**Gathered:** 2026-04-18
**Status:** Ready for planning

<domain>
## Phase Boundary

Turn every multi-blob operation over a single PQ connection from a strict
send→recv→send→recv chain into a pipelined stream where up to N requests
can be in flight simultaneously. Covers both reads (ReadRequest→ReadResponse)
and writes (Data→WriteAck). No wire-format changes — the `request_id` that
`Connection::send`/`recv` already carries *is* the correlation key; pipelining
is purely a client-side coordination refactor.

**Out of scope:** wire protocol changes, server-side concurrency, per-call
depth tuning (stays hard-coded at 8), dedicated reader threads, async/coroutine
rewrite of the CLI.

</domain>

<decisions>
## Implementation Decisions

### API shape

- **D-01:** Two-layer API. Expose the transport primitives:
  `Connection::send_async(type, payload, rid)` — non-blocking enqueue up to depth
  `Connection::recv_for(rid) -> optional<DecodedTransport>` — wait for a specific reply
  Then build command-level helpers on top: `cmd::batch_get({hashes})` and
  `cmd::batch_put({files})`. Consumers pick the level that fits — future
  chunked-download reassembly (Phase 119) and `delegate @group` (backlog) can
  use primitives directly if the batch shape doesn't match.
- **D-02:** Batch helpers return `std::vector<Result<T, Error>>` — one entry per
  input, independently succeeded or failed. Callers iterate, report per-item.
  Matches current `cdb` lenient behavior (one bad hash doesn't sink the batch).

### Reader model

- **D-03:** Cooperative pump. `recv_for(rid)` loops over `recv()`, stashing any
  off-target replies in an in-memory correlation map keyed by `request_id`.
  When the target rid arrives (or was already stashed), return it. No background
  thread, no mutex, no async runtime — the CLI stays strictly single-threaded.
- **D-04:** The correlation map is a `std::unordered_map<uint32_t, DecodedTransport>`
  on `Connection`. Size is bounded by depth (at most `depth-1` off-target
  replies can be queued before the caller finishes draining), so no cleanup
  policy is needed. If a reply arrives for an unknown rid (server bug), log at
  debug and discard.

### Scope — reads and writes both

- **D-05:** Single mechanism covers `ReadRequest`↔`ReadResponse` (used by
  `cmd::get`) and `Data`↔`WriteAck` (used by `cmd::put`). Implement once, both
  call sites migrate. The existing `request_id` round-trip on both paths is
  already in place.

### Backpressure

- **D-06:** `send_async` blocks the caller when `depth` requests are already
  outstanding. Internally it calls `recv()` to drain one reply (which goes into
  the correlation map), freeing a slot, then enqueues. Natural flow control —
  no unbounded queue, no pushed-back error surface. A 10 GiB chunked download
  self-regulates to 8 in-flight chunks.

### Pipeline depth

- **D-07:** Hard-coded constant, default **8** (matches PIPE-03). No flag, no
  config key, no per-call argument on initial ship. First real demand for a
  different value promotes it to `~/.cdb/config.json` (parallel to the other
  tunables landed in Phase 118).

### Progress / UX

- **D-08:** Each completed item emits one line as it lands, same look as the
  current sequential behavior in `cmd::get` and `cmd::put`. Pipelining means
  lines arrive in completion order, not request order — documented, not a bug.
  `--quiet` still suppresses per-item lines. No progress bar.

### Single-sender invariant

- **D-09:** Existing serialized send queue (per-connection drain coroutine
  documented in project memory) is untouched — it already guarantees AEAD
  nonce monotonicity for the outbound direction. `send_async` enqueues onto the
  same queue; concurrency happens strictly through the "multiple in-flight,
  one-reader, one-writer" shape, not by introducing new threads.

### Claude's Discretion

- Exact name/layout of `send_async` and `recv_for` — could alternately be
  `send(..., async=true)` or a builder API. Pick what reads cleanest.
- How to fold the two existing loops (`cmd::get` multi-hash, `cmd::put` multi-
  file) onto `batch_get`/`batch_put`. Likely drop-in — both already iterate
  with a local `rid` counter.
- rid allocation within a batch — monotonic counter per-batch is fine, match
  current `++rid` pattern.
- Whether `batch_get` should print per-item lines itself or return results for
  the caller to print. Matching current `cmd::get` side-effects is fine.
- Unit-test fixture shape — any approach that exercises "send 8, receive out
  of order, correlate correctly".

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Transport primitives (the thing being extended)
- `cli/src/connection.h` — Connection class surface; send/recv signatures
- `cli/src/connection.cpp` — `Connection::send` (line ~626), `send_chunked`
  (~641), `recv` (~669). Already rid-aware.

### Call sites that become pipelined consumers
- `cli/src/commands.cpp` — `cmd::get` multi-hash loop (currently sequential),
  `cmd::put` multi-file loop (currently sequential). Both maintain a local
  `uint32_t rid = 1; ++rid;` pattern that maps cleanly onto the new primitives.

### Prior phase decisions that constrain this one
- `.planning/phases/117-blob-type-indexing-ls-filtering/117-CONTEXT.md` D-10 —
  ListResponse entry shape. Superseded by the P1 commit (15433f30) that
  extended it to 60 bytes, but no bearing on 120 since pipelining is orthogonal
  to ListResponse layout.
- `.planning/REQUIREMENTS.md` — PIPE-01 (pipelined requests over one PQ conn),
  PIPE-02 (single-reader invariant), PIPE-03 (depth 8 default).

### Project-level constraints
- Project memory: "Per-connection send queue with drain coroutine prevents
  AEAD nonce desync" — send serialization is already solved; don't undo it.
- Project memory: "every co_await is a potential container invalidation point"
  — the correlation map must not be mutated while another reply is being
  decoded into it. Single-threaded cooperative pump (D-03) satisfies this by
  construction.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets

- `Connection::send(MsgType, span, rid)` — already rid-aware, already serialized
  via the send queue. `send_async` can be a thin wrapper that pumps `recv` for
  backpressure and otherwise delegates.
- `Connection::recv() -> optional<DecodedTransport>` — already returns the
  `request_id` of the reply. `recv_for(rid)` is a pump loop on top of it.
- `DecodedTransport` struct — has `type`, `request_id`, `payload`. Moveable,
  stashable in a map without trouble.
- Chunked framing (`send_chunked`, `recv` reassembly branch) — already handles
  large payloads transparently and uses a single rid per logical message.
  Pipelining inherits this for free.

### Established Patterns

- All multi-blob consumers today open one connection, iterate, close. Changing
  them to pipeline is a pure drop-in at the loop body.
- Error handling in the iteration loops is already per-item (`++errors;
  continue;`) — maps directly onto `vector<Result<T, Error>>`.
- `opts.quiet` gating is consistent — new batch helpers inherit the convention.

### Integration Points

- `cmd::get`: replace the inner loop (send ReadRequest, recv ReadResponse,
  decode envelope, write file) with a two-pass pipelined shape: phase 1 fires
  `depth` ReadRequests; phase 2 drains replies via `recv_for(rid)`, decrypts,
  writes, fires one new request per drained reply. Loop until all hashes are
  processed.
- `cmd::put`: identical pattern with `Data` + `WriteAck`. Payload encoding
  (envelope encrypt, flatbuf) is already expensive enough that batching the
  encrypt work and then pipelining the sends is a big win for
  `cdb put a b c d`.
- No changes to the node — server sees the same requests in the same shape,
  just faster-arriving.

</code_context>

<specifics>
## Specific Ideas

No concrete references cited by the user. The design is anchored on existing
project patterns:

- "Don't add a thread, don't add a runtime" — cooperative pump is the minimum
  required change.
- "Pipeline depth 8" — taken from PIPE-03 verbatim, no re-litigation.
- "Per-item results" — matches the lenient error style shipped in the cdb
  sweep (M2 in commit ac892ace: per-hash error line + non-zero exit when any
  item fails).
- "One connection" — reuses the existing single-connection pattern from
  `cmd::get` / `cmd::put`. No connection pooling.

</specifics>

<deferred>
## Deferred Ideas

- **Configurable depth** — `pipeline_depth` in `~/.cdb/config.json` and/or a
  per-call argument. Deferred until someone hits a real tuning need. D-07.
- **Dedicated reader thread / asio coroutine refactor** — rejected options
  from the reader-model question. Bigger rewrite, no current demand,
  cooperative pump is sufficient.
- **Progress bar with N/M counter** — rejected in favor of per-item lines.
  Revisit if very-large-batch UX becomes a complaint.
- **Unbounded internal queue / reject-past-depth modes** — rejected from the
  backpressure question. Blocking send gives natural flow control with the
  smallest surface area.
- **Strict-vs-lenient error mode flag** — rejected. Per-item `Result` already
  lets callers implement either behavior externally if they want.

</deferred>

---

*Phase: 120-request-pipelining*
*Context gathered: 2026-04-18*
