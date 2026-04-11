# Phase 108: Live Feature Verification - Research

**Researched:** 2026-04-11
**Domain:** E2E operational behavior testing (pub/sub, rate limiting, SIGHUP, SIGTERM)
**Confidence:** HIGH

## Summary

Phase 108 tests four operational behaviors of the relay in a live relay+node environment: pub/sub notification fan-out (E2E-02), rate limiting enforcement (E2E-03), SIGHUP config reload (E2E-04), and SIGTERM graceful shutdown (E2E-05). All four are already implemented and unit-tested; this phase proves they work end-to-end.

The implementation is a new C++ binary `tools/relay_feature_test.cpp` that reuses patterns from the existing `relay_smoke_test.cpp`. The binary needs two TCP connections for pub/sub (subscriber + writer), signal delivery via `kill()`, config file modification for SIGHUP verification, and close-frame detection for SIGTERM. The `run-smoke.sh` script gets updated to run both binaries.

**Primary recommendation:** Build relay_feature_test.cpp by extracting helper code (TCP, WebSocket framing, auth) into a shared header, then implement tests in order: pub/sub first (validates multi-client), rate limit + SIGHUP combined (validates config reload), SIGTERM last (kills relay).

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** NEW binary `tools/relay_feature_test.cpp` -- separate from relay_smoke_test. Smoke test = protocol correctness (single-client, sequential). Feature test = operational behaviors (multi-client, signals, rate limiting). Different concerns, different tools.
- **D-02:** run-smoke.sh updated to run BOTH binaries: smoke test first (protocol), then feature test (operations). Same one-command workflow.
- **D-03:** Two TCP connections in the same process, sequential blocking (no threads). Client A subscribes, client B writes, read notification from client A's socket. SO_RCVTIMEO on both sockets.
- **D-04:** Notification arrival on client A is guaranteed after client B's write_ack -- relay fan-out happens synchronously after ingest.
- **D-05:** `--relay-pid <PID>` CLI argument on the feature test binary. run-smoke.sh already has $RELAY_PID -- pass it through.
- **D-06:** SIGHUP test: modify relay config file (change rate_limit_messages_per_sec), send SIGHUP via kill(), verify behavior changed by observing rate limiting kick in.
- **D-07:** SIGTERM test: connect a client, send SIGTERM via kill(), verify WebSocket close frame (opcode 0x08) arrives before connection drops. This must be the LAST test since it kills the relay.
- **D-08:** Start relay with rate_limit_messages_per_sec=0 (disabled) for all other tests. Rate limit tests configure it via SIGHUP reload.
- **D-09:** Combined SIGHUP + rate limit flow: (1) blast messages with limit=0, all succeed, (2) modify config to limit=5, (3) SIGHUP, (4) blast again, relay disconnects with close code 4002 after 10 consecutive rejections. Tests both E2E-03 and E2E-04 in one sequence.
- **D-10:** Standalone rate limit test: start relay with limit configured from the start, burst messages, verify disconnect. Proves rate limiting works without SIGHUP dependency.
- **D-11:** SIGTERM test runs LAST (kills the relay). Connect client, send SIGTERM, verify close frame arrives. Relay has 5s drain timer + 2s close handshake timeout.
- **D-12:** After SIGTERM, verify relay process actually exits (waitpid or poll PID). No zombie processes.

