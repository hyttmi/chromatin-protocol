# Phase 110: Performance Benchmarking - Research

**Researched:** 2026-04-11
**Domain:** Python async WebSocket benchmarking, ML-DSA-87 auth, relay performance measurement
**Confidence:** HIGH

## Summary

Phase 110 builds a standalone Python benchmark tool (`tools/relay_benchmark.py`) that measures relay performance across 4 workload types. The tool connects to a running relay via WebSocket, authenticates with ML-DSA-87 challenge-response, and measures throughput/latency for various operation patterns.

Key findings: (1) All required Python dependencies (`liboqs-python`, `PyNaCl`, `flatbuffers`) are available on the dev machine except `websockets`, which must be pip-installed. (2) The relay's 1 MiB MAX_TEXT_MESSAGE_SIZE limit on incoming WebSocket text frames caps the maximum blob data size to approximately 750 KB through the relay -- PERF-03's 10/50/100 MiB test sizes are physically impossible through the relay's current JSON text frame protocol. The benchmark should test at the relay's actual limits. (3) For PERF-02 (relay vs UDS comparison), implementing a direct UDS client in Python is feasible but heavyweight -- requires TrustedHello handshake, HKDF-SHA256 key derivation, ChaCha20-Poly1305 AEAD, and FlatBuffers transport encoding. An alternative approach measures relay-only round-trip times and reports absolute latencies without a UDS baseline.

**Primary recommendation:** Build the benchmark tool with `websockets` + `asyncio` + `liboqs-python`, cap PERF-03 blob sizes at the relay's actual text frame limit (~750 KB), and for PERF-02 implement a minimal UDS direct client using the available Python crypto stack (PyNaCl + oqs + flatbuffers).

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Python benchmark tool at `tools/relay_benchmark.py`. Uses `websockets` library + `asyncio` for concurrent client simulation. No PQ crypto needed -- connects through the relay's WebSocket JSON interface (relay handles auth via ML-DSA-87 challenge-response).
- D-02: Dependencies: `websockets`, `liboqs-python` (for ML-DSA-87 challenge-response auth signing). No other deps. Not pip-installable -- standalone script.
- D-03: Identity: benchmark tool needs an ML-DSA-87 keypair for relay auth. Generate ephemeral keypair at startup or load from a test key file.
- D-04: Local dev laptop only. Node running on UDS, relay on localhost. Simulates concurrent WebSocket connections with asyncio tasks.
- D-05: Concurrency levels: 1, 10, 100 clients (per PERF-01 success criteria).
- D-06: PERF-01 (Throughput): Each client sends N small Data(8) blobs (e.g., 1 KB), measure total messages/sec at each concurrency level.
- D-07: PERF-02 (Latency overhead): Same operation (e.g., ReadRequest) through relay vs direct UDS. Measure per-operation time delta, report overhead percentage.
- D-08: PERF-03 (Large blobs): Write+read blobs at 1 MiB, 10 MiB, 50 MiB, 100 MiB. Report MiB/sec throughput for each size.
- D-09: PERF-04 (Mixed workload): Run concurrent small metadata queries (StatsRequest, ExistsRequest) alongside large blob transfers. Report whether small-message p99 latency degrades under large-blob load.
- D-10: Markdown report at `tools/benchmark_report.md`. Tables with: msgs/sec, p50/p95/p99 latency in ms, MiB/sec for large blobs. Human-readable.
- D-11: Report includes: test date, hardware info (CPU, RAM), relay config, node config, and raw numbers per workload.
- D-12: Latency: measure round-trip time (send request -> receive response) per operation. Report p50, p95, p99.
- D-13: Throughput: total operations / wall-clock time. Report per concurrency level.
- D-14: Large blob: total bytes transferred / wall-clock time. Report MiB/sec.

### Claude's Discretion
- Warm-up period before measuring (recommended: discard first N operations)
- Number of iterations per benchmark (enough for stable p99)
- Whether to include relay /metrics scrape in the report for cross-reference
- UDS direct benchmark implementation (raw TCP to UDS socket with AEAD, reuse relay_test_helpers pattern or Python equivalent)

