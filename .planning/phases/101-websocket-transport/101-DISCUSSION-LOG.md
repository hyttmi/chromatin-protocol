# Phase 101: WebSocket Transport - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-09
**Phase:** 101-websocket-transport
**Areas discussed:** WS/WSS mode config, Session/WebSocket integration, Client frame fragmentation, Pre-auth timeouts, Max message size, Server-initiated ping, Binary + text frames, Close frame behavior, Accept loop + session map, New config fields, Test strategy, Malformed frame handling, Plain WS safety, IPv4 vs IPv6, Max connections, WS extension negotiation, Thread model, TLS version policy, SIGHUP cert reload failure, Session lifecycle stages, Non-upgrade HTTP requests, Message ordering in thread pool, SIGTERM with active sessions, TLS library choice

---

## WS/WSS Mode Config

| Option | Description | Selected |
|--------|-------------|----------|
| Cert presence auto-detect | cert_path/key_path set -> WSS, omitted -> WS | |
| Explicit mode field | "tls": true/false config field | |
| Dual listeners | Separate bind_port_ws and bind_port_wss | |

**User's choice:** Cert presence auto-detect, but if certs are set and can't load -> error and exit (not fallback)
**Notes:** User initially wanted fallback to WS on bad certs, then chose error-exit when presented as security concern.

---

## Cert Load Failure

| Option | Description | Selected |
|--------|-------------|----------|
| Warn + fall back to WS | Log warning, run WS mode | |
| Error and exit | Refuse to start | ✓ |

**User's choice:** Error and exit
**Notes:** If operator set cert paths, they intended WSS. Silent fallback is a security surprise.

---

## Session / WebSocket Integration

| Option | Description | Selected |
|--------|-------------|----------|
| WsSession wraps Session | WsSession in ws/ owns stream, Session in core/ handles queue | ✓ |
| Session evolves directly | Session gains WS stream member | |
| Template Session on stream | Session<StreamType> with specialized do_send() | |

**User's choice:** WsSession wraps Session
**Notes:** Clean separation -- ws/ handles framing, core/ handles queuing.

---

## Client Frame Fragmentation

| Option | Description | Selected |
|--------|-------------|----------|
| Accept + reassemble | RFC 6455 SS5.4 compliant, ~20 extra lines | ✓ |
| Reject with close frame | Close(1003 Unsupported) on continuation | |
| You decide | Claude picks | |

**User's choice:** Accept + reassemble

---

## Upgrade Handshake Timeout

| Option | Description | Selected |
|--------|-------------|----------|
| 5 seconds | Tight, prevents slowloris | ✓ |
| 10 seconds | Same as auth timeout | |
| You decide | Claude picks | |

**User's choice:** 5 seconds

---

## Post-Upgrade Idle Timeout

| Option | Description | Selected |
|--------|-------------|----------|
| 30s idle timeout | Prevents connection hoarding | ✓ |
| No timeout until Phase 102 | Accept temporary exposure | |
| You decide | Claude picks | |

**User's choice:** 30s idle timeout

---

## Max Message Size

| Option | Description | Selected |
|--------|-------------|----------|
| 128 MiB | Above 100 MiB blob + base64 overhead | |
| 16 MiB initial, raise later | Conservative for Phase 101 | |
| 150 MiB | Comfortable margin for JSON envelope | |
| Match at transport, let node reject | 256 MiB generous limit | |
| 1 MiB text / 110 MiB binary | Hybrid approach | ✓ |

**User's choice:** 1 MiB text / 110 MiB binary (hybrid framing)
**Notes:** User questioned why blobs would go through JSON at all. Led to clarification that PROT-04 already specifies binary WS frames for large payloads. Hybrid approach: JSON text for control, binary for blobs.

---

## Server-Initiated Ping

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, 30s interval | Matches node keepalive pattern | ✓ |
| Pong-only | Only respond to client pings | |
| You decide | Claude picks | |

**User's choice:** 30s ping interval, 60s pong deadline

---

## Close Frame Behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Standard RFC compliance | Echo client Close, wait for echo on relay-initiated Close | ✓ |
| Fire and forget | Send Close, immediately TCP close | |
| You decide | Claude picks | |

**User's choice:** Standard RFC compliance

---

## Accept Loop + Session Map

| Option | Description | Selected |
|--------|-------------|----------|
| Acceptor owns session map | WsAcceptor has session map | |
| Separate SessionManager | Decoupled class for session lifecycle | ✓ |

**User's choice:** Separate SessionManager
**Notes:** Needed by Phase 104 for notification fan-out.

---

## Config Fields

| Option | Description | Selected |
|--------|-------------|----------|
| Minimal config | cert_path + key_path only, rest as constants | ✓ |
| Full config | All transport settings configurable | |
| You decide | Claude picks | |

**User's choice:** Minimal config -- "needs to be simple as fuck"

---

## Test Strategy

**User's choice:** Unit tests for framing logic + live testing against real node on dev machine
**Notes:** User will launch the node on this PC for live relay testing. No Docker integration tests for Phase 101.

---

## Malformed Frame Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Close + status code | Close(1002 Protocol Error), warn log | ✓ |
| Silent disconnect | Drop TCP, no ceremony | |
| You decide | Claude picks | |

**User's choice:** Close + status code

---

## Plain WS Safety