### Claude's Discretion
- Helper code reuse from relay_smoke_test (TCP helpers, WS framing, auth) -- copy or shared header, Claude decides
- Test ordering within the feature test binary
- Number of messages in burst for rate limit testing (enough to trigger, not excessive)
- Config file modification approach (rewrite whole file vs sed-like edit)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| E2E-02 | Subscribe/Unsubscribe/Notification fan-out works end-to-end with live blob writes | Multi-client pub/sub test pattern (D-03/D-04), notification JSON schema verified (namespace, hash, seq_num, size, is_tombstone fields) |
| E2E-03 | Rate limiting enforces messages/sec limit and disconnects on sustained violation | Token bucket rate limiter analyzed (rate=burst, 10 consecutive rejection threshold, Close(4002)), standalone + combined SIGHUP test flows (D-09/D-10) |
| E2E-04 | SIGHUP reloads TLS, ACL, rate limit, and metrics_bind without restart | Full SIGHUP handler analyzed in relay_main.cpp (lines 316-359): TLS reload, ACL reload, rate_limit_rate.store(), metrics_bind. Shared atomic propagation to sessions verified. |
| E2E-05 | SIGTERM drains send queues and sends close frames before exit | Full SIGTERM handler analyzed in relay_main.cpp (lines 280-311): stop acceptor -> stop metrics -> 5s drain -> Close(1001) -> 2s close handshake -> ioc.stop(). close() sends encode_close_frame via send_raw. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| chromatindb_relay_lib | N/A | Link target for relay headers + code | Same link target as relay_smoke_test uses |
| nlohmann/json | project-pinned | JSON parsing for config file modification and response validation | Already in use by smoke test |
| liboqs (OQS_SIG) | project-pinned | ML-DSA-87 auth for challenge-response | Required for auth handshake |
| OpenSSL (RAND_bytes) | system 3.3+ | Random masking keys, challenge signing | Already in use by smoke test |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| POSIX signals (kill()) | N/A | Send SIGHUP/SIGTERM to relay PID | D-05, D-06, D-07 |
| POSIX sockets | N/A | Blocking TCP with SO_RCVTIMEO | Same pattern as smoke test |
| sys/wait.h (waitpid) | N/A | Verify relay process exit after SIGTERM | D-12 |
| fstream | N/A | Config file rewriting for SIGHUP test | D-06, D-09 |

**Installation:** No new dependencies. Everything links against `chromatindb_relay_lib`.

## Architecture Patterns

### Recommended Project Structure
```
tools/
  relay_smoke_test.cpp       # Existing -- protocol correctness
  relay_feature_test.cpp     # NEW -- operational behaviors
  relay_test_helpers.h       # NEW -- shared helpers extracted from smoke test
  relay_uds_tap.cpp          # Existing
  CMakeLists.txt             # Updated to add relay_feature_test target
```

### Pattern 1: Shared Test Helpers Header
**What:** Extract TCP, WebSocket, auth, and test-result helpers from relay_smoke_test.cpp into `tools/relay_test_helpers.h` (header-only, inline functions).
**When to use:** Both smoke test and feature test need identical TCP send/recv, WS framing, auth flow, and test result tracking.
**Why:** Avoids ~300 lines of code duplication. Both binaries include the same header. Simple -- no library target needed, just an include.

Functions to extract:
- `send_all()`, `recv_all()`, `recv_until()` -- TCP helpers
- `ws_send_text()`, `WsFrame`, `ws_recv_frame()`, `ws_recv_text()` -- WebSocket framing
- `build_signing_input()`, `make_data_message()` -- blob signing
- `TestResult`, `record()`, `results` -- test tracking
- Auth handshake flow (currently inline in smoke test main()) -- extract as `do_auth(fd, identity)` returning bool

```cpp
// tools/relay_test_helpers.h
#pragma once
#include "relay/identity/relay_identity.h"
#include "relay/util/base64.h"
#include "relay/util/hex.h"
#include "relay/ws/ws_frame.h"
#include "relay/ws/ws_handshake.h"
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <oqs/sha3.h>
// ... TCP/WS/auth helper implementations (all static inline)
```

### Pattern 2: Modified ws_recv_frame for Close Frame Detection
**What:** The existing `ws_recv_frame()` returns `nullopt` on `OPCODE_CLOSE` (line 209 of smoke test). For the SIGTERM test (E2E-05), we need to actually capture close frames to verify close code 1001 arrives.
**When to use:** SIGTERM test only.
**Solution:** Add a separate function `ws_recv_frame_raw()` that returns the frame including close frames, or modify ws_recv_frame to use a flag.