### Deferred Ideas (OUT OF SCOPE)
- Automated regression benchmarks (CI integration) -- separate phase
- KVM swarm benchmarks -- only if local numbers look suspicious
- Grafana dashboard for benchmark results -- overkill for pre-MVP
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| PERF-01 | Relay throughput benchmark -- messages/sec at 1, 10, 100 concurrent clients | Python asyncio.gather() for concurrent WS connections; Data(8) write + WriteAck(30) round-trip; time.perf_counter_ns() for measurement |
| PERF-02 | Relay latency overhead -- relay vs direct UDS for same operation | Relay path: WS JSON round-trip; UDS path: TrustedHello + HKDF + AEAD + FlatBuffers transport; PyNaCl + oqs + flatbuffers all available |
| PERF-03 | Large blob throughput -- write+read through relay at various sizes | CRITICAL: Relay MAX_TEXT_MESSAGE_SIZE=1 MiB caps incoming text frames; base64 expansion means max raw blob ~750 KB; PERF-03 sizes must be adjusted |
| PERF-04 | Mixed workload -- concurrent small metadata queries + large blob transfers | asyncio task groups mixing StatsRequest/ExistsRequest coroutines with Data write coroutines; measure p99 degradation |
</phase_requirements>

## Critical Discovery: Relay Text Frame Size Limit

**Confidence: HIGH** (verified from source code)

The relay's WebSocket frame assembler enforces a 1 MiB limit on text messages:

```cpp
// relay/ws/ws_frame.h:22
constexpr size_t MAX_TEXT_MESSAGE_SIZE   = 1 * 1024 * 1024;    // 1 MiB
```

The relay ONLY accepts text frames from clients (binary frames are silently ignored):

```cpp
// relay/ws/ws_session.cpp:426
if (opcode != OPCODE_TEXT) {
    spdlog::debug("session {}: ignoring non-text frame in authenticated state", session_id_);
    co_return;
}
```

Since Data(8) blobs are JSON with base64-encoded data, the maximum raw blob size through the relay is approximately:
- 1 MiB text frame limit
- Minus JSON envelope overhead (~200-300 bytes for type, namespace, pubkey, ttl, timestamp, signature fields)
- The `data` field is base64-encoded (4/3 expansion)
- The `signature` field is 4627 bytes hex-encoded = ~9254 chars
- The `pubkey` field is 2592 bytes hex-encoded = ~5184 chars
- Effective max raw blob data: approximately 730-740 KB

**Impact on PERF-03:** The originally specified test sizes of 1 MiB, 10 MiB, 50 MiB, and 100 MiB CANNOT be tested through the relay as currently implemented. The benchmark should test at sizes like 1 KB, 10 KB, 100 KB, 500 KB, and ~730 KB (near-limit) to characterize large blob performance within the relay's actual operating envelope.

**Recommendation for planner:** Adjust PERF-03 test sizes to relay's actual limits. Report this finding as a relay capability constraint in the benchmark report. The benchmark itself discovers the practical limit, which is valuable data.

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| websockets | 16.0 | Async WebSocket client | Standard Python WS library; asyncio native; supports max_size=None for large frames |
| liboqs-python | 0.14.1 | ML-DSA-87 challenge-response auth | Already installed; matches relay's liboqs PQ crypto |
| asyncio | stdlib | Concurrent client simulation | Python stdlib; asyncio.gather() for parallel connections |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| PyNaCl | 1.5.0 | ChaCha20-Poly1305 AEAD for UDS direct path | PERF-02 UDS comparison only |
| flatbuffers | 25.12.19 | Transport message encoding for UDS direct path | PERF-02 UDS comparison only |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| websockets | aiohttp | websockets is simpler, purpose-built; D-01 locks this choice |
| PyNaCl | cryptography | PyNaCl wraps libsodium directly (matches C++ node); cryptography not installed |
| UDS direct | skip PERF-02 baseline | Reporting absolute relay latencies is still valuable without UDS comparison |

