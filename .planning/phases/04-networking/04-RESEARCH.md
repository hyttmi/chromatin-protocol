# Phase 4: Networking - Research

**Researched:** 2026-03-04
**Domain:** PQ-encrypted TCP transport with Standalone Asio C++20 coroutines
**Confidence:** HIGH

## Summary

Phase 4 builds the PQ-encrypted TCP transport layer using Standalone Asio with C++20 coroutines. The handshake protocol uses ML-KEM-1024 for key exchange, HKDF-SHA256 to derive session keys, and ML-DSA-87 for mutual authentication -- all crypto primitives already implemented and tested in Phase 1. The wire framing uses 4-byte length-prefixed messages with ChaCha20-Poly1305 encryption and counter-based nonces.

Standalone Asio 1.38.0 (latest stable, December 2025) provides `co_spawn`, `awaitable<T>`, and `use_awaitable` for C++20 coroutines. The `as_tuple` adapter enables error handling without exceptions. Signal handling uses `asio::signal_set` for SIGTERM/SIGINT with graceful shutdown. The OlivierLDff/asio.cmake wrapper provides clean CMake FetchContent integration.

**Primary recommendation:** Build bottom-up: transport framing first (testable without crypto), then handshake/auth layer, then connection manager with signal handling. Each layer testable in isolation.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Initiator sends ML-KEM-1024 public key first; responder encapsulates and replies with ciphertext
- Session fingerprint for mutual auth: SHA3-256(shared_secret || initiator_pubkey || responder_pubkey)
- Both nodes sign the session fingerprint with ML-DSA-87 after the encrypted channel is established
- On handshake failure (bad KEM, invalid signature, auth rejection): silent TCP close + local log -- no error details sent to peer
- 10-second handshake timeout -- kill connection if handshake doesn't complete
- Persistent connections -- stay open after handshake, avoid repeated PQ handshake overhead
- Auto-reconnect on unexpected disconnect with exponential backoff (1s, 2s, 4s... capped at 60s)
- No max connection limit for now (pre-MVP, mesh will be small)
- Application-level ping/pong heartbeat (~30s interval) to detect dead connections
- SIGTERM/SIGINT triggers graceful shutdown with 5-second drain timeout
- Send goodbye message to connected peers before closing (peers can skip reconnect backoff)
- Second SIGTERM/SIGINT during drain = force immediate exit
- Verbose shutdown logging: log each step at info level
- 4-byte big-endian uint32_t length prefix, then encrypted payload
- FlatBuffers for all transport-level messages (handshake, ping/pong, goodbye, blob transfer)
- 16 MB max message size -- reject frames exceeding this before allocating memory
- Counter-based nonces: 64-bit send counter per direction, zero overhead per frame

### Claude's Discretion
- Exact Asio event loop architecture (single-threaded vs strand-per-connection)
- Internal buffer management and memory allocation strategy
- Specific FlatBuffer schema design for transport messages
- Ping/pong timeout threshold and dead-peer detection logic
- HKDF context strings for key derivation from shared secret

### Deferred Ideas (OUT OF SCOPE)
- None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| TRNS-01 | Node-to-node connections use ML-KEM-1024 key exchange to establish shared secret | Handshake protocol section: initiator sends ephemeral KEM pubkey, responder encapsulates, HKDF derives session keys |
| TRNS-02 | After key exchange, channel is encrypted with AES-256-GCM | **Note:** CONTEXT.md overrides to ChaCha20-Poly1305 (locked decision, already implemented in src/crypto/aead.h). Counter-based nonces per direction. |
| TRNS-03 | Transport includes mutual authentication via ML-DSA-87 signed key exchange | Auth protocol section: both sides sign SHA3-256(shared_secret \|\| initiator_pubkey \|\| responder_pubkey) after encrypted channel established |
| TRNS-04 | Node listens for inbound TCP connections on configurable bind address | Asio TCP acceptor with co_spawn coroutines, reads bind_address from config |
| TRNS-05 | Node makes outbound TCP connections to configured peers | Asio tcp::socket::async_connect with use_awaitable, reads bootstrap_peers from config |
| DAEM-03 | Node handles signals for graceful shutdown | asio::signal_set for SIGTERM/SIGINT, 5s drain timeout, goodbye message to peers |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | 1.38.0 | Async TCP networking, signal handling, timers | De facto C++ async networking; C++20 coroutine support via awaitable/co_spawn |
| asio.cmake | main | CMake FetchContent wrapper for Standalone Asio | Clean integration; creates asio::asio target; supports version pinning via ASIO_TAG |

