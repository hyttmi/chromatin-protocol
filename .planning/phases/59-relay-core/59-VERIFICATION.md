---
phase: 59-relay-core
verified: 2026-03-23T16:00:00Z
status: human_needed
score: 3/4 success criteria verified automatically
re_verification: false
human_verification:
  - test: "Run a chromatindb node, start chromatindb_relay pointed at its UDS socket, then connect a client through the relay and perform write/read/list/stats operations"
    expected: "Client writes a blob, receives WriteAck; reads it back via ReadRequest/ReadResponse; lists it via ListRequest/ListResponse; queries namespace stats via StatsRequest/StatsResponse -- all successfully forwarded through the relay"
    why_human: "Success criterion 4 is an end-to-end integration test requiring a live node and client. The code path is fully wired (relay_main -> RelaySession -> Connection::create_uds_outbound -> TrustedHello -> bidirectional forwarding) but the actual message round-trip through the relay cannot be verified by static analysis alone."
---

# Phase 59: Relay Core Verification Report

**Phase Goal:** A client can connect to the relay via PQ-encrypted channel and interact with the node -- the relay authenticates, filters, and forwards
**Verified:** 2026-03-23T16:00:00Z
**Status:** human_needed
**Re-verification:** No -- initial verification

## Goal Achievement

### Observable Truths (from ROADMAP.md Success Criteria)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Relay accepts TCP connections and completes full PQ handshake as responder using its own identity keypair | VERIFIED | `relay_main.cpp:134` calls `identity.to_node_identity()`; `relay_main.cpp:177-178` calls `Connection::create_inbound(std::move(socket), node_identity)` -- inbound connections run the full ML-KEM-1024 + ML-DSA-87 responder handshake via the existing Connection class |
| 2 | For each authenticated client, relay opens a dedicated UDS connection to the node using TrustedHello -- one UDS per client, torn down on disconnect | VERIFIED | `relay_main.cpp:191-192` calls `RelaySession::create(c, cfg.uds_path, node_identity, ioc)` inside `on_ready` (post-handshake); `relay_session.cpp:59` calls `Connection::create_uds_outbound` (TrustedHello initiator); `relay_session.cpp:163-176` teardown closes both client and node connections |
| 3 | Relay forwards allowed message types bidirectionally without inspecting payloads; 16 allowed, peer-only and unknown types default-denied | VERIFIED | `message_filter.cpp:8-34` switch statement with 16 explicit allowed cases, default returns false; `relay_session.cpp:113` calls `is_client_allowed(type)`; node-to-client forwarding at `relay_session.cpp:132-144` (no filter, as node only sends client-understood types); 8 test cases / 49 assertions all pass |
| 4 | Full client workflow (write/read/list/stats) works end-to-end through the relay | NEEDS HUMAN | Code path is fully wired but end-to-end round-trip requires live node + client |

**Score:** 3/4 success criteria verified automatically

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `relay/core/message_filter.h` | `is_client_allowed` filter function | VERIFIED | Present; contains both `is_client_allowed` and `type_name` declarations in correct namespace |
| `relay/core/message_filter.cpp` | Switch with all 16 allowed types, default-deny | VERIFIED | 16 `TransportMsgType_*` cases return true; default returns false; `type_name` uses `EnumNameTransportMsgType` with fallback |
| `relay/core/relay_session.h` | `class RelaySession` with client+node pair | VERIFIED | `class RelaySession : public std::enable_shared_from_this<RelaySession>`; `static Ptr create()`; `asio::awaitable<bool> start()`; `void stop()` |
| `relay/core/relay_session.cpp` | Bidirectional forwarding coroutines | VERIFIED | `handle_client_message` (filtered, co_spawn forward to node); `handle_node_message` (unfiltered, co_spawn forward to client); `teardown` closes both connections |
| `db/identity/identity.h` | `static NodeIdentity from_keys` factory | VERIFIED | Line 29-30: `static NodeIdentity from_keys(std::span<const uint8_t> pubkey, std::span<const uint8_t> seckey);` |
| `relay/identity/relay_identity.h` | `to_node_identity()` method | VERIFIED | Line 47: `chromatindb::identity::NodeIdentity to_node_identity() const;` |
| `relay/relay_main.cpp` | Full accept loop with signal handling | VERIFIED | `accept_loop` lambda coroutine; `asio::signal_set signals(ioc, SIGINT, SIGTERM)`; `ioc.run()` |
| `db/tests/relay/test_message_filter.cpp` | Unit tests for message filter | VERIFIED | 8 `TEST_CASE` blocks; 49 assertions; all pass |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `relay/identity/relay_identity.cpp` | `db/identity/identity.h` | `NodeIdentity::from_keys()` | WIRED | `relay_identity.cpp:116` calls `NodeIdentity::from_keys(signer_.export_public_key(), signer_.export_secret_key())` |
| `relay/core/relay_session.cpp` | `relay/core/message_filter.h` | `is_client_allowed()` call in forwarding | WIRED | `relay_session.cpp:113`: `if (!is_client_allowed(type))` gates client-to-node forwarding |
| `relay/core/relay_session.cpp` | `db/net/connection.h` | `Connection::send_message` and message callbacks | WIRED | `relay_session.cpp:59` uses `Connection::create_uds_outbound`; `relay_session.cpp:127-129` and `142-144` use `conn->send_message` via `co_spawn` |
| `relay/relay_main.cpp` | `relay/core/relay_session.h` | `RelaySession::create()` in `on_ready` | WIRED | `relay_main.cpp:191`: `RelaySession::create(c, cfg.uds_path, node_identity, ioc)` inside `conn->on_ready` callback |
| `relay/relay_main.cpp` | `relay/identity/relay_identity.h` | `to_node_identity()` for Connection compatibility | WIRED | `relay_main.cpp:134`: `auto node_identity = identity.to_node_identity()` |
| `relay/relay_main.cpp` | `db/net/connection.h` | `Connection::create_inbound` for TCP clients | WIRED | `relay_main.cpp:177-178`: `Connection::create_inbound(std::move(socket), node_identity)` |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| RELAY-01 | 59-01, 59-02 | Relay accepts TCP connections and performs PQ handshake as responder (ML-KEM-1024 + ML-DSA-87), using its own identity keypair | SATISFIED | `relay_main.cpp:134,177-178`: `to_node_identity()` feeds `Connection::create_inbound`; inbound Connection runs full PQ responder protocol |
| RELAY-02 | 59-01, 59-02 | Relay connects to chromatindb node via UDS with TrustedHello, one UDS connection per client session | SATISFIED | `relay_session.cpp:47-60`: UDS socket connected, `create_uds_outbound` (TrustedHello initiator); one session per `on_ready` call; teardown closes UDS on disconnect |
| RELAY-03 | 59-01, 59-02 | Relay filters messages by type; 16 allowed, peer-only blocked, default-deny | SATISFIED | `message_filter.cpp` switch with all 16 cases verified; `relay_session.cpp:113-119` applies filter and disconnects on blocked types; 49 test assertions validate exact type classification |
| RELAY-04 | 59-01, 59-02 | Relay forwards allowed messages bidirectionally without parsing payloads (type field only) | SATISFIED | `relay_session.cpp:122-129` (client->node) and `relay_session.cpp:138-144` (node->client): payload forwarded verbatim via `send_message(t, p)`; no payload parsing |

