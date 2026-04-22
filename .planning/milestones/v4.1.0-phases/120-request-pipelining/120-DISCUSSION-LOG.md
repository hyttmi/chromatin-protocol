# Phase 120: Request Pipelining - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-18
**Phase:** 120-request-pipelining
**Areas discussed:** API shape, Reader model, Scope, Errors, Backpressure, Depth config, Progress

---

## API shape

| Option | Description | Selected |
|--------|-------------|----------|
| High-level batch APIs only | `cmd::batch_get({hashes})` + `cmd::batch_put({files})` handle pipelining internally. Simple for the two obvious consumers. | |
| Low-level primitives only | Expose `send_async(rid)` + `recv_for(rid)` / `recv_any()`. Callers manage rid space and correlation. Maximum flexibility; more code per consumer. | |
| Both — primitives + wrappers | Low-level primitives as the transport layer; batch helpers built on top. Future consumers pick their level. | ✓ |

**User's choice:** Both — primitives + wrappers (recommended)
**Notes:** Phase 119 chunked-download reassembly and the deferred `delegate @group` multi-pubkey path are the future consumers that justify the primitive layer.

---

## Reader model

| Option | Description | Selected |
|--------|-------------|----------|
| Cooperative pump — drain on `recv_for` | When caller asks for a specific rid, pump `recv()` in a loop, stashing off-target replies in a correlation map. No background thread, single-threaded. | ✓ |
| Dedicated reader thread | `std::thread` loop on `recv()`, fan to per-rid promises/queues. Decouples reader from any blocking caller; thread safety overhead. | |
| Asio coroutines | Full async with the existing `io_context`. Conceptually clean; large refactor for CLI-scale workload. | |

**User's choice:** Cooperative pump (recommended)
**Notes:** Keeps the CLI strictly single-threaded. Matches project guidance about "every co_await is a potential container invalidation point" — avoided by construction.

---

## Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Gets + puts both | Same mechanism covers `ReadRequest↔ReadResponse` and `Data↔WriteAck`. Implement once, two call sites migrate. | ✓ |
| Gets only, defer puts | Narrower phase. `put` keeps sequential send+ack loop; revisit later. | |

**User's choice:** Gets + puts both (recommended)
**Notes:** The envelope-encrypt cost in `put` is significant; pipelining the send path after encryption is a measurable win for `cdb put a b c d`.

---

## Errors

| Option | Description | Selected |
|--------|-------------|----------|
| Per-item results, caller decides | Batch API returns `vector<Result<T, Error>>`. Caller iterates. Matches current cdb lenient behavior. | ✓ |
| Abort batch on first error | Cleaner semantics; worst-case UX (one bad blob sinks the batch). | |
| Configurable strict vs lenient | Per-call flag. More surface area; speculative. | |

**User's choice:** Per-item results (recommended)
**Notes:** Aligns with the M2 fix from the cdb sweep — per-item error lines plus non-zero exit when any item fails.

---

## Backpressure

| Option | Description | Selected |
|--------|-------------|----------|
| Block caller until a slot frees | `send_async` drains one reply (stashing it in the correlation map) when saturated, then enqueues. Natural flow control, no memory growth. | ✓ |
| Internal unbounded queue | `send_async` never blocks; memory grows unbounded on very large batches. | |
| Reject past depth, caller retries | `send_async` errors when saturated; pushes backpressure plumbing into every consumer. | |

**User's choice:** Block caller (recommended)
**Notes:** A 10 GiB chunked download self-regulates to 8 in-flight chunks via natural TCP-window-like backpressure.

---

## Depth config

| Option | Description | Selected |
|--------|-------------|----------|
| Hard-coded default only | `constexpr size_t PIPELINE_DEPTH = 8`. No flag, no config. Promote on first real demand. | ✓ |
| Per-call argument | `batch_get(hashes, depth=8)`. Exposes the knob from day one. Speculative. | |
| Config.json + per-call override | `pipeline_depth` key plus per-call override. Maximum flexibility; more surface area. | |

**User's choice:** Hard-coded default (recommended)
**Notes:** PIPE-03 wording unchanged. Matches Phase 118's pattern of only promoting constants to config when there's real operator demand.

---

## Progress

| Option | Description | Selected |
|--------|-------------|----------|
| Per-item line as each completes | Matches today's sequential UX; line order = completion order, not request order (documented). | ✓ |
| Silent, dump at end | Cleaner for scripts; no signal during long batches. | |
| Progress bar with N/M | TTY-dependent, complicates `--quiet`. | |

**User's choice:** Per-item line (recommended)
**Notes:** `--quiet` still suppresses. Completion-order output is an honest reflection of the pipelining, not a bug to hide.

---

## Claude's Discretion

- Exact naming/layout of `send_async` and `recv_for` (could be variants on a builder, or a flag on existing `send`).
- Whether `batch_get` prints per-item lines itself or returns results to the caller for printing — matching `cmd::get`'s current in-line printing is fine.
- rid allocation within a batch (monotonic counter per-batch matches current style).
- Unit test fixture shape — any design that exercises "send 8, receive out of order, correlate correctly" satisfies the invariant.
- Whether to share the correlation-map data structure with the chunked-receive path (currently reassembles within `recv()` itself). Planner's call.

## Deferred Ideas

- Configurable pipeline depth (per-call argument or config.json key) — revisit when an operator actually asks for tuning.
- Dedicated reader thread or asio-coroutine refactor of the CLI — bigger scope, no current need.
- Progress bar with N/M counter — revisit if large-batch UX becomes a complaint.
- Strict-vs-lenient error mode flag on batch APIs — callers can implement either behavior over the per-item `Result` already.
