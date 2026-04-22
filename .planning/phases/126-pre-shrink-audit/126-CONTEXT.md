# Phase 126: Pre-shrink Audit — Context

**Gathered:** 2026-04-22
**Status:** Ready for planning

## Phase Boundary

Pin the invariant that no send path can produce a TLS/AEAD frame exceeding the proposed 2 MiB `MAX_FRAME_SIZE`, without actually changing `MAX_FRAME_SIZE` itself (that's FRAME-01 in Phase 128). Scope is an audit of the send side plus a small set of assertions/tests that will fail if a future code change breaks the invariant.

Originally scoped as a per-response-type worst-case payload-size sweep. During discuss-phase the scope was reframed — see D-03. The name "Pre-shrink Audit" is preserved but the audit subject is now the streaming invariant, not response payload sizes.

Requirements covered: AUDIT-01, AUDIT-02.

## Implementation Decisions

### Audit subject (reframed during discuss)

- **D-01:** The audit subject is the **send-side streaming invariant**: every large payload must go through `Connection::send_message`'s `>= STREAMING_THRESHOLD` branch, which sub-frames at 1 MiB plaintext. No code path may construct a single frame with ciphertext ≥ `MAX_FRAME_SIZE`.
- **D-02:** The original "per-response-type worst-case size table" work is **deferred to Phase 131 (Documentation Reconciliation)** as an operator-facing documentation deliverable. It is not a gate, because under the streaming invariant no response can produce an oversized frame by construction.
- **D-03:** Reframe rationale: `db/net/connection.cpp:972` auto-streams any payload ≥ `STREAMING_THRESHOLD = 1 MiB` before the frame cap is consulted. So `MAX_FRAME_SIZE` is not protecting us from oversized responses — it is a DoS bound on the 4-byte length prefix (`u32 BE` allows declared sizes up to 4 GiB). The audit must pin the streaming invariant, not measure payloads.

### Invariant pinning

- **D-04:** Add a **runtime assertion** in the single-frame send primitive (`Connection::enqueue_send`'s non-chunked branch, or the lower-level `send_encrypted` — researcher to pick the exact site) that asserts `payload.size() < STREAMING_THRESHOLD`. Any future bypass produces a loud test failure.
- **D-05:** Add a **compile-time invariant** in `db/net/framing.h`: `static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD)`. Trivially holds today (110 MiB ≥ 2 MiB); still holds after Phase 128 shrinks `MAX_FRAME_SIZE` to 2 MiB. Pins the numeric relationship so a future tweak to one without the other fails the build.
- **D-06:** **Do not adopt** an AST/grep CI check for `enqueue_send` call sites. D-04 and D-05 together cover the call-site concern without the maintenance burden of pattern-matching.

### Call-site audit outcome handling

- **D-07:** The audit inventories every send-side code path that reaches the AEAD frame write — `send_message`, `send_message_chunked`, `enqueue_send`, `send_encrypted`, and any direct byte-pusher that skips the `send_message` wrapper. Each is documented with the path it takes.
- **D-08:** If the inventory surfaces a call site that can emit a single frame ≥ `STREAMING_THRESHOLD` bypassing `send_message`, **fix in this phase** by rerouting through `send_message`. Expected to be a one-line change per site.
- **D-09:** If the inventory surfaces **zero** bypass sites, that is the success condition — no fix work, just the assertions and the unit test. Phase 128's frame shrink lands with the invariant already pinned.

### Test design

- **D-10:** One round-trip unit test that covers both sides of the streaming boundary: a `STREAMING_THRESHOLD − 1` payload sent via `send_message` produces a single frame under the cap; a `STREAMING_THRESHOLD + 1` payload auto-streams into multiple sub-frames each under the cap. Both decrypt and decode correctly.
- **D-11:** Tests reference `STREAMING_THRESHOLD` directly for the boundary value — not `MAX_FRAME_SIZE` — so the test is meaningful today (under the 110 MiB cap) and still meaningful after Phase 128 shrinks the cap.
- **D-12:** Test location: colocated with existing framing tests (`db/tests/net/test_framing.cpp` or `db/tests/net/test_connection.cpp`). Researcher picks the better fit.

### Phase 128 interlock

- **D-13:** `MAX_FRAME_SIZE = 2 MiB` is **not changed in Phase 126**. The actual constant change lives in Phase 128 (FRAME-01). Phase 126's deliverable is "the streaming invariant is pinned and the audit found no bypass sites" — a green light for Phase 128 to shrink the constant safely.

### Claude's Discretion

- Exact location of the runtime assertion (`enqueue_send` vs `send_encrypted` vs a shared helper) — pick whichever is the narrowest waist through which every non-chunked send must pass.
- Exact phrasing of the `static_assert` message — clear enough that a reader understands the relationship.
- Whether to add a separate debug-only build-time trace/log for the first sub-frame of every chunked send (nice-to-have, not required).
- Naming of the new test — something along the lines of `streaming_invariant_single_frame_bound` and `streaming_invariant_auto_chunks_large_payload`.

## Specific Ideas

No specific references — this is a mechanical audit phase. The user's probe during discuss ("should we simply remove the whole cap of frame size") is captured as the motivation for D-03's reframe: `MAX_FRAME_SIZE` remains as a DoS bound, not as a payload-size bound, and the phase work reflects that distinction.

## Canonical References

### Wire protocol
- `db/PROTOCOL.md` §frame format — current 4-byte BE length prefix + AEAD-wrapped ciphertext definition
- `db/net/framing.h` — `MAX_FRAME_SIZE`, `STREAMING_THRESHOLD`, `FRAME_HEADER_SIZE`, `make_nonce`, `write_frame`, `read_frame`
- `db/net/framing.cpp:49` — current receive-side enforcement of `MAX_FRAME_SIZE`
- `db/net/connection.cpp:972–978` — `send_message` auto-streaming branch
- `db/net/connection.cpp:1016–1030` — `drain_send_queue` chunked sub-frame loop at `STREAMING_THRESHOLD`

### v4.2.0 scope
- `.planning/REQUIREMENTS.md` AUDIT-01, AUDIT-02 — requirement text this phase delivers
- `.planning/ROADMAP.md` Phase 126 block — goal, depends-on, success criteria
- PROJECT.md Key Decisions table row **"MAX_FRAME_SIZE = 110 MiB (10% headroom)"** — the legacy decision this phase documents as outdated; Phase 128 will revise it

### Related prior art (read for context, not scope)
- `db/tests/net/test_framing.cpp:151–194` — existing tests exercising frame oversize rejection at 110 MiB
- `db/tests/net/test_connection.cpp:795` — test that caps `len > 110 * 1024 * 1024` in a local mock reader; will need updating with Phase 128, not here

## Existing Code Insights

### Reusable Assets
- `Connection::send_message` (`db/net/connection.cpp:969`) — the single public send API; already carries the streaming branch. Audit verifies no bypass.
- `Connection::enqueue_send` — the post-encode queue inject; non-chunked messages land here. Candidate site for D-04's runtime assertion.
- `Connection::send_encrypted` (used inside `drain_send_queue`) — the actual AEAD + TCP write primitive. Alternative site for D-04.
- Existing framing tests — provide the harness for D-10.

### Risk Areas (for audit attention)
- `Connection::send_message_chunked` — builds the chunked header separately via `send_encrypted`. The header itself is small by construction; verify.
- `drain_send_queue`'s chunked branch — sends sub-frames directly via `send_encrypted`. Each sub-frame is already bounded to `STREAMING_THRESHOLD` by the loop (`db/net/connection.cpp:1019`). Confirm with a static read.
- Handshake / AuthSignature / initial HELLO paths — may use direct AEAD writes that skip `send_message`. Verify each is a fixed-size small payload.
- PEX announce / SyncNamespaceAnnounce / BlobNotify broadcasts — verify these route through `send_message`, not a custom fan-out path.

### Known Pitfalls (from Accumulated Context)
- **Coroutine lifetime:** Direct AEAD write paths that cross coroutine suspension points have bitten us before (see PROJECT.md "recv_sync_msg executor transfer after offload()" decision). This phase's assertions should not introduce new suspension points.
- **AEAD nonce desync:** Chunked sends must not interleave with other messages (`Connection::drain_send_queue` serializes this explicitly). Audit must not propose restructuring the send queue.

## Out of Scope

- Changing `MAX_FRAME_SIZE` from 110 MiB to 2 MiB — that's Phase 128 (FRAME-01).
- Producing a per-response-type worst-case size table — deferred to Phase 131 as operator docs.
- CI-time AST/grep scans — rejected by D-06 as maintenance-heavy and redundant with D-04.
- Any change to `STREAMING_THRESHOLD`. It stays 1 MiB.
- Changing `BatchReadResponse.MAX_CAP` — during discuss-phase it was flagged as possibly redundant at 4 MiB under the new blob cap, but it does not break the frame invariant and Phase 131 can document the relationship.

## Deferred Ideas

- Per-response-type worst-case size table as operator documentation → Phase 131 (DOCS-02 or new DOCS item if the table doesn't fit existing scope).
- Observability: counter `chromatindb_send_frame_bytes_histogram` for continuous monitoring of actual frame sizes in production — defer to operator tooling milestone (post-MVP).

## Next Step

`/gsd-plan-phase 126` — produce PLAN.md covering the inventory, the two assertions (runtime + static), the round-trip test, and any one-line reroute fixes if the inventory surfaces bypass sites.