**Installation:**
```bash
pip3 install websockets
```

**Version verification:** websockets 16.0 confirmed available via `pip3 index versions websockets`. liboqs-python 0.14.1 and PyNaCl 1.5.0 already installed.

## Architecture Patterns

### Recommended Project Structure
```
tools/
    relay_benchmark.py     # Main benchmark script (~600-800 lines)
    benchmark_report.md    # Generated output report
```

Single-file standalone script per D-02. No package structure needed.

### Pattern 1: Async Client Pool
**What:** Each concurrent "client" is an asyncio coroutine holding one WebSocket connection. All clients authenticate independently. asyncio.gather() runs N clients in parallel.
**When to use:** All four PERF benchmarks.
**Example:**
```python
import asyncio
from websockets.asyncio.client import connect

async def benchmark_client(url, identity, iterations):
    async with connect(url, max_size=None) as ws:
        await authenticate(ws, identity)
        results = []
        for i in range(iterations):
            t0 = time.perf_counter_ns()
            await ws.send(json.dumps(make_data_msg(identity, i)))
            response = await ws.recv()
            t1 = time.perf_counter_ns()
            results.append(t1 - t0)
    return results

async def run_throughput(url, identity, num_clients, iterations):
    tasks = [benchmark_client(url, identity, iterations)
             for _ in range(num_clients)]
    all_results = await asyncio.gather(*tasks)
    return all_results
```

### Pattern 2: ML-DSA-87 Auth Flow (Python)
**What:** Implements the relay challenge-response auth exactly as the C++ smoke test does.
**When to use:** Every WebSocket connection in every benchmark.
**Example:**
```python
import oqs
import hashlib
import os

class BenchmarkIdentity:
    def __init__(self, key_path=None):
        self.sig = oqs.Signature('ML-DSA-87')
        if key_path:
            with open(key_path, 'rb') as f:
                sk = f.read()
            pub_path = key_path.replace('.key', '.pub')
            with open(pub_path, 'rb') as f:
                self.public_key = f.read()
            # oqs-python: set secret key via internal attribute
            self.sig.secret_key = sk
        else:
            self.public_key = self.sig.generate_keypair()
        self.namespace = hashlib.sha3_256(self.public_key).digest()

    def sign(self, message: bytes) -> bytes:
        return self.sig.sign(message)

async def authenticate(ws, identity):
    # Step 1: Receive challenge
    msg = json.loads(await ws.recv())
    assert msg["type"] == "challenge"
    challenge = bytes.fromhex(msg["nonce"])

    # Step 2: Sign and respond
    signature = identity.sign(challenge)
    await ws.send(json.dumps({
        "type": "challenge_response",
        "pubkey": identity.public_key.hex(),
        "signature": signature.hex()
    }))

    # Step 3: Receive auth_ok
    result = json.loads(await ws.recv())
    assert result["type"] == "auth_ok"
```

### Pattern 3: Blob Construction (Python)
**What:** Build a Data(8) JSON message with proper ML-DSA-87 signature over SHA3-256 digest.
**When to use:** PERF-01 (throughput), PERF-03 (large blob), PERF-04 (mixed).
**Example:**
```python
import struct, base64, time as time_mod

def make_data_message(identity, request_id, data_bytes, ttl=3600):
    timestamp = int(time_mod.time())
    ns = identity.namespace

    # Build signing input: SHA3-256(namespace || data || ttl_be32 || timestamp_be64)
    h = hashlib.sha3_256()
    h.update(ns)
    h.update(data_bytes)
    h.update(struct.pack('>I', ttl))
    h.update(struct.pack('>Q', timestamp))
    digest = h.digest()

    signature = identity.sign(digest)

    return {
        "type": "data",
        "request_id": request_id,
        "namespace": ns.hex(),
        "pubkey": identity.public_key.hex(),
        "data": base64.b64encode(data_bytes).decode(),
        "ttl": ttl,
        "timestamp": str(timestamp),
        "signature": base64.b64encode(signature).decode()
    }
```