### Supporting (already in project)
| Library | Version | Purpose | Integration Point |
|---------|---------|---------|-------------------|
| liboqs | 0.15.0 | ML-KEM-1024 (key exchange), ML-DSA-87 (signing) | crypto::KEM, crypto::Signer already implemented |
| libsodium | latest | ChaCha20-Poly1305 AEAD, HKDF-SHA256 | crypto::AEAD, crypto::KDF already implemented |
| FlatBuffers | 25.2.10 | Wire format for transport messages | Extend existing schemas for transport protocol |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Standalone Asio | Boost.Asio | Heavier dependency; same API but pulls in Boost headers. Standalone is lighter. |
| Standalone Asio | libuv | C library; no native C++20 coroutine support; requires wrapping. |
| Single-threaded io_context | Thread-per-connection | Wastes threads; coroutines handle concurrency without threads for small mesh. |

### CMake Integration

```cmake
# Standalone Asio via asio.cmake wrapper
set(ASIO_TAG "asio-1-38-0" CACHE STRING "" FORCE)
FetchContent_Declare(asiocmake
  GIT_REPOSITORY https://github.com/OlivierLDff/asio.cmake
  GIT_TAG main
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(asiocmake)

# Link: target_link_libraries(chromatindb_lib PUBLIC asio::asio)
```

## Architecture Patterns

### Recommended Project Structure
```
src/
├── net/
│   ├── transport.h/.cpp      # Length-prefixed framing + AEAD encrypt/decrypt
│   ├── handshake.h/.cpp      # ML-KEM key exchange + ML-DSA mutual auth
│   ├── connection.h/.cpp     # Single peer connection lifecycle (persistent, heartbeat)
│   ├── server.h/.cpp         # TCP acceptor + connection manager + signal handling
│   └── protocol.h            # Transport message types (FlatBuffer-generated)
├── wire/
│   ├── transport_generated.h  # FlatBuffers-generated transport messages
│   └── ...existing...
└── ...existing...

schemas/
├── blob.fbs                   # existing
└── transport.fbs              # NEW: handshake, ping, pong, goodbye, data envelope
```

### Pattern 1: Single-Threaded io_context with Coroutines (RECOMMENDED)

**What:** One `asio::io_context` running on the main thread. All connections handled as coroutines via `co_spawn`. No threads, no mutexes.

**When to use:** Small peer mesh (< 100 connections), CPU-light networking (PQ crypto is the bottleneck, not connection count).

**Why for chromatindb:** Pre-MVP, mesh will be small. Single-threaded avoids all data races. PQ handshake crypto (~0.5ms) is fast enough. Can upgrade to thread pool later if needed.

**Example:**
```cpp
asio::awaitable<void> listener(asio::ip::tcp::acceptor acceptor) {
    for (;;) {
        auto [ec, socket] = co_await acceptor.async_accept(
            asio::as_tuple(asio::use_awaitable));
        if (ec) {
            spdlog::warn("accept error: {}", ec.message());
            continue;
        }
        asio::co_spawn(acceptor.get_executor(),
            handle_connection(std::move(socket)),
            asio::detached);
    }
}

int main() {
    asio::io_context ioc;
    // ... setup acceptor, signal_set ...
    ioc.run();  // blocks until stopped
}
```

### Pattern 2: Length-Prefixed Frame IO

**What:** Read/write frames as [4-byte BE length][encrypted payload]. Enforce max size before allocating.

