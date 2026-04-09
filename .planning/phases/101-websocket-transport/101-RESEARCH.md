# Phase 101: WebSocket Transport - Research

**Researched:** 2026-04-09
**Domain:** WebSocket (RFC 6455) over TLS with Asio SSL, hand-rolled framing, SIGHUP-reloadable TLS
**Confidence:** HIGH

## Summary

Phase 101 implements the WebSocket transport layer for Relay v2: WSS and WS listeners with RFC 6455 framing, TLS termination via system OpenSSL 3.6.2, and SIGHUP-reloadable TLS context. The implementation is hand-rolled on top of standalone Asio 1.38.0 because all third-party WebSocket libraries are incompatible with standalone Asio (Boost.Beast requires Boost.Asio namespace, WebSocket++ broken with Asio 1.28+ executor changes, others are C++11 callback-based). The existing Prometheus HTTP endpoint in `db/peer/metrics_collector.cpp` already proves the hand-rolled HTTP parsing pattern, and the RFC 6455 server-side subset is ~200 lines of deterministic framing logic.

The key new system dependency is OpenSSL 3.3+ via `find_package(OpenSSL REQUIRED)` -- already present on the build host as OpenSSL 3.6.2. Asio 1.38.0 provides `ssl::context` with `tlsv13_server` method enum and `ssl::stream` with coroutine-compatible `async_handshake` / `async_read_some` / `async_write_some`. A separate compilation unit (`asio_ssl.cpp` including `asio/ssl/impl/src.hpp`) is required because the project uses `ASIO_SEPARATE_COMPILATION`. The TLS context reload pattern uses `shared_ptr<ssl::context>` -- new connections atomically load the current pointer; existing connections hold their own shared_ptr keeping the old context alive until disconnect.

**Primary recommendation:** Use `asio::ssl::context(asio::ssl::context::tlsv13_server)` for TLS 1.3 only (cleanest API -- no need for `set_options` flag dance). Implement WS framing as a pure encode/decode layer in `relay/ws/`, with WsSession wrapping the existing `Session` send queue from `relay/core/`.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Auto-detect WS vs WSS from `cert_path`/`key_path` config fields. Fields omitted -> plain WS. Fields present -> must load successfully or relay refuses to start (error exit, no silent fallback to WS).
- **D-02:** Only `cert_path` + `key_path` added to RelayConfig. All transport settings (timeouts, frame sizes, ping intervals) are constexpr constants. Minimal config.
- **D-03:** Plain WS mode: startup warning encouraging TLS. No bind restriction. Blob data is already PQ-encrypted at application layer.
- **D-04:** WsSession (ws/) wraps Session (core/). WsSession owns TLS+WS stream and lifecycle. Session provides send queue + drain. do_send() calls back to WsSession::write_frame() via injected callback. Clean separation: ws/ handles framing, core/ handles queuing.
- **D-05:** Separate SessionManager class (not embedded in WsAcceptor). Owns `unordered_map<uint64_t, shared_ptr<WsSession>>` session map. Needed by Phase 104 for notification fan-out.
- **D-06:** Session added to SessionManager after WS upgrade completes. Failed TLS handshake or upgrade attempts are logged and dropped -- no session object created.
- **D-07:** Accept + reassemble fragmented client frames per RFC 6455 SS5.4. Reassemble up to max message size, reject if exceeded.
- **D-08:** Support both text (0x1) and binary (0x2) opcodes from Phase 101. Text for JSON messages (future Phase 102+), binary prepared for Phase 103 large blob data.
- **D-09:** Max frame sizes: 1 MiB text, 110 MiB binary. Text frames are for JSON control messages (small). Binary frames are for blob data (up to 100 MiB + overhead).
- **D-10:** Malformed frames (bad opcode, unmasked, oversized) -> Close(1002 Protocol Error), log at warn level, disconnect.
- **D-11:** Silently ignore WS extension requests (permessage-deflate etc.) -- omit Sec-WebSocket-Extensions from 101 response. Per RFC, omission means no extensions negotiated.
- **D-12:** Non-upgrade HTTP requests -> 426 Upgrade Required with `Upgrade: websocket` header.
- **D-13:** 5-second timeout for TLS handshake + WS upgrade (combined). Prevents slowloris-style resource exhaustion.
- **D-14:** 30-second idle timeout after WS upgrade completion (pre-auth placeholder). Phase 102 replaces this with auth deadline.
- **D-15:** Server-initiated WebSocket Ping every 30 seconds. Disconnect if no Pong within 60 seconds. Same pattern as node's bidirectional keepalive (Phase 83).
- **D-16:** RFC-compliant Close frame handling. Client Close -> echo with same status code, then TCP close. Relay-initiated Close -> send with appropriate status code, wait up to 5s for client echo, then TCP close.
- **D-17:** Hardcoded 1024 max concurrent connections (safety limit). Phase 102 SESS-03 makes this configurable.
- **D-18:** TLS 1.3 only. No TLS 1.2 or older.
- **D-19:** SIGHUP TLS reload via `shared_ptr<ssl::context>` swap. New connections get new context. Existing connections keep old context (ref-counted lifetime). Reload failure -> keep old context + log error.
- **D-20:** System OpenSSL 3.3+ (`find_package(OpenSSL 3.3 REQUIRED)`). Not FetchContent'd -- system package only.
- **D-21:** Thread pool: `std::thread::hardware_concurrency()` threads running `ioc.run()`. Per-connection strand via natural Asio coroutine chain (implicit serialization per session). Different clients run concurrently across threads.
- **D-22:** IPv4 and IPv6 support. Default bind `0.0.0.0`. Operator can use `::` for dual-stack.
- **D-23:** Construct SessionManager + WsAcceptor in main(), co_spawn accept loop. SIGHUP handler calls `acceptor.reload_tls()`. SIGTERM stops ioc. Thread pool runs ioc.run(). Same flat pattern as node's main().
- **D-24:** Keep reading from WebSocket always. Rely on send queue backpressure for response side.
- **D-25:** Best-effort Close(1001 Going Away) to active sessions on SIGTERM. Phase 105 (SESS-04) implements proper graceful shutdown with queue drain.
- **D-26:** Unit tests (Catch2) for WS framing logic: frame encode/decode, mask/unmask, fragmentation reassembly, upgrade handshake. Live testing against real node on dev machine.