### Pattern 4: Statistics Collection
**What:** Collect per-operation latencies using time.perf_counter_ns(), compute percentiles with sorted array and index math (no numpy needed).
**When to use:** All benchmarks.
**Example:**
```python
def percentile(sorted_values, p):
    """Compute p-th percentile from pre-sorted list."""
    if not sorted_values:
        return 0
    k = (len(sorted_values) - 1) * (p / 100.0)
    f = int(k)
    c = f + 1 if f + 1 < len(sorted_values) else f
    return sorted_values[f] + (k - f) * (sorted_values[c] - sorted_values[f])

def compute_stats(latencies_ns):
    sorted_lat = sorted(latencies_ns)
    return {
        "count": len(sorted_lat),
        "p50_ms": percentile(sorted_lat, 50) / 1e6,
        "p95_ms": percentile(sorted_lat, 95) / 1e6,
        "p99_ms": percentile(sorted_lat, 99) / 1e6,
        "min_ms": sorted_lat[0] / 1e6 if sorted_lat else 0,
        "max_ms": sorted_lat[-1] / 1e6 if sorted_lat else 0,
    }
```

### Pattern 5: UDS Direct Client (for PERF-02)
**What:** Connect to node's UDS socket, perform TrustedHello handshake with HKDF-SHA256 key derivation, then AEAD-encrypt binary requests.
**When to use:** PERF-02 baseline measurement only.
**Key complexity:** Requires FlatBuffers transport encoding, ML-DSA-87 identity handshake over UDS, ChaCha20-Poly1305 AEAD with counter-based nonces, and SyncNamespaceAnnounce(62) drain after handshake.
**Dependencies:** PyNaCl (AEAD), oqs (signing), flatbuffers (transport), stdlib hmac/hashlib (HKDF).

UDS handshake protocol:
1. Generate 32-byte nonce
2. Send TrustedHello: [nonce_i:32][signing_pubkey:2592] in FlatBuffer TransportMessage
3. Receive TrustedHello response: [nonce_r:32][signing_pk:2592]
4. IKM = nonce_i || nonce_r (64 bytes)
5. HKDF-SHA256(IKM, salt=empty, info="chromatin-init-to-resp-v1") -> initiator-to-responder key
6. HKDF-SHA256(IKM, salt=empty, info="chromatin-resp-to-init-v1") -> responder-to-initiator key
7. Drain unsolicited SyncNamespaceAnnounce(62) message
8. Send/receive AEAD-encrypted transport messages

### Anti-Patterns to Avoid
- **Shared OQS Signature object across coroutines:** ML-DSA-87 `oqs.Signature` objects are not thread-safe. Each coroutine/client must have its own signer instance.
- **Using default websockets max_size:** Default is 1 MiB (2^20). Must set `max_size=None` when testing near-limit blob sizes. Otherwise `ConnectionClosedError` with code 1009.
- **Measuring auth time in throughput:** Auth is one-time per connection. Warm-up should complete auth and discard initial operations before timing starts.
- **Forgetting to drain SyncNamespaceAnnounce on UDS:** Node sends unsolicited type 62 after every TrustedHello handshake. Must read and discard before issuing requests.
- **Using time.time() for latency:** Use `time.perf_counter_ns()` for nanosecond resolution. `time.time()` has ~1ms resolution on Linux.
- **Running 100 ML-DSA-87 keygen at startup:** Keygen is fast (1.3ms) but 100 independent keypairs means 100 independent namespaces. If the relay has an ACL, all 100 must be whitelisted. Generate one keypair and share it across all clients (same auth identity, different connections).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| WebSocket client | Raw socket + HTTP upgrade + frame encode/decode | `websockets` library | RFC 6455 compliance, masking, ping/pong, fragmentation |
| Statistics/percentiles | Full statistics library | Sorted array + index math | Only need p50/p95/p99; 10 lines of code vs numpy dependency |
| Base64 encode/decode | Custom implementation | `base64` stdlib module | Exact match to relay's base64 handling |
| JSON serialization | Manual string building | `json` stdlib module | Handles escaping, unicode, nested structures |
| Hardware info collection | Parse /proc manually | `platform` + `os.cpu_count()` + `/proc/meminfo` read | Standard approach for benchmark reports |

