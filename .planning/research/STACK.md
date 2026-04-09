# Stack Research: Relay v2 (WebSocket/JSON/TLS Gateway)

**Domain:** WebSocket relay gateway translating JSON clients to FlatBuffers node over UDS
**Researched:** 2026-04-09
**Confidence:** HIGH (core stack is proven, additions are standard components)

## Context

Relay v2 replaces the old PQ-authenticated TCP relay with a WebSocket/JSON/TLS gateway. The database node (db/) is frozen. The relay must:

1. Accept WSS connections from clients (TLS + WebSocket)
2. Perform ML-DSA-87 challenge-response auth over WebSocket
3. Translate JSON messages to FlatBuffers binary and vice versa
4. Forward to the node via a single multiplexed UDS connection
5. Track subscriptions and route notifications

The existing C++20/Asio/coroutine stack is validated and stays. This document covers ONLY new additions.

## Critical Decision: WebSocket Library

### Recommendation: Write a minimal RFC 6455 server (no third-party WebSocket library)

**Why not Boost.Beast:**
- Boost.Beast requires Boost.Asio (boost:: namespace), NOT standalone Asio. This project uses standalone Asio (asio:: namespace) via OlivierLDff/asio.cmake with `ASIO_STANDALONE` defined. Switching to Boost.Asio would require changing every `asio::` reference in ~34,300 LOC of db/ code, which is frozen.
- The beast-asio-standalone fork (vimpunk) is explicitly discouraged by its maintainer: "I don't recommend that you use this since the update frequency is going to be smaller." Tests don't work. Unmaintained.