```cpp
// Returns frame INCLUDING close frames (does not swallow them)
static std::optional<WsFrame> ws_recv_frame_raw(int fd) {
    // Same as ws_recv_frame but on OPCODE_CLOSE:
    // parse close code from first 2 bytes of payload (BE uint16)
    // return WsFrame{opcode=0x08, payload=..., close_code=...}
    // Add close_code field to WsFrame struct
}
```

### Pattern 3: TCP Connect + Auth as Reusable Function
**What:** The feature test needs multiple authenticated connections (at least 2 for pub/sub, more for rate limit and SIGTERM tests). Extract the full connect+upgrade+auth flow into one helper.
**When to use:** Every test in the feature test binary.

```cpp
// Returns fd (ready for authenticated WS communication), or -1 on failure
static int connect_and_auth(const std::string& host, int port,
                            const identity::RelayIdentity& id) {
    // 1. socket() + connect()
    // 2. SO_RCVTIMEO(5s)
    // 3. WS upgrade handshake
    // 4. Auth challenge-response
    // Returns authenticated fd
}
```

### Pattern 4: Config File Rewrite for SIGHUP
**What:** Load relay.json, modify the `rate_limit_messages_per_sec` field, write it back. Use nlohmann/json for clean round-trip.
**When to use:** D-06, D-09 (SIGHUP rate limit change verification).

```cpp
static void rewrite_config(const std::string& path, const std::string& key, auto value) {
    std::ifstream in(path);
    auto j = nlohmann::json::parse(in);
    in.close();
    j[key] = value;
    std::ofstream out(path);
    out << j.dump(2);
}
```

### Anti-Patterns to Avoid
- **Threading for multi-client:** D-03 explicitly says sequential blocking, no threads. Two sockets in the same process, interleaved reads.
- **Shared state between tests:** Each test function should be self-contained. Only the SIGTERM test depends on the relay still running (must be last).
- **Tight timing for rate limiting:** Don't blast messages in a tight loop expecting exact rate behavior. The token bucket refills based on elapsed time. Send messages with minimal delay and count rejections -- 10+ consecutive rejections triggers disconnect. Send at least 20+ messages rapidly to ensure the bucket drains.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| JSON config editing | String manipulation / regex | nlohmann/json parse + dump | Round-trip preserves all fields, no format corruption |
| WebSocket framing | Raw byte manipulation | Existing ws_send_text / ws_recv_frame helpers | Already tested, handle masking and fragmentation |
| Auth challenge-response | Custom crypto flow | Existing pattern from smoke test (RelayIdentity::sign) | Proven correct in Phase 107 |
| Process exit verification | Polling /proc | waitpid(pid, WNOHANG) or kill(pid, 0) | Standard POSIX, race-free |

## Common Pitfalls

### Pitfall 1: Close Frame Swallowed by ws_recv_frame
**What goes wrong:** The existing `ws_recv_frame()` returns `nullopt` on `OPCODE_CLOSE` (line 209 of smoke_test). If used as-is for the SIGTERM test, the close frame will be invisible.
**Why it happens:** Smoke test doesn't need to inspect close frames -- it treats them as connection termination.
**How to avoid:** Create a `ws_recv_frame_raw()` variant or add a `bool include_close = false` parameter that preserves close frames. Parse the 2-byte BE status code from the close frame payload.
**Warning signs:** SIGTERM test passes but never validates the close code value.

### Pitfall 2: Rate Limit Token Bucket Timing
**What goes wrong:** Rate limiter uses `steady_clock` for refill timing. If messages are sent too slowly (e.g., with sleep between them), tokens refill and the bucket never drains.
**Why it happens:** Token bucket at rate=5 refills 5 tokens/second. Need to exhaust all tokens before they refill.
**How to avoid:** Send messages in a tight loop (no sleep). With rate=5, the bucket starts with 5 tokens. After 5 messages, all tokens consumed. Next 10 messages get rate-limited. After 10 consecutive rejections, relay disconnects with Close(4002). So ~15+ messages in rapid succession should work.
**Warning signs:** All messages succeed despite rate limit being set, or disconnect takes much longer than expected.