## Common Pitfalls

### Pitfall 1: Relay Rate Limiting
**What goes wrong:** With 100 concurrent clients each sending messages as fast as possible, the relay's rate limiter (token bucket, per-session) rejects messages and disconnects after 10 consecutive rejections.
**Why it happens:** `rate_limit_messages_per_sec` config defaults to 0 (disabled), but if enabled, throughput benchmark hits the limit.
**How to avoid:** Document that benchmarks require `rate_limit_messages_per_sec = 0` in relay config. Verify this in the benchmark's pre-flight check.
**Warning signs:** `Close(4002)` disconnections during benchmark run.

### Pitfall 2: Relay Max Connections
**What goes wrong:** With 100 concurrent clients, if `max_connections` is less than 100, the relay rejects new connections.
**Why it happens:** Default max_connections is 1024, so 100 clients fits. But a custom config could lower it.
**How to avoid:** Pre-flight check: parse relay config and warn if max_connections < planned concurrency.
**Warning signs:** TCP connection refused or WS upgrade failure.

### Pitfall 3: Python GIL and asyncio Performance
**What goes wrong:** asyncio runs all coroutines in a single thread. CPU-bound work (JSON serialization, base64 encoding, ML-DSA-87 signing) blocks the event loop, skewing latency measurements.
**Why it happens:** Python GIL means only one coroutine runs at a time. Heavy signing in one coroutine delays recv() in another.
**How to avoid:** Pre-sign all blob messages before the timed measurement phase. During measurement, only time the send/recv cycle. ML-DSA-87 signing is 0.1ms, so impact is minor for small iteration counts, but compounds at scale.
**Warning signs:** Latency variance increases disproportionately with client count.

### Pitfall 4: WebSocket Ping/Pong Interference
**What goes wrong:** The relay sends WebSocket Pings every 30 seconds (per Phase 101). The `websockets` library handles Pong automatically, but a long-running benchmark may see Pings interleaved with data, affecting recv() timing.
**Why it happens:** relay/ws_session.cpp has a 30-second ping_loop; the websockets library auto-responds with Pong.
**How to avoid:** The `websockets` library handles Pings transparently (filters them from recv()). Benchmark runs should be shorter than 30 seconds per workload to avoid interference. If longer, accept that occasional Ping processing adds ~0.1ms variance.
**Warning signs:** Occasional outlier latencies at ~30 second intervals.

### Pitfall 5: Request Timeout
**What goes wrong:** Relay has a 10-second default request timeout (Phase 999.3). If a benchmark operation takes longer than 10 seconds (possible with large blobs or high contention), the relay sends a timeout error instead of the expected response.
**Why it happens:** `request_timeout_seconds = 10` in relay config.
**How to avoid:** Set `request_timeout_seconds = 0` (disabled) in relay config for benchmarking. Document this in the benchmark's pre-flight requirements.
**Warning signs:** `{"type": "error", "code": "timeout"}` responses.

### Pitfall 6: oqs.Signature Secret Key Loading
**What goes wrong:** `liboqs-python` `Signature` class doesn't have a clean "load from bytes" API for secret keys. The `generate_keypair()` method generates a new key internally.
**Why it happens:** The oqs-python API is designed around generate -> use flow, not load existing keys.
**How to avoid:** For the benchmark, generate a fresh ephemeral keypair at startup (per D-03). Only load from file if the relay has ACL restrictions requiring a specific whitelisted key. For file loading, use the internal `sig.secret_key = sk_bytes` attribute assignment (verified working).
**Warning signs:** `oqs.MechanismNotSupportedError` if algorithm name is wrong.

## Code Examples