### Claude's Discretion
- Internal class APIs for WsAcceptor, SessionManager, WsSession (as long as they follow decided patterns)
- WebSocket close status code selection for specific error scenarios
- Exact constexpr values for non-decided constants (e.g., fragment buffer size)
- Whether SIGTERM Close(1001) is worth the complexity in Phase 101 or deferred to Phase 105

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| TRANS-01 | Relay accepts WebSocket Secure (WSS) connections with TLS cert/key from config | Asio `ssl::context(tlsv13_server)` + `ssl::stream` pattern verified. OpenSSL 3.6.2 available. Config extension for `cert_path`/`key_path` straightforward. |
| TRANS-02 | Relay accepts plain WebSocket (WS) connections for local dev/testing | Auto-detect from config: omit cert/key fields -> WS mode. Same acceptor, skip TLS handshake step. |
| TRANS-03 | Hand-rolled RFC 6455 WebSocket framing (upgrade handshake, text/binary frames, ping/pong/close) | RFC 6455 server subset fully specified. SHA-1 + Base64 from OpenSSL EVP API. Frame encode/decode ~200 lines. Existing metrics_collector.cpp HTTP parsing as reference. |
| TRANS-04 | SIGHUP-reloadable TLS context via atomic ssl::context swap | `shared_ptr<ssl::context>` swap pattern. SIGHUP handler already wired in relay_main.cpp. New connections load atomically; existing connections ref-count old context. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| OpenSSL | 3.3+ (system, verified 3.6.2) | TLS 1.3 termination, SHA-1/Base64 for WS handshake | System package, Asio SSL built on it, already on every Linux server. `find_package(OpenSSL 3.3 REQUIRED)`. |
| Standalone Asio | 1.38.0 (existing) | `ssl::context`, `ssl::stream`, coroutine async IO | Already in project. `tlsv13_server` enum confirmed in `context_base.hpp`. |
| spdlog | 1.15.1 (existing) | Structured logging | Already in project. |
| nlohmann/json | 3.11.3 (existing) | Config file parsing (cert_path/key_path fields) | Already in project. |
| Catch2 | 3.7.1 (existing) | Unit tests for WS framing | Already in project. |

### New vs Existing
No new FetchContent dependencies. Only addition: `find_package(OpenSSL 3.3 REQUIRED)` for system OpenSSL.