### Pitfall 3: Rate Limit Shared Atomic Propagation Delay
**What goes wrong:** After SIGHUP, the shared atomic `rate_limit_rate` is updated, but each session only checks it on the next incoming message (line 481-484 of ws_session.cpp). If you send one message right after SIGHUP, it may use the new rate -- but `set_rate()` resets tokens to full bucket.
**Why it happens:** `set_rate(new_rate)` resets `tokens_ = rate` and `consecutive_rejects_ = 0`. So the first message after rate change always succeeds (bucket is full).
**How to avoid:** After SIGHUP, send enough messages to drain the new bucket. With rate=5: first message triggers set_rate(5) which gives 5 tokens, so need 5+10=15 more messages to trigger disconnect.
**Warning signs:** First burst after SIGHUP doesn't trigger rate limiting despite low rate value.

### Pitfall 4: Notification Ordering with Two Sockets
**What goes wrong:** Client A subscribes, client B writes. Per D-04, notification arrives on A after B's write_ack. But if the read timeout on socket A is too short, the notification may be missed.
**Why it happens:** Relay fan-out is synchronous after ingest, but network scheduling and relay threading introduce microsecond-level non-determinism.
**How to avoid:** Use SO_RCVTIMEO of 5 seconds on both sockets (same as smoke test). Read the notification from socket A AFTER receiving write_ack on socket B.
**Warning signs:** Notification test passes locally but flakes in CI or on slow machines.

### Pitfall 5: SIGTERM Close Frame Race
**What goes wrong:** After `kill(pid, SIGTERM)`, the relay starts its shutdown sequence (5s drain, then Close(1001)). The test client needs to read the close frame. If the relay exits before the client reads, the read fails with connection reset.
**Why it happens:** The relay sends Close(1001) and then waits 2 seconds for echo, then does `ioc.stop()`. If the client is slow to read, the relay may force-close.
**How to avoid:** Read immediately after sending SIGTERM. Use SO_RCVTIMEO to handle the case where close frame never arrives. The 5s drain + 2s close handshake timeout gives the client 7 seconds to read.
**Warning signs:** Test works on fast machines but fails on loaded systems.

### Pitfall 6: SIGTERM Test Must Be Last
**What goes wrong:** Running SIGTERM test before other tests kills the relay. Subsequent tests fail with connection refused.
**Why it happens:** SIGTERM terminates the relay process.
**How to avoid:** Explicitly ensure SIGTERM test is the final test function called. Return from main after it.
**Warning signs:** Intermittent failures in tests that follow SIGTERM.

### Pitfall 7: PID Validation
**What goes wrong:** If `--relay-pid` is wrong or the relay has already exited, `kill(pid, SIGHUP)` sends signal to wrong process or fails silently.
**Why it happens:** PID reuse or typo.
**How to avoid:** Validate the PID with `kill(pid, 0)` before sending any signals. Return error if relay not running.
**Warning signs:** SIGHUP test passes but relay config didn't actually change.

## Code Examples

### Multi-Client Pub/Sub Test (E2E-02)
```cpp
// Source: relay_smoke_test.cpp patterns + CONTEXT.md D-03/D-04
void test_pubsub(const std::string& host, int port,
                 const identity::RelayIdentity& id) {
    // Client A: connect + auth + subscribe to own namespace
    int fd_a = connect_and_auth(host, port, id);
    auto ns_hex = util::to_hex(id.public_key_hash());
    json sub = {{"type", "subscribe"}, {"request_id", 1},
                {"namespaces", json::array({ns_hex})}};
    ws_send_text(fd_a, sub.dump());

    // Client B: connect + auth + write blob
    int fd_b = connect_and_auth(host, port, id);
    auto data_msg = make_data_message(id, 100, test_data, 3600, time(nullptr));
    ws_send_text(fd_b, data_msg.dump());
    auto ack = ws_recv_text(fd_b);  // write_ack

    // Read notification from client A
    auto notif = ws_recv_frame(fd_a);  // Should be notification type=21
    // Validate: type=="notification", namespace matches, hash matches ack

    // Cleanup: unsubscribe + close
    close(fd_a); close(fd_b);
}
```