### Complete Auth Flow (verified against relay source)
```python
# Source: relay/ws/ws_session.cpp:153-175, tools/relay_test_helpers.h:337-467
import json
import oqs
import hashlib

async def authenticate(ws, identity):
    """Complete ML-DSA-87 challenge-response auth with relay."""
    # Relay sends: {"type": "challenge", "nonce": "<64-char hex>"}
    challenge_msg = json.loads(await ws.recv())
    assert challenge_msg["type"] == "challenge", f"Expected challenge, got {challenge_msg['type']}"
    challenge_bytes = bytes.fromhex(challenge_msg["nonce"])
    assert len(challenge_bytes) == 32

    # Client signs the raw challenge bytes
    signature = identity.sign(challenge_bytes)

    # Client responds: {"type": "challenge_response", "pubkey": "<hex>", "signature": "<hex>"}
    await ws.send(json.dumps({
        "type": "challenge_response",
        "pubkey": identity.public_key.hex(),
        "signature": signature.hex()
    }))

    # Relay responds: {"type": "auth_ok", "namespace": "<64-char hex>"}
    result = json.loads(await ws.recv())
    assert result["type"] == "auth_ok", f"Auth failed: {result}"
    return result["namespace"]
```

### Data Message Construction (verified against relay translator)
```python
# Source: relay/translate/translator.cpp:188-228, tools/relay_test_helpers.h:293-313
import base64, struct, time as time_mod, hashlib

def make_data_message(identity, request_id, raw_data, ttl=3600):
    """Build a Data(8) JSON message with ML-DSA-87 signature."""
    timestamp = int(time_mod.time())
    ns = identity.namespace  # SHA3-256(pubkey), 32 bytes

    # Signing input: SHA3-256(ns || data || ttl_be32 || timestamp_be64)
    h = hashlib.sha3_256()
    h.update(ns)
    h.update(raw_data)
    h.update(struct.pack('>I', ttl))    # Big-endian uint32
    h.update(struct.pack('>Q', timestamp))  # Big-endian uint64
    digest = h.digest()

    signature = identity.sign(digest)

    return {
        "type": "data",
        "request_id": request_id,
        "namespace": ns.hex(),
        "pubkey": identity.public_key.hex(),
        "data": base64.b64encode(raw_data).decode('ascii'),
        "ttl": ttl,
        "timestamp": str(timestamp),
        "signature": base64.b64encode(signature).decode('ascii')
    }
```

### ReadRequest + ExistsRequest (for PERF-02, PERF-04)
```python
# Source: relay/translate/json_schema.h READ_REQUEST_FIELDS, EXISTS_REQUEST_FIELDS
def make_read_request(request_id, namespace_hex, hash_hex):
    return {
        "type": "read_request",
        "request_id": request_id,
        "namespace": namespace_hex,
        "hash": hash_hex
    }

def make_exists_request(request_id, namespace_hex, hash_hex):
    return {
        "type": "exists_request",
        "request_id": request_id,
        "namespace": namespace_hex,
        "hash": hash_hex
    }

def make_stats_request(request_id, namespace_hex):
    return {
        "type": "stats_request",
        "request_id": request_id,
        "namespace": namespace_hex
    }
```

