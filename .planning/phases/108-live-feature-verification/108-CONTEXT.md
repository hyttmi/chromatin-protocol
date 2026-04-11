# Phase 108: Live Feature Verification - Context

**Gathered:** 2026-04-11
**Status:** Ready for planning

<domain>
## Phase Boundary

Verify that pub/sub fan-out, rate limiting, SIGHUP config reload, and SIGTERM graceful shutdown all work correctly in a live relay+node environment. Four requirements: E2E-02, E2E-03, E2E-04, E2E-05.

This phase tests OPERATIONAL BEHAVIORS, not protocol correctness (Phase 107 covered that).

</domain>

<decisions>
## Implementation Decisions

### Test Binary Architecture
- **D-01:** NEW binary `tools/relay_feature_test.cpp` — separate from relay_smoke_test. Smoke test = protocol correctness (single-client, sequential). Feature test = operational behaviors (multi-client, signals, rate limiting). Different concerns, different tools.
- **D-02:** run-smoke.sh updated to run BOTH binaries: smoke test first (protocol), then feature test (operations). Same one-command workflow.

### Multi-Client Pub/Sub (E2E-02)
- **D-03:** Two TCP connections in the same process, sequential blocking (no threads). Client A subscribes, client B writes, read notification from client A's socket. SO_RCVTIMEO on both sockets.
- **D-04:** Notification arrival on client A is guaranteed after client B's write_ack — relay fan-out happens synchronously after ingest.

### Signal Delivery (E2E-04, E2E-05)
- **D-05:** `--relay-pid <PID>` CLI argument on the feature test binary. run-smoke.sh already has $RELAY_PID — pass it through.
- **D-06:** SIGHUP test: modify relay config file (change rate_limit_messages_per_sec), send SIGHUP via kill(), verify behavior changed by observing rate limiting kick in.
- **D-07:** SIGTERM test: connect a client, send SIGTERM via kill(), verify WebSocket close frame (opcode 0x08) arrives before connection drops. This must be the LAST test since it kills the relay.

### Rate Limit Testing (E2E-03)
- **D-08:** Start relay with rate_limit_messages_per_sec=0 (disabled) for all other tests. Rate limit tests configure it via SIGHUP reload.
- **D-09:** Combined SIGHUP + rate limit flow: (1) blast messages with limit=0 → all succeed, (2) modify config to limit=5, (3) SIGHUP, (4) blast again → relay disconnects with close code 4002 after 10 consecutive rejections. Tests both E2E-03 and E2E-04 in one sequence.
- **D-10:** Standalone rate limit test: start relay with limit configured from the start, burst messages, verify disconnect. Proves rate limiting works without SIGHUP dependency.

### SIGTERM Graceful Shutdown (E2E-05)
- **D-11:** SIGTERM test runs LAST (kills the relay). Connect client, send SIGTERM, verify close frame arrives. Relay has 5s drain timer + 2s close handshake timeout.
- **D-12:** After SIGTERM, verify relay process actually exits (waitpid or poll PID). No zombie processes.

### Claude's Discretion
- Helper code reuse from relay_smoke_test (TCP helpers, WS framing, auth) — copy or shared header, Claude decides
- Test ordering within the feature test binary
- Number of messages in burst for rate limit testing (enough to trigger, not excessive)
- Config file modification approach (rewrite whole file vs sed-like edit)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Existing Test Infrastructure
- `tools/relay_smoke_test.cpp` — Current smoke test with TCP/WS/auth helpers to reuse
- `/tmp/chromatindb-test/run-smoke.sh` — Test runner script to extend
- `/tmp/chromatindb-test/relay.json` — Relay config file (modified during SIGHUP test)

### Relay Implementation
- `relay/core/rate_limiter.h` — Token bucket rate limiter, rate=burst, current_rate() getter
- `relay/ws/ws_session.cpp` — Rate limit enforcement in AUTHENTICATED path, Close(4002) after 10 consecutive rejections
- `relay/relay_main.cpp` — SIGHUP handler (rate_limit_rate.store(), TLS reload, metrics_bind), SIGTERM drain-first shutdown (stop acceptor → stop metrics → 5s drain → Close(1001) → 2s close handshake → ioc.stop())
- `relay/core/subscription_tracker.h` — Per-session namespace subscriptions, fan-out logic

### Protocol Reference
- `relay/ws/ws_frame.h` — OPCODE_CLOSE = 0x08 for shutdown verification

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay_smoke_test.cpp`: send_all, recv_all, recv_until, ws_send_text, ws_recv_frame, WsFrame struct, WS upgrade handshake, ML-DSA-87 auth flow, build_signing_input, make_data_message, TestResult + record() pattern
- `relay/identity/relay_identity.h`: RelayIdentity::load_from() for client auth

### Established Patterns
- Blocking TCP sockets with SO_RCVTIMEO (5s) for hang prevention
- JSON request/response via ws_send_text + ws_recv_frame
- TestResult vector with pass/fail summary at end

### Integration Points
- run-smoke.sh: add relay_feature_test invocation after smoke test
- relay.json: rate_limit_messages_per_sec field for SIGHUP test
- Relay PID from script variable $RELAY_PID → --relay-pid argument

</code_context>

<specifics>
## Specific Ideas

- E2E-04 SIGHUP + E2E-03 rate limiting can be tested in a combined flow (D-09) that proves both in one sequence
- SIGTERM test (E2E-05) must be last since it kills the relay process
- Phase 107 added SO_RCVTIMEO which prevents hangs — carry this pattern forward

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 108-live-feature-verification*
*Context gathered: 2026-04-11*