**Example:**
```cpp
asio::awaitable<std::vector<uint8_t>> read_frame(asio::ip::tcp::socket& socket) {
    // Read 4-byte length header
    std::array<uint8_t, 4> len_buf;
    co_await asio::async_read(socket, asio::buffer(len_buf),
        asio::as_tuple(asio::use_awaitable));
    uint32_t len = (len_buf[0] << 24) | (len_buf[1] << 16) |
                   (len_buf[2] << 8)  | len_buf[3];
    if (len > MAX_FRAME_SIZE) throw std::runtime_error("frame too large");

    // Read payload
    std::vector<uint8_t> payload(len);
    co_await asio::async_read(socket, asio::buffer(payload),
        asio::as_tuple(asio::use_awaitable));
    co_return payload;
}
```

### Pattern 3: Counter-Based Nonces

**What:** Each direction has a 64-bit counter starting at 0. Nonce = 4 zero bytes + 8-byte BE counter. Never reuse because counter only increments.

**Example:**
```cpp
std::array<uint8_t, 12> make_nonce(uint64_t counter) {
    std::array<uint8_t, 12> nonce{};
    // First 4 bytes zero, last 8 bytes = big-endian counter
    for (int i = 7; i >= 0; --i) {
        nonce[4 + i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    return nonce;
}
```

### Pattern 4: Graceful Shutdown via signal_set

**What:** Register SIGTERM/SIGINT with `asio::signal_set`. First signal starts drain, second forces exit.

**Example:**
```cpp
asio::signal_set signals(ioc, SIGINT, SIGTERM);
bool draining = false;
signals.async_wait([&](auto ec, int sig) {
    if (draining) {
        spdlog::info("second signal received, forcing exit");
        std::_Exit(1);
    }
    draining = true;
    spdlog::info("signal {} received, draining connections...", sig);
    // Start drain: send goodbye to all peers, wait up to 5s, then stop ioc
    start_drain(ioc, connection_manager, std::chrono::seconds(5));
});
```

### Anti-Patterns to Avoid
- **Thread-per-connection:** Wasteful for persistent connections; coroutines handle this natively
- **Raw socket reads without framing:** TCP is a byte stream; always frame messages
- **Exceptions for expected errors:** Use `as_tuple` for expected failures (connection refused, timeout); reserve exceptions for programmer errors
- **Blocking crypto in event loop:** Not an issue here (PQ ops are <1ms) but if future crypto is slow, offload to thread pool

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Async TCP IO | Raw epoll/kqueue wrappers | Asio io_context + co_spawn | Cross-platform, coroutine-native, battle-tested |
| Timer management | std::thread + sleep | asio::steady_timer with co_await | Integrates with event loop, no extra threads |
| Signal handling | signal()/sigaction() manually | asio::signal_set | Thread-safe, integrates with io_context |
| Reconnect with backoff | Custom timer loops | asio::steady_timer + exponential delay formula | Clean, cancellable, integrates with shutdown |
| Nonce management | Random nonces per message | Counter-based nonces (locked decision) | Deterministic, no RNG overhead, AEAD failure detects desync |

**Key insight:** Asio provides the entire async runtime. Every timer, signal, and socket operation goes through the io_context. Fighting this (e.g., separate threads for timers) creates complexity with no benefit.

## Common Pitfalls

### Pitfall 1: Dangling References in Coroutines
**What goes wrong:** Coroutine captures reference to local variable; variable dies before coroutine resumes at co_await.
**Why it happens:** co_await suspends the function; locals in the caller's scope may be destroyed.
**How to avoid:** Pass by value or use shared_ptr for anything that outlives a co_await. Move sockets into coroutine parameters.
**Warning signs:** Use-after-free, intermittent crashes on connection close.

### Pitfall 2: Forgetting as_tuple on Expected Errors
**What goes wrong:** Connection reset / timeout throws system_error, unwinds the coroutine unexpectedly.
**Why it happens:** `use_awaitable` alone converts errors to exceptions.
**How to avoid:** Always use `asio::as_tuple(asio::use_awaitable)` for operations that can fail in normal operation (connect, read, write, accept). Check ec before proceeding.
**Warning signs:** Unhandled exceptions in coroutine, connections silently dropped.

### Pitfall 3: Partial Reads/Writes on TCP
**What goes wrong:** `async_read_some` returns fewer bytes than expected; parsing garbage data.
**Why it happens:** TCP is a byte stream, not a message stream.
**How to avoid:** Use `asio::async_read` (not `async_read_some`) with exact buffer size for length-prefixed framing. It reads until buffer is full.
**Warning signs:** Corrupt messages, protocol desync.

