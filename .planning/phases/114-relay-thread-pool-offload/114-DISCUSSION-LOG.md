# Phase 114: Relay Thread Pool Offload - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-14
**Phase:** 114-relay-thread-pool-offload
**Areas discussed:** Offload scope, Offload granularity, Threshold behavior, UDS AEAD handling

---

## Offload Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Full translation pipeline | Offload json_to_binary() and binary_to_json() entirely -- covers JSON parse, base64, FlatBuffer, hex decode all in one offload call. Simplest boundary. | ✓ |
| Translation + AEAD | Same as above PLUS offload AEAD encrypt/decrypt for UDS frames. Covers all CPU work in the request path. | |
| Only base64 + FlatBuffer | Keep JSON parse/serialize inline. Only offload base64_encode/decode and FlatBuffer build. More surgical. | |

**User's choice:** Full translation pipeline
**Notes:** AEAD offload was decided separately in the UDS AEAD area. The combined effect is Translation + AEAD, but decided in two steps.

---

## Offload Granularity

| Option | Description | Selected |
|--------|-------------|----------|
| Wrap each translate call | Each json_to_binary() and binary_to_json() call gets its own co_await offload(). Simple pattern: offload -> transfer back -> continue. Works identically in HTTP handlers and UDS notification path. | ✓ |
| Wrap entire request handler | Offload the full request processing (translate + UDS send + await + translate response) as one block. Fewer thread hops but holds a pool thread during the UDS await (bad for pool utilization). | |

**User's choice:** Wrap each translate call
**Notes:** None

---

## Threshold Behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Always offload | Every json_to_binary/binary_to_json goes through the pool regardless of size. Simpler code, predictable behavior. | |
| Size threshold (Recommended) | Only offload when the input exceeds a threshold (e.g., body size > 64KB). Small queries stay inline. | ✓ |
| Type-based threshold | Offload only for types known to carry large payloads: Data(8), Delete(17), ReadResponse(32), BatchReadResponse(54). | |

**User's choice:** Size threshold (Recommended)

### Follow-up: Threshold Value

| Option | Description | Selected |
|--------|-------------|----------|
| 64 KB | Catches large blobs and batch responses. Small queries stay inline. Good balance. | ✓ |
| 4 KB | More aggressive -- offloads medium payloads too. Lower risk of event loop stalls but more thread hops. | |
| You decide | Let Claude pick a reasonable default. | |

**User's choice:** 64 KB
**Notes:** None

---

## UDS AEAD Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Leave AEAD inline | Keep send_encrypted/recv_encrypted as-is. UDS traffic already serialized. AEAD for typical payloads is sub-microsecond. | |
| Offload AEAD too | Wrap aead_encrypt/decrypt in offload() calls. Safe because send/recv already serialized. Prevents stalls for large blob AEAD. | ✓ |
| You decide | Let Claude assess whether UDS AEAD is a bottleneck. | |

**User's choice:** Offload AEAD too

### Follow-up: AEAD Threshold

| Option | Description | Selected |
|--------|-------------|----------|
| Same 64 KB threshold | Only offload AEAD when plaintext > 64KB. Consistent with translation threshold. | ✓ |
| Always offload AEAD | Every AEAD call goes through the pool. Simpler code but unnecessary for small payloads. | |

**User's choice:** Same 64 KB threshold
**Notes:** Safe because send_encrypted called from send queue drain (serialized), recv_encrypted called from single read_loop coroutine (serialized). Counter incremented before offload.

---

## Claude's Discretion

- Helper wrapper vs inline if/else at each call site
- How to thread asio::thread_pool& reference to all call sites
- Plan decomposition
- Test strategy

## Deferred Ideas

None