### Rate Limit + SIGHUP Combined Test (E2E-03 + E2E-04)
```cpp
// Source: relay_main.cpp SIGHUP handler + ws_session.cpp rate limit path
void test_sighup_rate_limit(const std::string& host, int port,
                            const identity::RelayIdentity& id,
                            pid_t relay_pid,
                            const std::string& config_path) {
    int fd = connect_and_auth(host, port, id);

    // Phase 1: blast with rate=0 (disabled), all should succeed
    for (int i = 0; i < 10; ++i) {
        json msg = {{"type", "node_info_request"}, {"request_id", 1000 + i}};
        ws_send_text(fd, msg.dump());
        ws_recv_text(fd);  // consume response
    }
    record("sighup_rate_limit_phase1", true, "10 msgs with rate=0");

    // Phase 2: modify config, send SIGHUP
    rewrite_config(config_path, "rate_limit_messages_per_sec", 5);
    kill(relay_pid, SIGHUP);
    usleep(100000);  // 100ms for SIGHUP processing

    // Phase 3: blast again -- first message triggers set_rate(5) with 5 tokens
    // Messages 1-5: consume tokens (succeed)
    // Messages 6-15: rate limited (10 consecutive rejections -> disconnect)
    bool disconnected = false;
    int sent = 0;
    for (int i = 0; i < 25; ++i) {
        json msg = {{"type", "node_info_request"}, {"request_id", 2000 + i}};
        if (!ws_send_text(fd, msg.dump())) { disconnected = true; break; }
        auto resp = ws_recv_frame_raw(fd);
        if (!resp) { disconnected = true; break; }
        if (resp->opcode == 0x08) {
            // Close frame -- check code 4002
            disconnected = true;
            break;
        }
        ++sent;
    }
    record("sighup_rate_limit_disconnect", disconnected, ...);

    // Restore config for subsequent tests
    rewrite_config(config_path, "rate_limit_messages_per_sec", 0);
    kill(relay_pid, SIGHUP);
    usleep(100000);
    close(fd);
}
```

### SIGTERM Close Frame Verification (E2E-05)
```cpp
// Source: relay_main.cpp SIGTERM handler (lines 280-311)
void test_sigterm(const std::string& host, int port,
                  const identity::RelayIdentity& id,
                  pid_t relay_pid) {
    int fd = connect_and_auth(host, port, id);

    // Send SIGTERM
    kill(relay_pid, SIGTERM);

    // Read close frame (relay sends Close(1001) after 5s drain)
    // Use longer timeout since relay has 5s drain period
    struct timeval tv{};
    tv.tv_sec = 10;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto frame = ws_recv_frame_raw(fd);
    bool got_close = frame && frame->opcode == 0x08;
    uint16_t close_code = 0;
    if (got_close && frame->payload.size() >= 2) {
        close_code = (static_cast<uint16_t>(frame->payload[0]) << 8)
                   | static_cast<uint8_t>(frame->payload[1]);
    }
    record("sigterm_close_frame", got_close && close_code == 1001, ...);

    // Verify relay process exits
    for (int i = 0; i < 20; ++i) {
        if (kill(relay_pid, 0) != 0) { /* exited */ break; }
        usleep(500000);  // 500ms
    }
    bool exited = (kill(relay_pid, 0) != 0);
    record("sigterm_process_exit", exited, ...);

    close(fd);
}
```