### Pitfall 4: Not Cancelling Timers on Shutdown
**What goes wrong:** io_context::run() never returns because timers keep the event loop alive.
**Why it happens:** Outstanding async_wait on timers keeps work pending.
**How to avoid:** Cancel all timers during drain. Call `timer.cancel()` before stopping io_context.
**Warning signs:** Process hangs on SIGTERM.

### Pitfall 5: PQ Key Sizes in Handshake
**What goes wrong:** Buffer too small for ML-KEM-1024 public key (1568 bytes) or ML-DSA-87 signature (up to 4627 bytes).
**Why it happens:** PQ keys are much larger than classical keys.
**How to avoid:** Use the constants from existing crypto wrappers (KEM::PUBLIC_KEY_SIZE, Signer::MAX_SIGNATURE_SIZE). Always use the length-prefixed framing, never assume fixed sizes for variable-length signatures.
**Warning signs:** Buffer overflows, truncated keys.

### Pitfall 6: AEAD Nonce Reuse
**What goes wrong:** Catastrophic security failure -- attacker can recover plaintext and forge messages.
**Why it happens:** Bug in counter logic, or using same counter for both send/receive directions.
**How to avoid:** Separate counters per direction (send_counter, recv_counter). Each starts at 0 and only increments. Never reset.
**Warning signs:** None visible -- this is a silent cryptographic failure. Must get it right by design.

## Code Examples

### Handshake Protocol Flow

```
INITIATOR                                RESPONDER
    |                                        |
    |--- [ephemeral KEM pubkey (1568B)] ---->|
    |                                        | encapsulate(pubkey) -> (ct, ss)
    |<--- [KEM ciphertext (1568B)] ---------|
    | decapsulate(ct) -> ss                  |
    |                                        |
    |  [ENCRYPTED CHANNEL ESTABLISHED]       |
    |  HKDF-SHA256(ss) -> send_key, recv_key |
    |                                        |
    | fingerprint = SHA3-256(ss || init_pk || resp_pk)
    |                                        |
    |--- [ML-DSA-87 sig(fingerprint)] ------>| verify(sig, init_pk)
    |<--- [ML-DSA-87 sig(fingerprint)] ------| verify(sig, resp_pk)
    |                                        |
    |  [MUTUALLY AUTHENTICATED]              |
```

### Key Derivation from Shared Secret

```cpp
// After ML-KEM key exchange, derive directional session keys
// Use HKDF with direction-specific context strings
auto prk = crypto::KDF::extract(/*salt=*/{}, shared_secret.span());

// Initiator->Responder key
auto init_to_resp_key = crypto::KDF::expand(
    prk.span(), "chromatin-init-to-resp-v1", crypto::AEAD::KEY_SIZE);

// Responder->Initiator key
auto resp_to_init_key = crypto::KDF::expand(
    prk.span(), "chromatin-resp-to-init-v1", crypto::AEAD::KEY_SIZE);
```

### Session Fingerprint for Mutual Auth

```cpp
// SHA3-256(shared_secret || initiator_pubkey || responder_pubkey)
std::vector<uint8_t> fingerprint_input;
fingerprint_input.insert(fingerprint_input.end(),
    shared_secret.data(), shared_secret.data() + shared_secret.size());
fingerprint_input.insert(fingerprint_input.end(),
    init_pubkey.begin(), init_pubkey.end());
fingerprint_input.insert(fingerprint_input.end(),
    resp_pubkey.begin(), resp_pubkey.end());
auto fingerprint = crypto::sha3_256(fingerprint_input);
```

### Transport FlatBuffer Schema Design

```flatbuffers
// schemas/transport.fbs
namespace chromatin.wire;

enum TransportMsgType : byte {
    KemPubkey = 1,
    KemCiphertext = 2,
    AuthSignature = 3,
    Ping = 4,
    Pong = 5,
    Goodbye = 6,
    Data = 7
}

table TransportMessage {
    type: TransportMsgType;
    payload: [ubyte];
}

root_type TransportMessage;
```