### Report Generation
```python
def generate_report(results, hw_info, relay_config):
    """Generate markdown benchmark report."""
    lines = [
        "# Relay Performance Benchmark Report",
        "",
        f"**Date:** {results['date']}",
        f"**Hardware:** {hw_info['cpu']} ({hw_info['cores']} cores), {hw_info['ram']}",
        f"**Relay config:** rate_limit={relay_config.get('rate_limit', 'disabled')}, "
        f"max_connections={relay_config.get('max_connections', 1024)}",
        "",
        "## PERF-01: Throughput",
        "",
        "| Clients | msgs/sec | p50 (ms) | p95 (ms) | p99 (ms) |",
        "|---------|----------|----------|----------|----------|",
    ]
    for level in results['throughput']:
        lines.append(
            f"| {level['clients']} | {level['msgs_per_sec']:.0f} | "
            f"{level['p50_ms']:.2f} | {level['p95_ms']:.2f} | {level['p99_ms']:.2f} |"
        )
    # ... additional sections for PERF-02, PERF-03, PERF-04
    return "\n".join(lines)
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Binary WS frames for ReadResponse | All responses as JSON text frames | Phase 999.5 | Simplifies client, but limits max response size to text frame limit |
| No blob size limit | Configurable max_blob_size_bytes | Phase 109 (FEAT-02) | Benchmark must test with limit disabled (0) |
| No request timeout | 10s default request_timeout_seconds | Phase 999.3 | Benchmark must disable (0) for accurate results |

## Open Questions

1. **PERF-03 Size Adjustment**
   - What we know: Relay MAX_TEXT_MESSAGE_SIZE = 1 MiB, base64 expansion makes max raw blob ~730-740 KB
   - What's unclear: Whether the user intended to test relay's protocol limits or expected larger blob support
   - Recommendation: Test at 1 KB, 10 KB, 100 KB, 500 KB, ~730 KB. Document the text frame limit as a relay capability finding. The benchmark report should note this as an engineering constraint.

2. **PERF-02 UDS Implementation Scope**
   - What we know: Full UDS client needs TrustedHello + HKDF + AEAD + FlatBuffers (4 protocol layers). All Python deps available. Approximately 200-300 lines of UDS client code.
   - What's unclear: Whether the overhead of Python FlatBuffers encoding biases the UDS baseline (Python FlatBuffers is slower than C++ FlatBuffers)
   - Recommendation: Implement the UDS direct path. The Python overhead is consistent across both paths and cancels out when computing the relay-specific overhead delta. The absolute UDS latency may be higher than a C++ client would achieve, but the relative overhead (relay - UDS) is what PERF-02 measures.

3. **Shared Identity vs Per-Client Identity**
   - What we know: All concurrent clients could share one ML-DSA-87 identity (same auth, different WS connections). The relay allows multiple connections from the same namespace.
   - What's unclear: Whether same-namespace concurrent writes create node-side contention that skews benchmarks
   - Recommendation: Use one shared identity for simplicity. The benchmark measures relay performance, not node write contention. Document this choice in the report.

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| Python 3 | All benchmarks | Yes | 3.14.4 | -- |
| websockets | WS connections | No (not installed) | 16.0 available | pip3 install websockets |
| liboqs-python | ML-DSA-87 auth | Yes | 0.14.1 | -- |
| PyNaCl | UDS AEAD (PERF-02) | Yes | 1.5.0 | -- |
| flatbuffers | UDS transport (PERF-02) | Yes | 25.12.19 | -- |
| Running chromatindb node | All benchmarks | Manual | -- | User must start node |
| Running relay | All benchmarks | Manual | -- | User must start relay |

**Missing dependencies with no fallback:**
- websockets: `pip3 install websockets` (required before running benchmark)

**Missing dependencies with fallback:**
- None

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Manual validation (benchmark tool is itself the test) |
| Config file | None -- standalone script |
| Quick run command | `python3 tools/relay_benchmark.py --host 127.0.0.1 --port 4201 --quick` |
| Full suite command | `python3 tools/relay_benchmark.py --host 127.0.0.1 --port 4201` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| PERF-01 | Throughput at 1/10/100 clients | benchmark | `python3 tools/relay_benchmark.py --workload throughput` | Wave 0 |
| PERF-02 | Relay vs UDS latency | benchmark | `python3 tools/relay_benchmark.py --workload latency` | Wave 0 |
| PERF-03 | Large blob throughput | benchmark | `python3 tools/relay_benchmark.py --workload large-blob` | Wave 0 |
| PERF-04 | Mixed workload p99 | benchmark | `python3 tools/relay_benchmark.py --workload mixed` | Wave 0 |

### Sampling Rate
- **Per task commit:** Syntax check: `python3 -c "import ast; ast.parse(open('tools/relay_benchmark.py').read())"`
- **Per wave merge:** Full benchmark requires running node+relay (user intervention)
- **Phase gate:** All 4 workloads produce results in benchmark_report.md

### Wave 0 Gaps
- [ ] `tools/relay_benchmark.py` -- the entire benchmark tool (this phase creates it)
- [ ] `tools/benchmark_report.md` -- generated by the tool
- [ ] `pip3 install websockets` -- runtime dependency

## Dev Machine Hardware (for Report Context)

| Property | Value |
|----------|-------|
| CPU | AMD Ryzen 5 5600U (6 cores, 12 threads) |
| RAM | 7.1 GiB |
| OS | Linux 6.19.11-arch1-1 (x86_64) |
| Python | 3.14.4 |

## ML-DSA-87 Performance (Measured)

| Operation | Time | Notes |
|-----------|------|-------|
| Keygen | 1.3 ms | One-time per benchmark run |
| Sign (32-byte digest) | 0.1 ms | Per-blob, per-auth |

Auth signing overhead is negligible compared to WS round-trip time.

## Relay JSON Message Protocol Reference

### Auth Sequence
1. Client connects WebSocket to `ws://host:port/`
2. Relay sends: `{"type": "challenge", "nonce": "<64-char hex>"}`
3. Client signs raw 32-byte challenge with ML-DSA-87
4. Client sends: `{"type": "challenge_response", "pubkey": "<hex>", "signature": "<hex>"}`
5. Relay sends: `{"type": "auth_ok", "namespace": "<64-char hex>"}`