### Config File Modification
```cpp
// Source: nlohmann/json + relay_config.cpp load pattern
static void rewrite_config(const std::string& path,
                           const std::string& key, auto value) {
    std::ifstream in(path);
    auto j = nlohmann::json::parse(in);
    in.close();
    j[key] = value;
    std::ofstream out(path);
    out << j.dump(2);
    out.close();
}
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Standalone C++ binary (not Catch2 -- same as relay_smoke_test) |
| Config file | `tools/CMakeLists.txt` (add relay_feature_test target) |
| Quick run command | `./build/tools/relay_feature_test --identity /tmp/chromatindb-test/keys/identity.key --relay-pid $RELAY_PID --config /tmp/chromatindb-test/relay.json` |
| Full suite command | `/tmp/chromatindb-test/run-smoke.sh` (runs smoke test + feature test) |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| E2E-02 | Pub/sub notification fan-out with live writes | E2E (multi-client) | `./build/tools/relay_feature_test --identity ... --relay-pid ... --config ...` | No -- Wave 0 |
| E2E-03 | Rate limiting disconnects after sustained violation | E2E (signal + rate test) | Same binary | No -- Wave 0 |
| E2E-04 | SIGHUP reloads TLS, ACL, rate limit, metrics_bind | E2E (signal + behavioral verification) | Same binary | No -- Wave 0 |
| E2E-05 | SIGTERM sends close frames before exit | E2E (signal + close frame capture) | Same binary | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** Build relay_feature_test and run individually with live relay
- **Per wave merge:** Full `run-smoke.sh` run (smoke test + feature test)
- **Phase gate:** All tests pass in run-smoke.sh, ASAN clean

### Wave 0 Gaps
- [ ] `tools/relay_feature_test.cpp` -- new binary covering E2E-02 through E2E-05
- [ ] `tools/relay_test_helpers.h` -- shared helpers (Claude's discretion: shared header vs copy)
- [ ] `tools/CMakeLists.txt` update -- add relay_feature_test target
- [ ] `run-smoke.sh` update -- add relay_feature_test invocation with --relay-pid

## Key Technical Details

### Rate Limiter Behavior (Verified from Source)
- Token bucket: `rate_ = burst_` (no separate burst config)
- `rate = 0` means disabled (`try_consume()` always returns `true`)
- `set_rate(new_rate)` resets tokens to full bucket AND resets consecutive_rejects to 0
- Each session checks shared atomic on every message; if rate changed, calls `set_rate()`
- After 10 consecutive rejections (`RATE_LIMIT_DISCONNECT_THRESHOLD`), calls `close(4002, "rate limit exceeded")`
- Rate limited messages still get an error JSON response before the connection close

### SIGHUP Handler (Verified from relay_main.cpp:316-359)
The handler reloads in this exact order:
1. `load_relay_config(config_path)` -- re-reads JSON file
2. TLS: `acceptor.reload_tls(cert_path, key_path)` -- if tls_enabled()
3. ACL: `authenticator.reload_allowed_keys(new_acl)` -- rebuilds key set
4. max_connections: `acceptor.set_max_connections(new_cfg.max_connections)`
5. Rate limit: `rate_limit_rate.store(new_value)` -- shared atomic
6. Metrics bind: `metrics_collector.set_metrics_bind(new_bind)`

On failure: catches exception, logs error, keeps previous config. Does NOT crash.

### SIGTERM Handler (Verified from relay_main.cpp:280-311)
Shutdown sequence:
1. `stopping.store(true)` -- atomic flag
2. `acceptor.stop()` -- stops accepting new connections
3. `metrics_collector.stop()` -- stops metrics endpoint
4. 5-second drain timer expires
5. `session_manager.for_each` -> `session->close(1001, "server shutting down")`
6. 2-second close handshake timer expires
7. `ioc.stop()` -- kills the event loop

The `close(1001, ...)` method sends an encoded close frame via `send_raw()` (bypasses send queue), then waits 5 seconds for client echo before shutting down socket.

### Notification JSON Format (Type 21)
From json_schema.h NOTIFICATION_FIELDS:
```json
{
    "type": "notification",
    "namespace": "<64-char hex>",
    "hash": "<64-char hex>",
    "seq_num": "<uint64 string>",
    "size": <uint32 number>,
    "is_tombstone": <bool>
}
```
Notification payload from node is 77 bytes: namespace(32) + hash(32) + seq_num_be(8) + size_be(4) + tombstone(1).

### Close Frame Wire Format
From ws_frame.cpp `encode_close_frame()`:
- Opcode 0x08 (FIN=1)
- Payload: 2-byte BE status code + optional UTF-8 reason string
- Server frames are NOT masked (only client->server frames are masked per RFC 6455)

### Config File Location
From run-smoke.sh: `/tmp/chromatindb-test/relay.json`
Current contents include `"rate_limit_messages_per_sec": 0` which is exactly what D-08 requires.

### relay_feature_test CLI Interface
Per D-05:
```
relay_feature_test --identity <key_path>
                   --relay-pid <PID>
                   --config <relay_config_path>
                   [--host <addr>] [--port <port>]