### Asio TCP Server with Coroutines

```cpp
#include <asio.hpp>
#include <asio/experimental/as_tuple.hpp>

using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

awaitable<void> handle_peer(tcp::socket socket) {
    // 1. Perform handshake (KEM + auth)
    // 2. Enter message loop (read frames, dispatch)
    // 3. Handle disconnect (log, maybe reconnect)
}

awaitable<void> listen(tcp::acceptor acceptor) {
    for (;;) {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) break;
        co_spawn(acceptor.get_executor(), handle_peer(std::move(socket)), detached);
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Boost.Asio callbacks | co_spawn + awaitable (C++20) | Asio 1.18+ (2020) | Linear async code, no callback hell |
| experimental::as_tuple | asio::as_tuple (promoted) | Asio 1.28+ (2023) | No longer experimental, stable API |
| RSA/ECDH key exchange | ML-KEM-1024 (FIPS 203) | NIST 2024 | Post-quantum security |
| RSA/ECDSA auth | ML-DSA-87 (FIPS 204) | NIST 2024 | Post-quantum signatures |

**Note on as_tuple:** In older Asio versions, `as_tuple` was under `asio::experimental`. In Asio 1.28+, it was promoted to `asio::as_tuple`. Use the non-experimental version. The header is `<asio/as_tuple.hpp>`.

## Open Questions

1. **as_tuple exact header in Standalone Asio 1.38.0**
   - What we know: Promoted from experimental in recent versions. Header is `<asio/as_tuple.hpp>`.
   - What's unclear: Whether 1.38.0 still supports both experimental and promoted paths.
   - Recommendation: Try `<asio/as_tuple.hpp>` first, fall back to experimental if needed. LOW confidence.

2. **ASIO_STANDALONE define requirement**
   - What we know: The asio.cmake wrapper handles this automatically via target compile definitions.
   - What's unclear: Whether we need to set it explicitly when using FetchContent.
   - Recommendation: Use asio.cmake wrapper which sets `ASIO_STANDALONE` and `ASIO_NO_DEPRECATED`. The wrapper's CMakeLists.txt handles this.

## Sources

### Primary (HIGH confidence)
- [Standalone Asio GitHub](https://github.com/chriskohlhoff/asio) - version 1.38.0 confirmed (Dec 2025)
- [Asio C++20 Coroutines Documentation](https://think-async.com/Asio/asio-1.22.0/doc/asio/overview/core/cpp20_coroutines.html) - co_spawn, awaitable, use_awaitable API
- [OlivierLDff/asio.cmake](https://github.com/OlivierLDff/asio.cmake) - CMake FetchContent wrapper, ASIO_TAG configuration
- [Frontiers PQ Session Protocol Paper](https://www.frontiersin.org/journals/physics/articles/10.3389/fphy.2025.1723966/full) - ML-KEM + ML-DSA handshake design reference

### Secondary (MEDIUM confidence)
- [Boost.Asio 1.87.0 C++20 Coroutines](https://live.boost.org/doc/libs/1_87_0/doc/html/boost_asio/overview/composition/cpp20_coroutines.html) - as_tuple promotion, cancellation state
- [Signal Handling in Boost.Asio](https://iifx.dev/en/articles/460263451/thread-safe-signal-handling-in-boost-asio-best-practices-and-examples) - signal_set patterns

### Tertiary (LOW confidence)
- [Asio Coroutines Blog](https://xc-jp.github.io/blog-posts/2022/03/03/Asio-Coroutines.html) - composition patterns (2022, may be slightly outdated)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - Standalone Asio is the only serious choice; version confirmed on GitHub
- Architecture: HIGH - Single-threaded io_context with coroutines is well-documented, matches pre-MVP scope
- Pitfalls: HIGH - Based on established Asio patterns and PQ crypto size considerations from existing codebase
- Handshake protocol: HIGH - ML-KEM encaps/decaps + HKDF derivation is standard KEM usage; mutual auth via signed fingerprint is established pattern

**Research date:** 2026-03-04
**Valid until:** 2026-04-04 (stable domain, Asio API unlikely to change)