**Why not WebSocket++:**
- Has real compatibility issues with recent standalone Asio (GitHub issue #794: compilation failures with executor model changes in Asio 1.28+). The project uses Asio 1.38.0.
- CMake build still requires Boost for examples/tests (GitHub issue #1099: open, unresolved). Standalone Asio mode is second-class.
- Callback-based API, not coroutine-native. Would require wrapping everything in coroutine adapters.
- Header-only C++11 design -- not idiomatic with the project's C++20 coroutine patterns.

**Why not eidheim/Simple-WebSocket-Server:**
- C++11 callback-based, uses Boost.Asio, not standalone Asio coroutines.

**Why a hand-rolled implementation works here:**

The relay only needs SERVER-side WebSocket (no client). The server-side subset of RFC 6455 is small:

1. **HTTP Upgrade handshake** (~40 lines): Parse `Sec-WebSocket-Key`, respond with `SHA-1(key + magic)` Base64-encoded in `Sec-WebSocket-Accept`. OpenSSL (already needed for TLS) provides SHA-1 and Base64.
2. **Frame read** (~60 lines): Read 2-14 byte header, unmask client payload (XOR with 4-byte key). Clients MUST mask; server MUST NOT mask.
3. **Frame write** (~30 lines): Write unmasked frames to client. Text frames (opcode 0x1) for JSON messages.
4. **Control frames** (~20 lines): Handle Ping (respond Pong), Pong (ignore), Close (echo close frame).

Total: ~150 lines of protocol logic, fully under project control, coroutine-native, zero dependency risk. The existing codebase already has hand-rolled binary protocol framing (db/net/framing.h) and a hand-rolled HTTP server (db/peer/metrics_collector.cpp Prometheus endpoint), so this pattern is proven.

**Confidence:** HIGH -- RFC 6455 server-side is well-specified, the codebase has precedent for hand-rolled protocols, and all third-party options have disqualifying compatibility issues.

## Recommended Stack Additions

### New System Dependencies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| OpenSSL | 3.x (system) | TLS termination for WSS, SHA-1+Base64 for WS handshake | Industry standard, Asio SSL built on it, already on every Linux server. System package, not FetchContent (too large, tightly coupled to OS). |

### Existing Technologies (Already Available, New Usage)

| Technology | Current Usage | New Usage in Relay v2 |
|------------|---------------|----------------------|
| Standalone Asio 1.38.0 | TCP networking, UDS, C++20 coroutines | `asio::ssl::context` + `asio::ssl::stream` for TLS, WebSocket frame IO over TLS stream |
| nlohmann/json 3.11.3 | Config file parsing | JSON message serialization/deserialization for all 38 client-allowed message types |
| FlatBuffers 25.2.10 | Node wire format | Relay-side encoding/decoding for JSON-to-FlatBuffers translation |
| liboqs 0.15.0 | ML-DSA-87, ML-KEM-1024 | ML-DSA-87 challenge-response auth verification (verify only, no signing by relay) |
| spdlog 1.15.1 | Structured logging | Relay logging |
| Catch2 | Unit testing | Relay unit tests |

### No New FetchContent Dependencies

The relay v2 requires ZERO new FetchContent dependencies. Everything it needs is either already fetched (Asio, nlohmann/json, FlatBuffers, liboqs, spdlog, Catch2) or is a system package (OpenSSL).

## Detailed Component Analysis

### 1. TLS Termination (Asio SSL)

**How it works:** Standalone Asio includes `asio/ssl.hpp` which wraps OpenSSL. The headers are confirmed present at `asio/ssl/context.hpp`, `asio/ssl/stream.hpp`, etc. in the project's fetched Asio 1.38.0 source.

**Pattern:**

```cpp
#include <asio/ssl.hpp>

// Setup (once at relay startup)
asio::ssl::context ssl_ctx(asio::ssl::context::tls_server);
ssl_ctx.set_options(asio::ssl::context::default_workarounds |
                    asio::ssl::context::no_sslv2 |
                    asio::ssl::context::no_sslv3 |
                    asio::ssl::context::no_tlsv1 |
                    asio::ssl::context::no_tlsv1_1);
ssl_ctx.use_certificate_chain_file(cert_path);
ssl_ctx.use_private_key_file(key_path, asio::ssl::context::pem);

// Per-connection: wrap accepted TCP socket in SSL stream
asio::ssl::stream<asio::ip::tcp::socket> ssl_socket(std::move(tcp_socket), ssl_ctx);
co_await ssl_socket.async_handshake(asio::ssl::stream_base::server,
                                      asio::use_awaitable);
// Then do WebSocket upgrade over ssl_socket
```

**Separate compilation requirement:** The asio.cmake wrapper's `asio.cpp` only includes `asio/impl/src.hpp`, NOT `asio/ssl/impl/src.hpp`. The relay must provide its own SSL compilation unit:

```cpp
// relay2/asio_ssl.cpp
#include <asio/ssl/impl/src.hpp>
```

This ensures SSL symbols are compiled only for the relay binary, keeping the db binary free of OpenSSL dependency.

**Config additions to RelayConfig:**
```json
{
  "cert_path": "/etc/chromatindb/relay.crt",
  "key_path": "/etc/chromatindb/relay.key"
}
```

**Gotcha -- ASIO_SEPARATE_COMPILATION:** The project defines `ASIO_SEPARATE_COMPILATION` (set by asio.cmake). When this is defined, Asio expects implementation code in exactly one translation unit. For SSL, this means `asio/ssl/impl/src.hpp` must be included in exactly one .cpp file in the relay, and any .cpp file using SSL headers must also see `ASIO_SEPARATE_COMPILATION` defined (which it will, since it's a PUBLIC compile definition on the asio target).

**Confidence:** HIGH -- Asio SSL is mature, well-documented, and pattern is verified against source.

### 2. WebSocket Protocol (Hand-Rolled, ~150 Lines)

**Upgrade handshake (over TLS stream):** After TLS handshake completes, the client sends a standard HTTP/1.1 upgrade request. The relay must:

1. Read HTTP request line + headers (same pattern as existing Prometheus endpoint in `metrics_collector.cpp`)
2. Validate: `Connection: Upgrade`, `Upgrade: websocket`, `Sec-WebSocket-Version: 13`, presence of `Sec-WebSocket-Key`
3. Compute accept: `Base64(SHA-1(Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`
4. Respond with `HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: <accept>\r\n\r\n`

OpenSSL provides both SHA-1 (`EVP_sha1`) and Base64 (`EVP_EncodeBlock`) -- no additional deps needed.

**Frame format (server reads from client -- always masked):**
```
Byte 0: [FIN:1][RSV1:1][RSV2:1][RSV3:1][Opcode:4]
Byte 1: [MASK:1][PayloadLen:7]
  If PayloadLen == 126: next 2 bytes = BE uint16 actual length
  If PayloadLen == 127: next 8 bytes = BE uint64 actual length
Next 4 bytes: masking key (MUST be present for client-to-server)
Remaining: masked payload (XOR each byte with mask[i % 4])
```

**Frame format (server writes to client -- never masked):**
```
Byte 0: [FIN:1][RSV1:1][RSV2:1][RSV3:1][Opcode:4]  -- FIN=1 always (no fragmentation)
Byte 1: [0:1][PayloadLen:7]  -- MASK bit = 0
  If PayloadLen == 126: next 2 bytes = BE uint16 actual length
  If PayloadLen == 127: next 8 bytes = BE uint64 actual length
Remaining: unmasked payload
```

**Opcodes to implement:**
- `0x1` (Text): JSON messages -- all application data
- `0x8` (Close): Connection teardown with status code
- `0x9` (Ping): Auto-respond with Pong (same payload)
- `0xA` (Pong): Ignore (unsolicited keepalive ack from client)

**Deliberately NOT implementing:**
- `0x2` (Binary frames): Not needed -- all messages are JSON text
- Fragmented messages (FIN=0 continuation frames): JSON messages are complete, no fragmentation needed
- Per-message deflate (RSV1 extension): JSON messages are small, TLS handles compression
- Subprotocol negotiation: Single protocol, no negotiation needed

**Maximum frame size:** Cap at 110 MiB (matching `MAX_FRAME_SIZE` in db/net/framing.h) to handle large blob Data messages. In practice, JSON-encoded blob data (hex strings) will be roughly double the binary size, so the actual JSON frame limit should be ~220 MiB.

**Confidence:** HIGH -- server-side only, no fragmentation, no extensions, well-specified subset.

### 3. JSON Message Format (nlohmann/json)

**Translation layer architecture:** The relay translates between JSON (client-facing) and the node's FlatBuffers binary wire format. Two translation paths exist:

**Path A -- Blob messages (types 8, 17, 32, 54):** These carry FlatBuffers-encoded `Blob` payloads. Translation uses `wire::encode_blob()` / `wire::decode_blob()` from `db/wire/codec.h`.

**Path B -- Binary-struct messages (all other 34 types):** These carry hand-packed binary payloads with fields at fixed offsets (documented byte-by-byte in PROTOCOL.md). Translation is field-by-field pack/unpack.

**JSON message envelope:**
```json
{
  "type": "ReadRequest",
  "request_id": 42,
  "payload": { ... type-specific fields ... }
}
```

**Binary field encoding in JSON:** All byte arrays (namespace_id, blob_hash, pubkey, signature, data) use hex encoding. This matches the existing codebase convention (`db/util/hex.h` provides `to_hex`/`from_hex`).

**Translation layer structure:**

```cpp
// json_codec.h
namespace chromatindb::relay2::json {

// Client JSON -> node binary
std::vector<uint8_t> json_to_payload(wire::TransportMsgType type,
                                      const nlohmann::json& j);

// Node binary -> client JSON
nlohmann::json payload_to_json(wire::TransportMsgType type,
                                std::span<const uint8_t> payload);

// Type string -> enum
std::optional<wire::TransportMsgType> type_from_string(std::string_view name);

// Enum -> type string
std::string_view type_to_string(wire::TransportMsgType type);

} // namespace chromatindb::relay2::json
```

**Example translations:**

ReadRequest:
```json
{"type":"ReadRequest","request_id":1,"payload":{"namespace":"a1b2...","hash":"d4e5..."}}
```
Binary: 64 bytes = `[namespace_id:32][blob_hash:32]`

Data (blob write):
```json
{"type":"Data","request_id":2,"payload":{"namespace":"a1b2...","pubkey":"...","data":"...","ttl":86400,"timestamp":1712678400,"signature":"..."}}
```
Binary: FlatBuffers-encoded Blob via `wire::encode_blob()`

Subscribe:
```json
{"type":"Subscribe","request_id":3,"payload":{"namespace":"a1b2..."}}
```
Binary: 32 bytes = `[namespace_id:32]`

Notification (node->client):
```json
{"type":"Notification","payload":{"namespace":"a1b2...","data":"..."}}
```
Binary: `[namespace_id:32][blob_data:variable]`

**Performance note:** nlohmann/json is not the fastest JSON library, but relay message rates are bounded by WebSocket + TLS overhead and node processing speed. JSON parsing will not be the bottleneck. Hex encoding doubles the size of binary data, but the tradeoff (human-readable messages, any language can participate) is the entire point of Relay v2.

**Confidence:** HIGH -- nlohmann/json is already in the project, hex encoding is a proven pattern, message formats are fully specified in PROTOCOL.md.

### 4. Challenge-Response Auth

**Flow:**
1. Client connects WSS, completes TLS handshake
2. Client sends WebSocket upgrade, relay accepts (101)
3. Relay generates 32-byte random challenge via `randombytes_buf()` (libsodium), sends: `{"type":"AuthChallenge","challenge":"...hex..."}`
4. Client signs challenge with ML-DSA-87, responds: `{"type":"AuthResponse","pubkey":"...hex...","signature":"...hex..."}`
5. Relay verifies signature using `OQS_SIG_verify` (liboqs, already linked)
6. Relay derives namespace = `SHA3-256(pubkey)` (existing `crypto::sha3_256`)
7. Session authenticated, relay forwards to node via UDS with TrustedHello (node trusts relay)

**No new dependencies.** liboqs verify + libsodium randombytes + SHA3-256 are all already available via chromatindb_lib.

**Confidence:** HIGH -- all crypto primitives already used throughout the codebase.

### 5. Multiplexed UDS Connection

**Architecture change from old relay:** The old relay creates one UDS connection per client. Relay v2 uses a SINGLE UDS connection to the node, multiplexing all client traffic.

**Multiplexing via request_id remapping:**
- Relay maintains `relay_request_id -> (client_session, original_request_id)` map
- Each client request is assigned a relay-global `request_id` before forwarding
- Node responses are demultiplexed back to the correct client session
- Atomic uint32 counter for relay-global request_id generation

**Subscription routing:**
- Relay tracks `namespace -> set<client_session>` for Subscribe/Unsubscribe
- Node sends Notification (type 21) with first 32 bytes = namespace_id
- Relay extracts namespace, looks up subscribed sessions, sends JSON notification to each
- Maximum 256 subscriptions per client (matching existing relay limit)

**UDS reconnection:** Single-connection multiplexing means UDS disconnect affects ALL clients. Relay should:
- Queue client messages during brief UDS reconnect attempts (jittered backoff, matching existing pattern)
- Disconnect all clients if UDS cannot be restored within timeout
- This is simpler than old relay's per-session reconnect logic

**Uses existing Asio UDS support** (`asio::local::stream_protocol::socket`) and existing Connection class patterns.

**Confidence:** HIGH -- UDS already used, multiplexing is standard.

## CMake Integration

```cmake
# In root CMakeLists.txt (new relay2 subdirectory):
find_package(OpenSSL REQUIRED)

add_subdirectory(relay2)

# relay2/CMakeLists.txt:
add_library(chromatindb_relay2_lib STATIC
  asio_ssl.cpp         # asio/ssl/impl/src.hpp (separate compilation unit)
  config/config.cpp
  ws/handshake.cpp     # HTTP upgrade + WebSocket accept
  ws/frame.cpp         # RFC 6455 frame read/write
  ws/session.cpp       # Per-client WebSocket session lifecycle
  json/codec.cpp       # JSON <-> FlatBuffers translation (38 types)
  auth/challenge.cpp   # ML-DSA-87 challenge-response verify
  core/relay.cpp       # Main relay logic, UDS multiplexing, subscription routing
)

target_include_directories(chromatindb_relay2_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_link_libraries(chromatindb_relay2_lib PUBLIC
  chromatindb_lib       # Reuse db/ for FlatBuffers codec, crypto, hex, identity
  OpenSSL::SSL
  OpenSSL::Crypto
)
```

**What chromatindb_lib provides (no duplication):**
- `TransportCodec::encode/decode` (FlatBuffers transport envelope)
- `wire::encode_blob / decode_blob` (blob FlatBuffers codec)
- `wire::TransportMsgType` enum and `is_client_allowed` filter logic
- `crypto::sha3_256` (namespace derivation from pubkey)
- `util::to_hex / from_hex` (hex encoding for JSON)
- `identity::NodeIdentity` (relay's own signing identity)
- `net::Connection` patterns (UDS connection to node)

**OpenSSL is linked ONLY to the relay target**, not to the db target. The db binary remains OpenSSL-free.

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| Hand-rolled WS (~150 lines) | Boost.Beast | Requires Boost.Asio namespace. Incompatible with standalone Asio. Would touch 34K LOC of frozen db/ code. |
| Hand-rolled WS | beast-asio-standalone fork | Maintainer discourages use, tests broken, infrequent updates. |
| Hand-rolled WS | WebSocket++ 0.8.2 | Broken with Asio 1.28+ executor changes (issue #794), callback-based, Boost needed for CMake. |
| Hand-rolled WS | eidheim/Simple-WebSocket-Server | Uses Boost.Asio, C++11 callbacks, not coroutine-native. |
| System OpenSSL (find_package) | FetchContent OpenSSL | OpenSSL is too large and OS-coupled for FetchContent. System package is correct. |
| nlohmann/json (existing) | simdjson / RapidJSON | Already in project, adequate for relay message rates. YAGNI. |
| Single multiplexed UDS | Per-client UDS (old pattern) | Wasteful -- each client opens separate node connection. Single connection with request_id mux is cleaner. |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| Boost (any component) | Would contaminate standalone Asio codebase, massive dependency | Standalone Asio + hand-rolled WS |
| libwebsockets | C library, callback-based, poor C++ integration | Hand-rolled WS |
| uWebSockets | Brings its own event loop (us_loop), conflicts with Asio io_context | Hand-rolled WS |
| Per-message deflate (WS extension) | JSON messages are small, adds complexity, TLS handles transport | Plain text frames |
| Binary WS frames for JSON | Text frames (opcode 0x1) are correct for JSON per RFC 6455 | Text frames |
| WebSocket subprotocol negotiation | Single protocol, no negotiation needed | Fixed JSON protocol |
| OpenSSL in db/ target | Database node must remain OpenSSL-free (libsodium + liboqs only) | Link OpenSSL only to relay2 target |
| PQ-authenticated transport (old pattern) | TLS is sufficient -- payloads are already PQ-signed blobs + PQ-encrypted envelopes | Standard TLS + challenge-response auth |

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| Standalone Asio 1.38.0 | OpenSSL 3.x | Asio SSL headers work with OpenSSL 1.1.x and 3.x. System OpenSSL 3.6.2 confirmed on build host. |
| Standalone Asio 1.38.0 | `ASIO_SEPARATE_COMPILATION` | SSL needs its own `asio/ssl/impl/src.hpp` inclusion in one relay .cpp file. |
| nlohmann/json 3.11.3 | C++20 | Full C++20 support, no issues. |
| FlatBuffers 25.2.10 | Relay translation layer | Relay calls existing `encode_blob`/`decode_blob` + `TransportCodec`. No schema changes. |
| liboqs 0.15.0 | Challenge-response auth | `OQS_SIG_verify` for ML-DSA-87. Already proven in db/ code. |
| OpenSSL 3.6.2 (system) | Asio 1.38.0 SSL | Confirmed compatible. OpenSSL 3.x API used by Asio SSL. |

## Installation

```bash
# System dependency (Arch Linux)
sudo pacman -S openssl

# No pip/npm/new FetchContent deps needed.
# Everything else is already in CMakeLists.txt via FetchContent.
```

## Sources

- [Boost.Beast FAQ on standalone Asio](https://live.boost.org/doc/libs/1_87_0/libs/beast/doc/html/beast/design_choices/faq.html) -- confirms Beast requires Boost, not standalone Asio
- [beast-asio-standalone fork](https://github.com/vimpunk/beast-asio-standalone) -- maintainer discourages use
- [WebSocket++ standalone Asio compilation failure (issue #794)](https://github.com/zaphoyd/websocketpp/issues/794) -- broken with modern Asio executor model
- [WebSocket++ CMake Boost dependency (issue #1099)](https://github.com/zaphoyd/websocketpp/issues/1099) -- open, unresolved
- [OlivierLDff/asio.cmake](https://github.com/OlivierLDff/asio.cmake) -- CMake wrapper used by project, no SSL handling
- [Standalone Asio SSL headers](https://github.com/chriskohlhoff/asio) -- full SSL support confirmed in asio/ssl/ directory
- [Asio SSL documentation](https://dens.website/tutorials/cpp-asio/ssl-tls) -- TLS server pattern
- [RFC 6455 WebSocket Protocol](https://datatracker.ietf.org/doc/html/rfc6455) -- server-side subset specification
- [Boost.Beast vs WebSocket++ comparison](https://www.boost.org/doc/libs/1_86_0/libs/beast/doc/html/beast/design_choices/comparison_to_zaphoyd_studios_we.html) -- design differences

---
*Stack research for: Relay v2 WebSocket/JSON/TLS Gateway*
*Researched: 2026-04-09*