| Option | Description | Selected |
|--------|-------------|----------|
| Startup warning only | Log warning encouraging TLS | ✓ |
| Localhost-only in WS mode | Force 127.0.0.1 bind | |
| You decide | Claude picks | |

**User's choice:** Startup warning
**Notes:** "data is encrypted, i think it's good practice also use TLS when it's possible"

---

## IPv4 vs IPv6

| Option | Description | Selected |
|--------|-------------|----------|
| IPv4 only, default 0.0.0.0 | Simple, same as node | |
| Dual-stack (:: default) | Accept both IPv4 and IPv6 | ✓ |
| You decide | Claude picks | |

**User's choice:** Support both IPv4 and IPv6, default 0.0.0.0

---

## Max Connections (Early Limit)

| Option | Description | Selected |
|--------|-------------|----------|
| Hardcoded 1024 | Safety limit, configurable in Phase 102 | ✓ |
| No limit until Phase 102 | No session management logic | |
| You decide | Claude picks | |

**User's choice:** Hardcoded 1024

---

## WS Extension Negotiation

| Option | Description | Selected |
|--------|-------------|----------|
| Ignore silently | Omit from 101 response | ✓ |
| Reject explicitly | Error in upgrade response | |
| You decide | Claude picks | |

**User's choice:** Ignore silently

---

## Thread Model

| Option | Description | Selected |
|--------|-------------|----------|
| Single io_context thread | No locking, scale later | |
| Thread pool from start | hardware_concurrency() threads | ✓ |
| You decide | Claude picks | |

**User's choice:** Thread pool, hardware_concurrency() threads
**Notes:** User leaned toward thread pool model from the start.

---

## TLS Version Policy

| Option | Description | Selected |
|--------|-------------|----------|
| TLS 1.3 only | Modern, ephemeral keys | ✓ |
| TLS 1.2+ | Broader compatibility | |
| You decide | Claude picks | |

**User's choice:** TLS 1.3 only

---

## SIGHUP Cert Reload Failure

| Option | Description | Selected |
|--------|-------------|----------|
| Keep old context + warn | Same as node pattern | ✓ |
| Refuse new connections | Stop accepting until fixed | |
| You decide | Claude picks | |

**User's choice:** Keep old context + warn

---

## Session Lifecycle Stages

| Option | Description | Selected |
|--------|-------------|----------|
| After WS upgrade | Only fully upgraded sessions tracked | ✓ |
| After TLS handshake | Track TLS-connected clients | |
| After TCP accept | Track everything | |

**User's choice:** After WS upgrade

---

## Non-Upgrade HTTP Requests

| Option | Description | Selected |
|--------|-------------|----------|
| 426 Upgrade Required | Tells client what's needed | ✓ |
| 400 Bad Request + close | Generic rejection | |
| You decide | Claude picks | |

**User's choice:** 426 Upgrade Required

---

## Message Ordering in Thread Pool

| Option | Description | Selected |
|--------|-------------|----------|
| Per-connection strand | Natural Asio coroutine chain | ✓ |
| Explicit strand | asio::strand<> wrapper | |
| You decide | Claude picks | |

**User's choice:** Per-connection strand (implicit via coroutine chain)

---

## SIGTERM with Active Sessions

| Option | Description | Selected |
|--------|-------------|----------|
| ioc.stop() drops everything | Same as current, Phase 105 does it right | |
| Send Close(1001 Going Away) first | Best-effort before stop | ✓ |
| You decide | Claude picks | |

**User's choice:** Best-effort Close(1001) -- "doesn't matter if 105 is doing it properly"

---

## SIGHUP TLS Swap Mechanism

| Option | Description | Selected |
|--------|-------------|----------|
| shared_ptr swap | Atomic swap, ref-counted old context | ✓ |
| Recreate acceptor | Destroy + recreate, brief accept pause | |
| You decide | Claude picks | |

**User's choice:** shared_ptr swap

---

## Main Integration

| Option | Description | Selected |
|--------|-------------|----------|
| Construct in main, co_spawn | Flat pattern, same as node | ✓ |
| Relay class wraps everything | Encapsulated but extra class | |

**User's choice:** Construct in main, co_spawn

---

## Read-Side Backpressure

| Option | Description | Selected |
|--------|-------------|----------|
| Keep reading, rely on send queue | TCP flow control throttles naturally | ✓ |
| Pause WS reads on UDS pressure | Explicit flow control | |
| You decide | Claude picks | |

**User's choice:** Keep reading, rely on send queue

---

## TLS Library Choice

| Option | Description | Selected |
|--------|-------------|----------|
| OpenSSL 3.x (system) | Apache 2.0, universal, native Asio SSL | ✓ |
| wolfSSL | GPLv2 -- blocks closed-source | |
| LibreSSL | ISC license, API-compatible, lighter | |

**User's choice:** OpenSSL -- considered wolfSSL but GPLv2 blocks closed-source relay. Pin minimum version 3.0.
**Notes:** User wanted smaller library initially, accepted OpenSSL after licensing discussion.

---

## Claude's Discretion

- Internal class APIs (WsAcceptor, SessionManager, WsSession interfaces)
- Close status code selection for specific error scenarios
- Exact constexpr values for non-decided constants
- Whether SIGTERM Close(1001) is worth the complexity in Phase 101

## Deferred Ideas

None -- discussion stayed within phase scope.