```
The `--config` argument is needed for SIGHUP test (to know which file to modify).

## Open Questions

1. **Client B notification echo**
   - What we know: When client B writes a blob to its own namespace AND is subscribed to that namespace, it also receives a notification (no source exclusion yet -- that's FEAT-01 in Phase 109).
   - What's unclear: For the pub/sub test, both clients use the same identity. Client B is NOT subscribed, so it won't get a notification. Client A IS subscribed, so it gets exactly one notification.
   - Recommendation: This is fine as designed. Client B never subscribes, only client A does.

2. **TLS reload verification in test**
   - What we know: D-06 specifies SIGHUP test via rate limit change. E2E-04 requires TLS, ACL, rate limit, and metrics_bind reload.
   - What's unclear: The test environment uses plain WS (no TLS -- relay.json has no cert_path/key_path). ACL is also not configured. Testing ALL SIGHUP paths would require TLS setup.
   - Recommendation: Test rate_limit change as the observable verification of SIGHUP working. TLS/ACL/metrics_bind reload share the same code path (config re-read + handler call). The unit tests in relay/ cover those individual reload methods. The E2E test proves the SIGHUP signal triggers the reload path, and rate limit is the behaviorally verifiable one.

## Sources

### Primary (HIGH confidence)
- `relay/relay_main.cpp` -- SIGHUP handler (lines 316-359), SIGTERM handler (lines 280-311)
- `relay/ws/ws_session.cpp` -- rate limit enforcement (lines 479-498), close() method (lines 713-744)
- `relay/ws/ws_session.h` -- RATE_LIMIT_DISCONNECT_THRESHOLD = 10
- `relay/core/rate_limiter.h` -- token bucket implementation (set_rate resets tokens to full)
- `relay/core/subscription_tracker.h` -- subscribe/get_subscribers API
- `relay/core/uds_multiplexer.cpp` -- notification fan-out (route_response, handle_notification)
- `relay/translate/json_schema.h` -- NOTIFICATION_FIELDS (namespace, hash, seq_num, size, is_tombstone)
- `relay/ws/ws_frame.h` -- OPCODE_CLOSE = 0x08, encode_close_frame()
- `relay/config/relay_config.h` -- RelayConfig struct with all SIGHUP-reloadable fields
- `tools/relay_smoke_test.cpp` -- reusable helper patterns (986 lines)
- `/tmp/chromatindb-test/run-smoke.sh` -- current test runner script
- `/tmp/chromatindb-test/relay.json` -- test config file format

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries already in use, no new dependencies
- Architecture: HIGH -- pattern directly extends existing relay_smoke_test with well-understood relay behaviors
- Pitfalls: HIGH -- all identified from reading actual source code (rate limiter timing, close frame swallowing, SIGTERM sequencing)

**Research date:** 2026-04-11
**Valid until:** 2026-05-11 (stable -- relay implementation is frozen for this milestone)