**OpenSSL is linked ONLY to the relay target**, not to db/. The database binary remains OpenSSL-free.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Hand-rolled WS (~200 lines) | Boost.Beast | Requires Boost.Asio namespace, incompatible with standalone Asio. Would touch 34K LOC of frozen db/ code. |
| Hand-rolled WS | WebSocket++ | Broken with Asio 1.28+ executor changes (issue #794). C++11 callback-based, not coroutine-native. |
| `tlsv13_server` context method | `tls_server` + `set_options(no_tlsv1 | no_tlsv1_1 | no_tlsv1_2)` | Both work. `tlsv13_server` is cleaner -- single enum value, no flag dance. |
| `shared_ptr<ssl::context>` swap | `SSL_CTX_set_min_proto_version` at runtime | `shared_ptr` swap is the standard Asio pattern for cert reload. Atomic load gives new connections the new context. |

**Installation:**
```bash
# System dependency (Arch Linux) -- already installed
sudo pacman -S openssl
# Verify: openssl version -> OpenSSL 3.6.2
# Verify: pkg-config --modversion openssl -> 3.6.2
```

## Architecture Patterns

### Recommended Project Structure (Phase 101 additions)
```
relay/
  relay_main.cpp              # MODIFIED: thread pool, SessionManager, WsAcceptor, SIGHUP reload
  asio_ssl.cpp                # NEW: #include <asio/ssl/impl/src.hpp> (separate compilation)
  config/
    relay_config.h             # MODIFIED: add cert_path, key_path
    relay_config.cpp           # MODIFIED: load cert_path, key_path
  core/
    session.h                  # MODIFIED: inject write callback for WsSession
    session.cpp                # MODIFIED: do_send() calls injected callback
  ws/                          # NEW: all WebSocket transport code
    ws_frame.h                 # Frame encode/decode/mask/unmask, constants
    ws_frame.cpp
    ws_handshake.h             # HTTP upgrade parsing + 101 response generation
    ws_handshake.cpp
    ws_session.h               # Per-client WS lifecycle, TLS+WS stream, ping/pong, close
    ws_session.cpp
    ws_acceptor.h              # Accept loop, TLS context management, connection cap
    ws_acceptor.cpp
    session_manager.h          # Session map, lookup, fan-out support
    session_manager.cpp
  identity/                    # UNCHANGED
  tests/
    CMakeLists.txt             # MODIFIED: add new test files, link OpenSSL
    test_ws_frame.cpp          # NEW: frame encode/decode, mask/unmask
    test_ws_handshake.cpp      # NEW: upgrade request parsing, accept key computation
    test_session.cpp           # UNCHANGED
    test_relay_config.cpp      # MODIFIED: test cert_path/key_path loading
```

### Pattern 1: TLS 1.3 Context with `tlsv13_server`
**What:** Use `asio::ssl::context(asio::ssl::context::tlsv13_server)` for TLS 1.3 only enforcement.
**When to use:** WSS mode (cert_path + key_path present in config).
**Example:**
```cpp
// Source: Asio 1.38.0 context_base.hpp (verified in build/_deps)
auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv13_server);
ctx->use_certificate_chain_file(config.cert_path);
ctx->use_private_key_file(config.key_path, asio::ssl::context::pem);
```
**Why `tlsv13_server` instead of `tls_server` + options:** The `tlsv13_server` enum maps directly to `TLS_server_method()` with min/max version both set to TLS 1.3 internally. No need for `set_options(no_tlsv1 | no_tlsv1_1 | no_tlsv1_2)` which is the legacy approach. Cleaner, less error-prone.

### Pattern 2: SIGHUP TLS Reload via shared_ptr Swap
**What:** Store `shared_ptr<ssl::context>` in WsAcceptor. On SIGHUP, create a new context, load new cert/key, atomically store. New connections `shared_ptr<ssl::context>` copy from WsAcceptor. Existing connections hold their own shared_ptr, keeping old context alive via ref-counting.
**When to use:** TRANS-04 -- certificate rotation without relay restart.
**Example:**
```cpp
class WsAcceptor {
    std::shared_ptr<asio::ssl::context> tls_ctx_;  // atomic load for new connections

    bool reload_tls(const std::string& cert_path, const std::string& key_path) {
        try {
            auto new_ctx = std::make_shared<asio::ssl::context>(
                asio::ssl::context::tlsv13_server);
            new_ctx->use_certificate_chain_file(cert_path);
            new_ctx->use_private_key_file(key_path, asio::ssl::context::pem);
            // Atomic swap -- safe because SIGHUP handler runs on ioc strand
            tls_ctx_ = std::move(new_ctx);
            spdlog::info("TLS context reloaded");
            return true;
        } catch (const std::exception& e) {
            spdlog::error("TLS reload failed: {}", e.what());
            return false;  // Keep old context
        }
    }
};
```
**Thread safety note:** Because SIGHUP is handled via `asio::signal_set` which dispatches on the io_context, and the accept loop also runs on the io_context, the pointer swap and read are serialized by the implicit strand of the coroutine chain. No `std::atomic<shared_ptr>` needed if both run on the same io_context. However, with D-21 thread pool (`hardware_concurrency()` threads), the accept loop and SIGHUP handler could run on different threads. The safe approach is `std::atomic<std::shared_ptr<...>>` (C++20) or a mutex protecting the pointer.

### Pattern 3: Asio SSL Separate Compilation
**What:** The project uses `ASIO_SEPARATE_COMPILATION`. The asio.cmake wrapper compiles `asio/impl/src.hpp` but NOT `asio/ssl/impl/src.hpp`. The relay must provide its own SSL compilation unit.
**When to use:** Any relay source file that includes `<asio/ssl.hpp>`.
**Example:**
```cpp
// relay/asio_ssl.cpp -- compile ONCE in relay target
#include <asio/ssl/impl/src.hpp>
```
**Critical detail:** Without this file, you get linker errors for all SSL symbols (`asio::ssl::context::*`, `asio::ssl::stream::*`). This is the most common build failure when adding SSL to a standalone Asio project.

### Pattern 4: WsSession Wraps Session via Callback Injection
**What:** WsSession (ws/) owns the TLS/WS stream. Session (core/) owns the send queue. Session's `do_send()` calls back to WsSession's `write_frame()` via an injected `std::function` callback. This separates transport framing from queue management.
**When to use:** D-04 architecture decision.
**Example:**
```cpp
// Session modification: inject write callback
class Session {
public:
    using WriteCallback = std::function<asio::awaitable<bool>(const std::string&)>;

    void set_write_callback(WriteCallback cb) { write_cb_ = std::move(cb); }

private:
    asio::awaitable<bool> do_send(const std::string& data) {
        if (write_cb_) co_return co_await write_cb_(data);
        // Fallback: test mode (append to delivered_)
        delivered_.push_back(data);
        co_return true;
    }
    WriteCallback write_cb_;
};

// WsSession: inject its own write_frame as the callback
class WsSession {
    void setup() {
        session_.set_write_callback(
            [this](const std::string& data) -> asio::awaitable<bool> {
                return write_text_frame(data);
            });
    }
};
```

### Pattern 5: WebSocket Upgrade Handshake
**What:** After TLS handshake, parse HTTP/1.1 upgrade request, validate required headers, compute `Sec-WebSocket-Accept`, respond with 101.
**When to use:** Every new WSS/WS connection.
**Example:**
```cpp
// Sec-WebSocket-Accept computation using OpenSSL EVP API
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

std::string compute_accept_key(std::string_view client_key) {
    static constexpr auto MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string input(client_key);
    input += MAGIC;

    // SHA-1 hash
    unsigned char hash[20];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    // Base64 encode
    // EVP_EncodeBlock outputs null-terminated base64 string
    // Output size: 4 * ceil(20/3) = 28 chars + null
    char b64[29];
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64), hash, 20);
    return std::string(b64);
}
```
**Reference:** Existing HTTP parsing pattern in `db/peer/metrics_collector.cpp` lines 183-248 (read headers until `\r\n\r\n`, parse first line, generate response).

### Anti-Patterns to Avoid
- **Boost.Beast or any Boost dependency:** Requires Boost.Asio namespace, incompatible with standalone Asio. This is a hard constraint.
- **Per-connection thread:** Use coroutines on shared io_context. Threads are for io_context parallelism (D-21), not per-connection.
- **Blocking SSL operations:** All SSL operations must go through `ssl::stream::async_*` with `use_awaitable`. Synchronous `handshake()` blocks the thread.
- **Forgetting `asio_ssl.cpp`:** Missing the SSL separate compilation unit causes cryptic linker errors.
- **Using `std::atomic<shared_ptr>` for context swap when not needed:** If SIGHUP handler and accept loop can prove they run on the same strand, a plain `shared_ptr` suffices. With thread pool, either use `std::atomic<std::shared_ptr>` (C++20) or dispatch the reload to the accept coroutine's strand.
- **Parsing HTTP body for upgrade:** WS upgrade has no body. Stop reading after `\r\n\r\n` in headers.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| TLS termination | Custom TLS state machine | `asio::ssl::stream<tcp::socket>` | OpenSSL integration battle-tested. Asio wraps it cleanly with coroutine support. |
| SHA-1 for WS handshake | Custom SHA-1 | OpenSSL `EVP_sha1()` | SHA-1 is only used for RFC 6455 accept key (not security-critical). OpenSSL already linked for TLS. |
| Base64 for WS handshake | Custom Base64 encoder | OpenSSL `EVP_EncodeBlock()` | 20-byte hash -> 28-char base64. One function call. Already linked. |
| Send queue + backpressure | Custom queue in WsSession | Existing `Session` class (core/) | Already implemented and tested in Phase 100. Wrap via callback injection. |
| Config file parsing | Custom JSON parser | nlohmann/json (existing) | Already used in relay_config.cpp. Just add two fields. |

**Key insight:** Phase 101 adds exactly ONE new external dependency (system OpenSSL). Everything else reuses existing project infrastructure. The hand-rolled parts (WS framing, HTTP upgrade) are protocol-level logic that is simpler to implement correctly than to adapt a library with incompatible Asio namespace requirements.

## Common Pitfalls

### Pitfall 1: Missing Asio SSL Separate Compilation Unit
**What goes wrong:** Linker errors like "undefined reference to `asio::ssl::context::context(method)`" even though `<asio/ssl.hpp>` compiles fine.
**Why it happens:** Project defines `ASIO_SEPARATE_COMPILATION`. The asio.cmake wrapper includes `asio/impl/src.hpp` but NOT `asio/ssl/impl/src.hpp`. SSL symbols are never compiled.
**How to avoid:** Create `relay/asio_ssl.cpp` containing `#include <asio/ssl/impl/src.hpp>`. Add it to the relay library's source list in CMakeLists.txt.
**Warning signs:** Linker errors mentioning `asio::ssl::*` symbols.

### Pitfall 2: Thread Safety of shared_ptr TLS Context Swap
**What goes wrong:** Data race between SIGHUP reload writing `tls_ctx_` and accept loop reading it.
**Why it happens:** D-21 specifies `hardware_concurrency()` threads running `ioc.run()`. The SIGHUP handler and accept loop may execute on different threads.
**How to avoid:** Either (a) use `std::atomic<std::shared_ptr<ssl::context>>` (C++20, clean but requires GCC 12+ / Clang 16+), or (b) `asio::post()` the reload onto the acceptor's strand so it serializes with the accept loop, or (c) protect with a mutex.
**Warning signs:** Rare crashes on SIGHUP under load. TSAN reports.

### Pitfall 3: WebSocket Client Frames MUST Be Masked
**What goes wrong:** Server reads client frame, doesn't unmask payload, gets garbage data.
**Why it happens:** RFC 6455 requires all client-to-server frames to have MASK bit set with a 4-byte masking key. Forgetting to XOR-unmask the payload.
**How to avoid:** Always check MASK bit. If MASK=0 from client, send Close(1002) and disconnect. After reading mask key, XOR each payload byte with `mask[i % 4]`.
**Warning signs:** JSON parse errors on seemingly valid messages. Binary data corruption.

### Pitfall 4: Server Frames MUST NOT Be Masked
**What goes wrong:** Some WebSocket implementations reject server frames with MASK=1.
**Why it happens:** RFC 6455 Section 5.1: "A server MUST NOT mask any frames that it sends to the client."
**How to avoid:** Always set MASK bit to 0 in server-to-client frames. Never include a masking key.
**Warning signs:** Client-side frame parsing errors.

### Pitfall 5: Control Frames Can Interrupt Fragmented Messages
**What goes wrong:** Server receives a Ping in the middle of a fragmented data message and either crashes or breaks reassembly state.
**Why it happens:** RFC 6455 Section 5.4: "Control frames MAY be injected in the middle of a fragmented message."
**How to avoid:** When reassembling fragments, handle control frames (Ping/Pong/Close) inline without disturbing fragmentation state. Control frames themselves MUST NOT be fragmented.
**Warning signs:** Ping responses fail when large messages are being sent. Fragment reassembly produces corrupt output.

### Pitfall 6: WebSocket Frame Length Encoding Edge Cases
**What goes wrong:** Incorrectly reading 2-byte or 8-byte extended length fields.
**Why it happens:** Payload length field in byte 1 has three modes: 0-125 (actual length), 126 (next 2 bytes are BE uint16 length), 127 (next 8 bytes are BE uint64 length). Easy to get the offsets wrong, especially when the mask key follows the length.
**How to avoid:** Write unit tests for all three length encodings. Test boundary values: 125, 126, 127, 65535, 65536.
**Warning signs:** Frames larger than 125 bytes fail. Frames larger than 65535 bytes fail.

### Pitfall 7: TLS Handshake Timeout Not Enforced
**What goes wrong:** Slowloris attack: client connects TCP but never sends TLS ClientHello. Connection leaks resources.
**Why it happens:** Asio `ssl::stream::async_handshake` waits indefinitely by default.
**How to avoid:** Race the handshake against a `steady_timer(5s)` (D-13). On timeout, close the socket. Use `asio::experimental::make_parallel_group` or manual timer + cancel pattern.
**Warning signs:** Connection count grows without active sessions. Resource exhaustion under synthetic load.

### Pitfall 8: OpenSSL Version Mismatch with find_package
**What goes wrong:** `find_package(OpenSSL 3.3 REQUIRED)` fails on systems with OpenSSL 1.1.x.
**Why it happens:** CONTEXT.md locks to OpenSSL 3.3+ (D-20). Older distributions ship 1.1.x.
**How to avoid:** Document minimum OpenSSL 3.3 in build requirements. The build host has 3.6.2 so no issue for development. CI/deployment must verify.
**Warning signs:** CMake configure error at `find_package`.

## Code Examples

### WebSocket Frame Encode (Server-to-Client, Unmasked)
```cpp
// Source: RFC 6455 Section 5.2 + project conventions
std::string encode_frame(uint8_t opcode, std::span<const uint8_t> payload, bool fin = true) {
    std::string frame;
    frame.reserve(14 + payload.size());  // Max header + payload

    // Byte 0: FIN + opcode
    frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

    // Byte 1+: MASK=0 + payload length
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        uint16_t len = static_cast<uint16_t>(payload.size());
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
    }

    frame.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    return frame;
}
```

### WebSocket Frame Decode Header (Client-to-Server, Masked)
```cpp
// Source: RFC 6455 Section 5.2
struct FrameHeader {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t mask_key[4];
    size_t header_size;  // Total bytes consumed by header
};

// Parse from buffer. Returns nullopt if not enough bytes.
std::optional<FrameHeader> parse_frame_header(std::span<const uint8_t> buf) {
    if (buf.size() < 2) return std::nullopt;

    FrameHeader h{};
    h.fin = (buf[0] & 0x80) != 0;
    h.opcode = buf[0] & 0x0F;
    h.masked = (buf[1] & 0x80) != 0;
    uint8_t len7 = buf[1] & 0x7F;

    size_t offset = 2;
    if (len7 <= 125) {
        h.payload_length = len7;
    } else if (len7 == 126) {
        if (buf.size() < 4) return std::nullopt;
        h.payload_length = (static_cast<uint64_t>(buf[2]) << 8) | buf[3];
        offset = 4;
    } else {  // len7 == 127
        if (buf.size() < 10) return std::nullopt;
        h.payload_length = 0;
        for (int i = 0; i < 8; ++i) {
            h.payload_length = (h.payload_length << 8) | buf[2 + i];
        }
        offset = 10;
    }

    if (h.masked) {
        if (buf.size() < offset + 4) return std::nullopt;
        std::memcpy(h.mask_key, buf.data() + offset, 4);
        offset += 4;
    }

    h.header_size = offset;
    return h;
}
```

### Mask/Unmask Payload
```cpp
// Source: RFC 6455 Section 5.3
void apply_mask(std::span<uint8_t> payload, const uint8_t mask[4]) {
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] ^= mask[i % 4];
    }
}
```

### HTTP Upgrade Response
```cpp
// Source: RFC 6455 Section 4.2.2
std::string build_upgrade_response(const std::string& accept_key) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept_key + "\r\n"
           "\r\n";
}

std::string build_426_response() {
    return "HTTP/1.1 426 Upgrade Required\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Content-Length: 0\r\n"
           "\r\n";
}
```

### Accept Loop with TLS/Plain Dual Mode
```cpp
// Source: project pattern (db/peer/metrics_collector.cpp accept loop)
asio::awaitable<void> WsAcceptor::accept_loop() {
    while (!stopping_) {
        auto [ec, socket] = co_await acceptor_.async_accept(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;

        if (session_manager_.count() >= max_connections_) {
            spdlog::warn("connection rejected: max connections ({}) reached", max_connections_);
            socket.close();
            continue;
        }

        // Per-connection coroutine
        asio::co_spawn(socket.get_executor(),
            handle_new_connection(std::move(socket)),
            asio::detached);
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `sslv23_server` + disable flags | `tlsv13_server` enum | Asio 1.28+ (2023) | Cleaner API for TLS 1.3 only. No flag dance. |
| `SSL_CTX_set_options(NO_TLSv1...)` | `SSL_CTX_set_min_proto_version(TLS1_3_VERSION)` | OpenSSL 1.1.0+ | OpenSSL itself recommends version pinning over option flags. Asio `tlsv13_server` does this internally. |
| Callback-based Asio SSL | Coroutine-based `co_await async_handshake(use_awaitable)` | C++20 / Asio 1.24+ | Clean, linear code. Project already uses this pattern throughout db/. |
| Boost.Beast for WebSocket | Hand-rolled server-side WS | N/A (project constraint) | ~200 lines. No Boost dependency. Full coroutine integration. |

**Deprecated/outdated:**
- `sslv23_server` + option flags: Still works but `tlsv13_server` is cleaner for TLS 1.3 only.
- OpenSSL 1.1.x: D-20 requires 3.3+. OpenSSL 1.1.x is EOL (September 2023).

## Open Questions

1. **`std::function` vs template for Session write callback (D-04)**
   - What we know: Session needs to call WsSession's write_frame(). `std::function` is the obvious choice for runtime polymorphism.
   - What's unclear: `std::function` has heap allocation per invocation and type erasure overhead. For a hot path (every outbound message), this might matter at scale.
   - Recommendation: Use `std::function` initially. The overhead is negligible compared to TLS + WebSocket + TCP write latency. Optimize only if profiling shows otherwise. This is Claude's discretion per CONTEXT.md.

2. **SIGTERM Close(1001) in Phase 101 vs Phase 105**
   - What we know: D-25 says "best-effort Close(1001) Going Away" on SIGTERM. Phase 105 (SESS-04) does proper graceful shutdown with queue drain.
   - What's unclear: Is the "best-effort" version worth implementing in Phase 101? It means iterating all sessions and sending a close frame before ioc.stop(), with no queue drain.
   - Recommendation: Implement the best-effort version in Phase 101 (iterate SessionManager, send Close frame, short timer, then stop). It is a few lines of code and improves client experience. Phase 105 upgrades this to proper queue drain. This is Claude's discretion per CONTEXT.md.

3. **Thread safety pattern for `tls_ctx_` under D-21 thread pool**
   - What we know: SIGHUP handler runs on `ioc`. Accept loop runs on `ioc`. Both run on the thread pool.
   - What's unclear: Whether `asio::signal_set` guarantees the handler runs on a specific thread, or whether it can race with the accept loop.
   - Recommendation: Use `asio::post(strand_, ...)` to dispatch the TLS reload, where `strand_` is shared with the accept loop. Or simpler: just use a mutex around `tls_ctx_` read/write. The SIGHUP path is infrequent (human-initiated).

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| OpenSSL | TLS termination (TRANS-01, TRANS-04), WS handshake SHA-1/Base64 (TRANS-03) | Yes | 3.6.2 | -- (required, no fallback) |
| Standalone Asio | SSL stream, async IO | Yes | 1.38.0 (FetchContent) | -- |
| pkg-config | CMake find_package(OpenSSL) | Yes | -- | -- |
| Catch2 | Unit tests | Yes | 3.7.1 (FetchContent) | -- |

**Missing dependencies with no fallback:** None.
**Missing dependencies with fallback:** None.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 3.7.1 |
| Config file | `relay/tests/CMakeLists.txt` (exists) |
| Quick run command | `cd build && ctest --test-dir . -R relay -j4 --output-on-failure` |
| Full suite command | `cd build && ctest --test-dir . -R relay -j4 --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| TRANS-01 | WSS connections accepted with TLS cert/key | integration (live) | Manual: connect with `websocat wss://localhost:4201` | N/A -- live test |
| TRANS-02 | Plain WS connections accepted | integration (live) | Manual: connect with `websocat ws://localhost:4201` | N/A -- live test |
| TRANS-03 | RFC 6455 framing: frame encode/decode | unit | `ctest -R test_ws_frame` | Wave 0 |
| TRANS-03 | RFC 6455 framing: mask/unmask | unit | `ctest -R test_ws_frame` | Wave 0 |
| TRANS-03 | RFC 6455 framing: fragmentation reassembly | unit | `ctest -R test_ws_frame` | Wave 0 |
| TRANS-03 | RFC 6455 framing: upgrade handshake parsing | unit | `ctest -R test_ws_handshake` | Wave 0 |
| TRANS-03 | RFC 6455 framing: Sec-WebSocket-Accept key | unit | `ctest -R test_ws_handshake` | Wave 0 |
| TRANS-03 | Ping/pong, close frames | unit | `ctest -R test_ws_frame` | Wave 0 |
| TRANS-04 | SIGHUP reloads TLS cert/key | unit | `ctest -R test_ws_acceptor` or live test | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . && ctest --test-dir . -R relay -j4 --output-on-failure`
- **Per wave merge:** Same (no separate suite at this phase)
- **Phase gate:** All relay tests green + successful `websocat` connection over WSS and WS

### Wave 0 Gaps
- [ ] `relay/tests/test_ws_frame.cpp` -- covers TRANS-03 frame encode/decode, mask/unmask, length encoding, fragmentation
- [ ] `relay/tests/test_ws_handshake.cpp` -- covers TRANS-03 upgrade parsing, accept key computation, 426 response
- [ ] `relay/tests/CMakeLists.txt` update -- add new test files, link `OpenSSL::Crypto` for SHA-1 test dependency

## Sources

### Primary (HIGH confidence)
- Asio 1.38.0 `ssl/context_base.hpp` -- verified `tlsv13_server` enum, `no_tlsv1_2` option, all method types. Path: `build/_deps/asio-src/include/asio/ssl/context_base.hpp`
- Asio 1.38.0 `ssl/stream.hpp` -- verified `async_handshake`, `async_read_some` coroutine-compatible APIs. Path: `build/_deps/asio-src/include/asio/ssl/stream.hpp`
- Asio 1.38.0 `ssl/impl/src.hpp` -- verified separate compilation includes. Path: `build/_deps/asio-src/include/asio/ssl/impl/src.hpp`
- OpenSSL 3.6.2 system install -- verified via `openssl version`, `pkg-config --modversion openssl`, `find_package` test
- [RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455) -- WebSocket protocol specification. Server MUST requirements verified.
- `db/peer/metrics_collector.cpp` -- existing hand-rolled HTTP server pattern (accept loop, header parsing, response generation)
- `relay/core/session.h/cpp` -- existing send queue with drain coroutine (Phase 100 output)
- `relay/config/relay_config.h/cpp` -- existing config loader (to be extended)
- `relay/relay_main.cpp` -- existing main() with signal handling (to be extended)

### Secondary (MEDIUM confidence)
- [OpenSSL SSL_CTX_set_min_proto_version docs](https://docs.openssl.org/1.1.1/man3/SSL_CTX_set_min_proto_version/) -- TLS version pinning API
- [Asio SSL documentation](https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/ssl.html) -- SSL overview and patterns
- `.planning/research/ARCHITECTURE.md` -- project architecture research (pre-existing)
- `.planning/research/STACK.md` -- project stack research (pre-existing)

### Tertiary (LOW confidence)
- None -- all findings verified against local source code and system packages.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - OpenSSL 3.6.2 verified on system, Asio SSL headers verified in build tree, all APIs confirmed
- Architecture: HIGH - patterns directly derived from existing codebase (metrics_collector HTTP, session send queue) and locked CONTEXT.md decisions
- Pitfalls: HIGH - SSL separate compilation and masking requirements are well-documented, verified against Asio source and RFC 6455

**Research date:** 2026-04-09
**Valid until:** 2026-05-09 (stable -- RFC 6455 is final, OpenSSL 3.x stable, Asio 1.38.0 pinned)