No orphaned requirements. RELAY-05 through RELAY-08 belong to Phase 58 and are not in scope for Phase 59.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| None found | - | - | - | - |

Scanned `relay/core/message_filter.cpp`, `relay/core/relay_session.cpp`, `relay/relay_main.cpp`, `relay/identity/relay_identity.cpp`, `db/identity/identity.cpp` for TODO/FIXME/placeholder, empty returns, and hardcoded stub patterns. None found.

### Build and Test Verification

| Check | Result |
|-------|--------|
| `cmake --build .` | PASS -- entire project compiles including relay with all new code |
| `./chromatindb_tests "[message_filter]"` | PASS -- 8 test cases, 49 assertions, all passed |
| `./chromatindb_relay version` | PASS -- outputs `chromatindb_relay 1.1.0` |
| `./chromatindb_relay run --config /nonexistent` | PASS -- exits 1 with "Config error: Config file not found" (existing behavior preserved) |
| Commit c55ab86 (test: failing tests for message filter) | VERIFIED in git log |
| Commit 0c55d00 (feat: implement identity adapter and message filter) | VERIFIED in git log |
| Commit 0585e1f (feat: RelaySession with bidirectional forwarding) | VERIFIED in git log |
| Commit fbec47a (feat: wire relay event loop) | VERIFIED in git log |

### Human Verification Required

#### 1. End-to-End Client Workflow Through Relay

**Test:** Start a chromatindb node with a UDS socket. Configure and start `chromatindb_relay` pointing at that socket. Connect a PQ-authenticated client to the relay's TCP port and perform:
1. Write a blob (Data message) -- expect WriteAck back
2. Read the blob by hash (ReadRequest) -- expect ReadResponse with the blob
3. List the namespace (ListRequest) -- expect ListResponse with one entry
4. Query namespace stats (StatsRequest) -- expect StatsResponse with blob count = 1

**Expected:** All four operations complete successfully, responses arrive at the client, relay logs show "session active" and "client disconnected" (not "blocked message type").

**Why human:** This is an integration test requiring a running node, a relay binary, and a client sending real PQ-encrypted messages. The code path is fully wired and compiles, but the actual message routing through the live relay cannot be validated by static analysis or unit tests.

### Gaps Summary

No gaps found. All three automatically verifiable success criteria are fully implemented and wired. Success criterion 4 (end-to-end client workflow) requires human/integration testing and is not a gap in the code -- it is a verification gap only.

The relay binary is architecturally complete:
- PQ handshake responder path: `Connection::create_inbound` with relay's `NodeIdentity` (adapted via `to_node_identity()`)
- Per-client UDS session: `RelaySession::create` in `on_ready`, `Connection::create_uds_outbound` for TrustedHello
- Message filter: switch-based allow-list with default-deny, 16 types allowed, applied in `handle_client_message`
- Bidirectional forwarding: `co_spawn` pattern for async `send_message` in synchronous `on_message` callbacks
- Graceful shutdown: `asio::signal_set` handler sets `draining = true`, closes acceptor, calls `stop()` on all sessions

---

_Verified: 2026-03-23T16:00:00Z_
_Verifier: Claude (gsd-verifier)_