### Key Message Types for Benchmarks
| JSON type | Wire | Direction | Use in Benchmark |
|-----------|------|-----------|-----------------|
| `data` | 8 | client->node | PERF-01 throughput, PERF-03 large blob, PERF-04 mixed |
| `write_ack` | 30 | node->client | Response to data writes |
| `read_request` | 31 | client->node | PERF-02 latency, PERF-04 mixed |
| `read_response` | 32 | node->client | Response to reads |
| `stats_request` | 35 | client->node | PERF-04 mixed small query |
| `stats_response` | 36 | node->client | Response to stats |
| `exists_request` | 37 | client->node | PERF-02 latency, PERF-04 mixed small query |
| `exists_response` | 38 | node->client | Response to exists |

## Sources

### Primary (HIGH confidence)
- relay/ws/ws_frame.h -- MAX_TEXT_MESSAGE_SIZE = 1 MiB, MAX_BINARY_MESSAGE_SIZE = 110 MiB
- relay/ws/ws_session.cpp -- text-only frame acceptance, auth flow, rate limiting, blob size check
- relay/config/relay_config.h -- all config fields with defaults
- relay/core/authenticator.h -- ML-DSA-87 challenge-response API
- relay/translate/json_schema.h -- all JSON field encodings and message schemas
- relay/translate/json_schema.cpp -- full type registry (41 types)
- relay/identity/relay_identity.cpp -- key file format (raw binary, .key/.pub pair)
- tools/relay_test_helpers.h -- complete C++ auth flow + blob construction reference
- relay/core/rate_limiter.h -- token bucket rate limiter
- relay/core/uds_multiplexer.h/cpp -- TrustedHello + HKDF + AEAD handshake protocol
- db/schemas/transport.fbs -- TransportMessage FlatBuffer schema

### Secondary (MEDIUM confidence)
- [websockets PyPI](https://pypi.org/project/websockets/) -- v16.0, max_size default = 2^20
- [websockets GitHub](https://github.com/python-websockets/websockets) -- asyncio client API

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries verified installed/available, versions confirmed
- Architecture: HIGH -- patterns derived directly from relay source code and working C++ smoke test
- Pitfalls: HIGH -- derived from relay source code analysis (rate limiter, max connections, frame limits)
- Critical discovery (text frame limit): HIGH -- verified from ws_frame.h constant and ws_session.cpp enforcement code

**Research date:** 2026-04-11
**Valid until:** 2026-05-11 (stable -- relay code is in hardening phase, no structural changes expected)
