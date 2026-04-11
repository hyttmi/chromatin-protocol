# Phase 107: Message Type Verification - Research

**Researched:** 2026-04-11
**Domain:** End-to-end WebSocket relay message type verification (C++20, JSON/binary translation, ML-DSA-87 blob signing)
**Confidence:** HIGH

## Summary

Phase 107 extends the existing `tools/relay_smoke_test.cpp` (currently 13 tests) to verify all 38 relay-allowed message types translate correctly through the full relay pipeline (JSON -> binary -> node -> binary -> JSON). The relay code is already built and the 13-test smoke test already validates connectivity, auth, and 7 compound query types. This phase adds the remaining 25 tests, with the major new capability being signed blob production in the smoke test tool to enable Data(8) write and downstream read/delete/batch paths.

The 38 types break down into: 16 request/response pairs (32 types), 2 relay-intercepted (subscribe/unsubscribe), and 4 fire-and-forget/special (ping, pong, goodbye, notification). Each request/response pair covers both the client-send type and the node-response type, so a single test exercises 2 types at once.

**Primary recommendation:** Extend relay_smoke_test.cpp with blob signing (~50 lines using oqs/sha3.h + RelayIdentity::sign()), add a ws_recv_frame() that handles both text (opcode 0x01) and binary (opcode 0x02) WebSocket frames, then add tests for the remaining 16 request types plus subscribe/unsubscribe verification. Keep the same blocking-socket single-binary pattern.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Extend the existing `tools/relay_smoke_test.cpp` with all remaining message types. One binary, one run, all 38 types verified. Already has auth, WS framing, and result tracking infrastructure.
- **D-02:** Do NOT create a separate E2E test binary or Catch2 integration test. Keep it simple -- one tool that does everything.
- **D-03:** Build ML-DSA-87 blob signing + FlatBuffer encoding directly into the smoke test. The tool already loads a RelayIdentity key -- add SHA3-256(namespace||data||ttl||timestamp) hash, ML-DSA-87 sign, FlatBuffer encode. ~50 lines of signing code.
- **D-04:** This enables testing Data(8) write -> WriteAck(30), ReadRequest(31) -> ReadResponse(32), Delete(17) -> DeleteAck(18), and BatchReadRequest(53) -> BatchReadResponse(54) paths that Phase 106 had to skip.
- **D-05:** Test 3-4 core error paths to prove the relay translates errors correctly: read nonexistent blob, stats for nonexistent namespace, metadata for nonexistent hash, and one malformed request. Not exhaustive -- enough to prove the pattern works.
- **D-06:** Each error response must have a "type" field and structured error information in JSON.
- **D-07:** Extend the existing `/tmp/chromatindb-test/run-smoke.sh` script to start node+relay, run the extended smoke test, and dump all logs. Same one-command workflow used for Phase 106 live testing.
- **D-08:** User runs one script, gets full results. Claude reads the logs afterward.

### Claude's Discretion
- Grouping order of the 38 types within the test (by category, by wire type number, etc.)
- Exact FlatBuffer encoding approach for signed blobs (reuse relay lib or build minimal encoder)
- Specific error messages to validate beyond structural correctness
- Whether to add timing/performance annotations to the test output

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| E2E-01 | All 38 relay-allowed message types translate correctly through relay->node->relay with live node | Full type categorization below; blob signing approach for Data(8); binary WS frame handling for ReadResponse/BatchReadResponse; request/response field validation specs for all 16 pairs + 2 intercepted + 4 special types |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| chromatindb_relay_lib | (in-tree) | Relay WS frame encode/decode, hex/base64 utils, RelayIdentity, blob_codec, type_registry | Already linked by smoke test; provides all encoding/decoding utilities |
| liboqs | (fetched) | SHA3-256 for signing input, ML-DSA-87 via RelayIdentity::sign() | Already available via relay_lib link; oqs/sha3.h for incremental hashing |
| nlohmann/json | (fetched) | JSON parse/dump for request construction and response validation | Already used in smoke test |
| OpenSSL | 3.3+ (system) | RAND_bytes for WS frame masking | Already used in smoke test |
| FlatBuffers | (fetched) | blob_codec encode/decode for Data(8) FlatBuffer encoding | Already available via relay_lib; wire::encode_blob() |

### Supporting
No additional libraries needed. Everything is already linked.

**Installation:** No new dependencies. `relay_smoke_test` already links `chromatindb_relay_lib` which transitively provides all needed libraries.

## Architecture Patterns

### Test Organization (Recommended)

The extended smoke test should organize tests in logical groups that follow the request/response flow:

```
main() flow:
  1. TCP connect + WS upgrade + ML-DSA-87 auth (existing)
  2. Subscribe to own namespace (existing)
  3. Data write group:
     - data(8) -> write_ack(30)           [NEW: needs signed blob]
     - read_request(31) -> read_response(32) [NEW: binary WS frame]
     - metadata_request(47) -> metadata_response(48) [existing validates not-found, NEW: validates found]
     - exists_request(37) -> exists_response(38)     [existing: not-found, NEW: verify found]
     - batch_read_request(53) -> batch_read_response(54) [NEW: binary WS frame]
     - batch_exists_request(49) -> batch_exists_response(50) [NEW]
     - delete(17) -> delete_ack(18)       [NEW: needs known hash from write]
  4. Compound queries (many existing, add remaining):
     - node_info_request(39) -> node_info_response(40) [existing]
     - stats_request(35) -> stats_response(36)         [existing]
     - storage_status_request(43) -> storage_status_response(44) [existing]
     - exists_request(37) -> exists_response(38)       [existing]
     - list_request(33) -> list_response(34)           [existing]
     - namespace_list_request(41) -> namespace_list_response(42) [existing]
     - namespace_stats_request(45) -> namespace_stats_response(46) [NEW]
     - peer_info_request(55) -> peer_info_response(56) [existing]
     - time_range_request(57) -> time_range_response(58) [NEW]
     - delegation_list_request(51) -> delegation_list_response(52) [NEW]
  5. Fire-and-forget:
     - ping(5)    [NEW: send, verify no error/disconnect]
     - pong(6)    [NEW: send, verify no error/disconnect]
     - goodbye(7) [NEW: send as final test, connection closes]
  6. Error paths:
     - read nonexistent blob -> read_response with status=0
     - stats for nonexistent namespace -> stats_response with zeros
     - metadata for nonexistent hash -> metadata_response found=false
     - malformed request (blocked type) -> error response with code
  7. Unsubscribe + summary (existing)
```

### Pattern 1: Signed Blob Construction

**What:** Build a valid signed blob in the smoke test tool for Data(8) write.
**When to use:** To test the Data -> WriteAck -> Read -> ReadResponse -> Delete -> DeleteAck chain.

The signing input is SHA3-256(namespace || data || ttl_le32 || timestamp_le64), then ML-DSA-87 sign that 32-byte digest. The JSON Data message then includes namespace, pubkey, data, ttl, timestamp, and signature.

```cpp
// Source: db/wire/codec.cpp build_signing_input() + relay/translate/translator.cpp encode_data_blob()

#include <oqs/sha3.h>

// Build signing input: SHA3-256(namespace || data || ttl_le32 || timestamp_le64)
static std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl, uint64_t timestamp)
{
    std::array<uint8_t, 32> digest{};
    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);
    OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), namespace_id.size());
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());
    uint8_t ttl_le[4] = {
        static_cast<uint8_t>(ttl), static_cast<uint8_t>(ttl >> 8),
        static_cast<uint8_t>(ttl >> 16), static_cast<uint8_t>(ttl >> 24)};
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_le, 4);
    uint8_t ts_le[8];
    for (int i = 0; i < 8; ++i) ts_le[i] = static_cast<uint8_t>(timestamp >> (i * 8));
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ts_le, 8);
    OQS_SHA3_sha3_256_inc_finalize(digest.data(), &ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&ctx);
    return digest;
}

// Create signed blob JSON for Data(8) write
json make_data_message(const identity::RelayIdentity& id, uint32_t request_id) {
    auto ns = id.public_key_hash();
    auto pk = id.public_key();
    std::vector<uint8_t> test_data = {'H', 'e', 'l', 'l', 'o'};
    uint32_t ttl = 3600;
    uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

    auto digest = build_signing_input(ns, test_data, ttl, timestamp);
    auto signature = id.sign(digest);

    return {
        {"type", "data"},
        {"request_id", request_id},
        {"namespace", util::to_hex(ns)},
        {"pubkey", util::to_hex(pk)},
        {"data", util::base64_encode(test_data)},
        {"ttl", ttl},
        {"timestamp", std::to_string(timestamp)},
        {"signature", util::base64_encode(
            std::span<const uint8_t>(signature.data(), signature.size()))}
    };
}
```

### Pattern 2: Binary WebSocket Frame Handling

**What:** ReadResponse(32) and BatchReadResponse(54) are sent as binary WS frames (opcode 0x02).
**When to use:** The existing ws_recv_text() only handles text frames and control frames. Need a variant that handles both.

```cpp
// Extended frame receiver that handles both text (0x01) and binary (0x02) frames
struct WsFrame {
    uint8_t opcode;
    std::string payload;
};

static std::optional<WsFrame> ws_recv_frame(int fd) {
    uint8_t hdr[2];
    if (!recv_all(fd, hdr, 2)) return std::nullopt;
    uint8_t opcode = hdr[0] & 0x0F;
    // ... same length/mask parsing as ws_recv_text ...
    // Return {opcode, payload_string} for both text and binary
    // Handle PING -> send PONG + recurse (same as existing)
    // Handle CLOSE -> return nullopt
}
```

**Critical detail:** The relay sends binary responses as `session->send_binary(json_opt->dump())` -- this means the payload is still JSON text, just in a binary WS frame. So the smoke test can parse the payload as JSON regardless of opcode.

### Pattern 3: Request/Response Test Pattern

**What:** Standard pattern for each request/response pair.

```cpp
// Standard test pattern:
{
    json msg = {{"type", "xxx_request"}, {"request_id", N}, /* fields */};
    auto resp = send_recv(msg);  // or send_recv_any_frame() for binary responses
    bool ok = resp && resp->contains("type") &&
              (*resp)["type"] == "xxx_response" &&
              resp->contains("expected_field");
    record("xxx_request", ok, ok ? "detail" : (resp ? resp->dump() : "no response"));
}
```

### Pattern 4: Error Path Testing

**What:** Test that the relay returns structured error responses for failure cases.
**Node error encoding:** The node doesn't send JSON error messages. Instead:
- ReadResponse(32) with status=0x00 means "not found" (no blob data follows)
- MetadataResponse(48) with found=false means "not found"
- StatsResponse(36) returns zeros for nonexistent namespace
- NamespaceStatsResponse(46) with found=false means namespace doesn't exist
- The RELAY itself returns `{"type": "error", "code": "xxx", "message": "..."}` for relay-level errors (bad_json, missing_type, blocked_type, translation_error, node_unavailable, rate_limited, send_failed, subscription_limit)

So "error responses" in this context are really about:
1. Node returning "not found" status in structured responses (ReadResponse status=0, MetadataResponse found=false)
2. Relay returning JSON error messages for invalid requests

### Anti-Patterns to Avoid
- **Testing response types directly:** Do NOT send response-type messages (write_ack, read_response, etc.) as requests. They exist in the allowlist for outbound translation only. Sending them would fail at the node.
- **Expecting Pong response from Ping:** Application-level Ping(5)/Pong(6) are fire-and-forget through the relay. The node may send Pong(6) back with request_id=0, but the relay drops it as "unhandled server-initiated message". Do NOT wait for a response after sending Ping.
- **Forgetting binary WS frames:** ReadResponse(32) and BatchReadResponse(54) arrive as binary WS frames (opcode 0x02). The existing ws_recv_text() will not recognize these. Must update the frame reader.
- **Assuming Subscribe produces a response:** Subscribe/Unsubscribe are relay-intercepted. The relay does NOT send an acknowledgment back to the client. The test should verify no error/disconnect, not wait for a response.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| FlatBuffer blob encoding | Manual byte packing | wire::encode_blob() from blob_codec.h | Already linked via chromatindb_relay_lib; handles all FlatBuffer field alignment |
| SHA3-256 hashing | Custom hash impl | OQS_SHA3_sha3_256_inc_* from oqs/sha3.h | Exact same API as db/wire/codec.cpp; byte-identical results |
| ML-DSA-87 signing | Raw OQS_SIG calls | RelayIdentity::sign() | Already loaded in smoke test; handles OQS lifecycle |
| Hex encoding/decoding | sprintf loops | util::to_hex() / util::from_hex() | Already included in smoke test |
| Base64 encoding | Manual impl | util::base64_encode() / util::base64_decode() | Already linked via relay_lib |
| WS frame construction | Raw byte packing | Existing ws_send_text() pattern | Already correct, masked, RFC 6455 compliant |

**Key insight:** The smoke test already links `chromatindb_relay_lib` which provides ALL needed utilities. The only new code is the SHA3-256 signing input construction (~15 lines) and the Data message JSON assembly (~15 lines).

## Common Pitfalls

### Pitfall 1: Binary WS Frame for ReadResponse/BatchReadResponse
**What goes wrong:** Existing ws_recv_text() returns nullopt for opcode 0x02 (binary) frames, causing ReadResponse and BatchReadResponse tests to fail with "no response".
**Why it happens:** Phase 106 smoke test only handled text responses because it didn't test Data write/read.
**How to avoid:** Extend the WS frame receiver to handle opcode 0x02 the same as 0x01. The payload is JSON text in both cases (relay uses json_opt->dump() for both send_json and send_binary).
**Warning signs:** Tests that send ReadRequest or BatchReadRequest fail with "no response" despite relay logs showing successful translation.

### Pitfall 2: Signing Input Endianness
**What goes wrong:** Signing input uses LITTLE-endian for ttl (uint32) and timestamp (uint64), which is contrary to the BE convention used everywhere else in the wire protocol.
**Why it happens:** The signing input is protocol-defined and matches the db/wire/codec.cpp implementation. The wire protocol uses BE for framing, but the canonical signing input uses LE.
**How to avoid:** Copy the exact LE encoding from db/wire/codec.cpp:build_signing_input(). Do NOT use util::store_u32_be() for the signing input.
**Warning signs:** WriteAck returns with status != 0 (signature verification failure at the node).

### Pitfall 3: Node Sends SyncNamespaceAnnounce(62) After UDS Handshake
**What goes wrong:** Not relevant for the smoke test (it connects via WebSocket, not UDS), but documented for completeness. The UDS tap tool must drain this unsolicited message.
**How to avoid:** Already handled in the tap tool. The smoke test connects via WebSocket and is unaffected.

### Pitfall 4: Subscribe/Unsubscribe Don't Produce Responses
**What goes wrong:** Test waits for a response after sending Subscribe/Unsubscribe, blocking indefinitely.
**Why it happens:** Subscribe/Unsubscribe are relay-intercepted (ws_session.cpp line 566: `co_return` after tracker update). No response is sent to the client.
**How to avoid:** After sending Subscribe/Unsubscribe, immediately proceed to the next test. Verify success indirectly by checking that subsequent requests work and no error was received.
**Warning signs:** Test hangs after sending subscribe.

### Pitfall 5: Ping/Pong/Goodbye Are Fire-and-Forget
**What goes wrong:** Test sends Ping(5) and waits for Pong(6) response, blocking indefinitely.
**Why it happens:** The relay forwards Ping/Pong/Goodbye to the node without registering a request (ws_session.cpp line 580-586). Node's Pong response arrives with request_id=0 and is dropped by route_response as "unhandled server-initiated message".
**How to avoid:** Send Ping/Pong and verify the connection stays alive (send another test request after). Send Goodbye as the very last test since it may trigger disconnect.
**Warning signs:** Test hangs after sending Ping.

### Pitfall 6: Data Message JSON Format
**What goes wrong:** Data(8) JSON uses different field names than expected.
**Why it happens:** The encode_data_blob() function in translator.cpp expects specific JSON fields: "namespace" (hex), "pubkey" (hex), "data" (base64), "ttl" (number), "timestamp" (string or number), "signature" (base64).
**How to avoid:** Match the exact JSON format from encode_data_blob(). Note that "timestamp" accepts both string and number formats.
**Warning signs:** Relay returns `{"type": "error", "code": "translation_error"}`.

### Pitfall 7: TimeRangeRequest Requires Two Timestamps
**What goes wrong:** TimeRangeRequest JSON schema has a "since" field (UINT64_STRING) but the wire format is 52 bytes: namespace(32) + start_ts(8) + end_ts(8) + limit(4).
**Why it happens:** The JSON schema only shows "since" as UINT64_STRING. Looking at the actual json_to_binary encoding, the field spec has only "since" and "limit" -- but the Phase 106 findings established the wire format is 52 bytes with start_ts AND end_ts.
**How to avoid:** Check the json_schema.h carefully. TIME_RANGE_REQUEST_FIELDS has: request_id, namespace(HEX_32), since(UINT64_STRING), limit(UINT32_NUMBER). The encoder outputs 32+8+4=44 bytes. But Phase 106 summary says the node expects 52 bytes. This is resolved: the "since" field in JSON maps to start_ts(8 bytes), and end_ts is NOT a separate JSON field -- the encoder must add it. Actually, looking more carefully at the field spec, there's only "since" (8 bytes) and "limit" (4 bytes) after the namespace (32 bytes) = 44 bytes. But Phase 106 says the node expects 52. This discrepancy was already fixed in Phase 106 -- the UDS tap tool sends 52-byte TimeRangeRequests correctly. The smoke test uses JSON which goes through the translator, so if the translator produces the wrong size, the node will reject it. Needs verification during testing.
**Warning signs:** TimeRangeRequest fails with no response or node error in logs.

## Code Examples

### Complete Data Write + Read Roundtrip Flow

```cpp
// Source: relay/translate/translator.cpp encode_data_blob(), db/wire/codec.cpp build_signing_input()

// 1. Write blob
auto data_msg = make_data_message(id, 100);  // helper from Pattern 1
auto write_resp = send_recv(data_msg);
bool write_ok = write_resp && (*write_resp)["type"] == "write_ack" &&
                write_resp->contains("hash") && write_resp->contains("seq_num");
record("data_write", write_ok, ...);

// 2. Read it back (binary WS frame!)
std::string blob_hash = (*write_resp)["hash"];
json read_msg = {{"type", "read_request"}, {"request_id", 101},
                 {"namespace", ns_hex}, {"hash", blob_hash}};
auto read_resp = send_recv_any_frame(read_msg);  // handles opcode 0x02
bool read_ok = read_resp && (*read_resp)["type"] == "read_response" &&
               (*read_resp)["status"] == 1 &&
               read_resp->contains("data");
record("read_request", read_ok, ...);

// 3. Verify data matches
if (read_ok) {
    auto decoded_data = util::base64_decode((*read_resp)["data"].get<std::string>());
    bool match = decoded_data && *decoded_data == test_data;
    record("read_data_match", match, ...);
}
```

### Error Path Testing Examples

```cpp
// Source: relay/ws/ws_session.cpp error response patterns

// 1. Read nonexistent blob -> read_response with status=0 (not an "error" message)
{
    std::string zero_hash(64, '0');
    json msg = {{"type", "read_request"}, {"request_id", 200},
                {"namespace", ns_hex}, {"hash", zero_hash}};
    auto resp = send_recv_any_frame(msg);  // binary WS frame
    bool ok = resp && (*resp)["type"] == "read_response" && (*resp)["status"] == 0;
    record("error_read_nonexistent", ok, ...);
}

// 2. Metadata for nonexistent hash -> metadata_response found=false
{
    std::string zero_hash(64, '0');
    json msg = {{"type", "metadata_request"}, {"request_id", 201},
                {"namespace", ns_hex}, {"hash", zero_hash}};
    auto resp = send_recv(msg);
    bool ok = resp && (*resp)["type"] == "metadata_response" && (*resp)["found"] == false;
    record("error_metadata_nonexistent", ok, ...);
}

// 3. Blocked type -> relay error response
{
    json msg = {{"type", "blob_notify"}, {"request_id", 202}};  // blocked type
    auto resp = send_recv(msg);  // or use ws_recv_text if we don't want to wait
    bool ok = resp && (*resp)["type"] == "error" && (*resp)["code"] == "blocked_type";
    record("error_blocked_type", ok, ...);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Skip Data(8) in smoke test | Build signed blob in smoke test | Phase 107 (now) | Enables full write/read/delete chain testing |
| Text-only WS frame receiver | Text + binary frame receiver | Phase 107 (now) | Required for ReadResponse/BatchReadResponse |
| 13 tests covering 7 types | ~40+ tests covering all 38 types | Phase 107 (now) | E2E-01 requirement completion |

## Open Questions

1. **TimeRangeRequest wire format discrepancy**
   - What we know: JSON schema has namespace(32) + since(8) + limit(4) = 44 bytes. Phase 106 says node expects 52 bytes (with start_ts + end_ts).
   - What's unclear: Whether the translator correctly produces 52-byte payloads from the JSON schema fields, or if there's a mismatch. The tap tool sends raw binary directly, bypassing the translator.
   - Recommendation: Test this path carefully. If it fails, the translator may need a fix (adding an "until" or "end" field to the JSON schema). Check relay logs for size mismatch errors.

2. **Notification (type 21) verification**
   - What we know: Notification is triggered by a blob write to a subscribed namespace. The smoke test subscribes first, then writes a blob, so a notification should be delivered.
   - What's unclear: Whether the notification arrives before or after the WriteAck, and how to handle the ordering in a blocking-socket test.
   - Recommendation: After Data write, first receive the WriteAck, then attempt to receive a second frame (with timeout) for the Notification. If ordering is unpredictable, use a small receive loop that collects both and validates.

3. **Goodbye (type 7) behavior**
   - What we know: Goodbye is fire-and-forget. The relay forwards it to the node.
   - What's unclear: Whether the node or relay closes the connection after receiving Goodbye, or if the connection stays open.
   - Recommendation: Send Goodbye as the very last test. Verify send succeeds. Don't expect a response.

## Complete Type-by-Type Test Strategy

### Types Already Covered by Existing Smoke Test (7 tests)
| Type | Wire | Test | Status |
|------|------|------|--------|
| subscribe | 19 | Send, verify no error | EXISTING |
| node_info_request/response | 39/40 | Send, validate version field | EXISTING |
| stats_request/response | 35/36 | Send, validate blob_count | EXISTING |
| storage_status_request/response | 43/44 | Send, validate used_bytes+capacity_bytes | EXISTING |
| exists_request/response | 37/38 | Send with zero hash, validate exists field | EXISTING |
| list_request/response | 33/34 | Send, validate list_response type | EXISTING |
| namespace_list_request/response | 41/42 | Send, validate namespace_list_response type | EXISTING |
| peer_info_request/response | 55/56 | Send, validate peer_info_response type | EXISTING |
| unsubscribe | 20 | Send at cleanup | EXISTING |

### Types Needing New Tests

**Data write chain (requires signed blob):**
| Type | Wire | JSON Fields | Expected Response | Notes |
|------|------|-------------|-------------------|-------|
| data | 8 | namespace, pubkey, data, ttl, timestamp, signature | write_ack(30): hash, seq_num, status | FlatBuffer path; needs signed blob |
| read_request | 31 | namespace, hash | read_response(32): status, namespace, pubkey, data, ttl, timestamp, signature | Binary WS frame; use hash from write_ack |
| delete | 17 | namespace, hash | delete_ack(18): status | Use hash from write_ack |
| batch_read_request | 53 | namespace, hashes, max_bytes | batch_read_response(54): truncated, blobs[] | Binary WS frame; use hash from write_ack |
| batch_exists_request | 49 | namespace, hashes | batch_exists_response(50): results[] | Use hash from write_ack + nonexistent |

**Query types not yet covered:**
| Type | Wire | JSON Fields | Expected Response | Notes |
|------|------|-------------|-------------------|-------|
| namespace_stats_request | 45 | namespace | namespace_stats_response(46): found, blob_count, etc. | Compound decode |
| time_range_request | 57 | namespace, since, limit | time_range_response(58): entries[], truncated | Compound decode; verify wire size |
| delegation_list_request | 51 | namespace | delegation_list_response(52): delegations[] | Compound decode; likely empty |
| metadata_request | 47 | namespace, hash | metadata_response(48): found, hash, timestamp, etc. | Compound; test both found and not-found |

**Fire-and-forget:**
| Type | Wire | Strategy | Notes |
|------|------|----------|-------|
| ping | 5 | Send, verify connection alive (send another request after) | No response expected |
| pong | 6 | Send, verify connection alive | No response expected |
| goodbye | 7 | Send as final test | May trigger close |

**Notification (triggered):**
| Type | Wire | Strategy | Notes |
|------|------|----------|-------|
| notification | 21 | Subscribe to namespace, write blob, receive notification | Must handle ordering with WriteAck |

**Already implicit in responses:**
| Response Type | Wire | Covered By |
|---------------|------|------------|
| write_ack | 30 | data write test |
| delete_ack | 18 | delete test |
| exists_response | 38 | exists_request test |
| read_response | 32 | read_request test |
| batch_read_response | 54 | batch_read_request test |
| batch_exists_response | 50 | batch_exists_request test |
| list_response | 34 | list_request test |
| stats_response | 36 | stats_request test |
| node_info_response | 40 | node_info_request test |
| namespace_list_response | 42 | namespace_list_request test |
| storage_status_response | 44 | storage_status_request test |
| namespace_stats_response | 46 | namespace_stats_request test |
| metadata_response | 48 | metadata_request test |
| delegation_list_response | 52 | delegation_list_request test |
| peer_info_response | 56 | peer_info_request test |
| time_range_response | 58 | time_range_request test |
| notification | 21 | write-after-subscribe test |

**Signal types (node-originated, untestable directly):**
| Type | Wire | Notes |
|------|------|-------|
| storage_full | 22 | Would require filling storage to capacity; out of scope per D-05 |
| quota_exceeded | 25 | Would require setting up quota; out of scope per D-05 |

### Total Coverage
- 16 request/response pairs = 32 types (each test exercises both request AND response)
- 2 relay-intercepted (subscribe + unsubscribe)
- 3 fire-and-forget (ping + pong + goodbye)
- 1 notification (triggered by write + subscribe)
- **Total: 38 types verified**
- 2 node signals (storage_full, quota_exceeded) not directly testable without special node state; they exist in the type registry but cannot be triggered from a normal smoke test

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Custom C++ smoke test (not Catch2) |
| Config file | N/A -- standalone binary |
| Quick run command | `/tmp/chromatindb-test/run-smoke.sh` |
| Full suite command | `/tmp/chromatindb-test/run-smoke.sh` (same) |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| E2E-01 | All 38 relay-allowed message types translate correctly | integration (live node) | `/tmp/chromatindb-test/run-smoke.sh` | Partially (13 of ~40 tests exist) |

### Sampling Rate
- **Per task commit:** Build and verify tests compile: `cmake --build build -j$(nproc) --target relay_smoke_test`
- **Per wave merge:** Run full suite: `/tmp/chromatindb-test/run-smoke.sh`
- **Phase gate:** Full suite green (all tests pass) before verification

### Wave 0 Gaps
- [ ] Extend ws_recv_text() to handle binary WS frames (opcode 0x02) -- covers ReadResponse/BatchReadResponse
- [ ] Add SHA3-256 signing input helper function -- covers Data(8) write
- [ ] Add Data message construction helper -- covers write/read/delete chain

## Sources

### Primary (HIGH confidence)
- `relay/translate/type_registry.h` -- all 40 type entries (38 client + 2 node signals), source of truth
- `relay/translate/translator.cpp` -- json_to_binary() and binary_to_json() with all compound decoders, encode_data_blob()
- `relay/translate/json_schema.h` -- all FieldSpec definitions for JSON field names and encodings
- `relay/translate/json_schema.cpp` -- schema registry, is_flatbuffer/is_compound flags
- `relay/ws/ws_session.cpp` -- error response patterns, fire-and-forget routing, subscribe interception
- `relay/core/uds_multiplexer.cpp` -- route_response() for binary WS frame decision, bulk_fail, notification fan-out
- `relay/core/message_filter.cpp` -- 38-type allowlist (sorted), 40-type outbound list
- `tools/relay_smoke_test.cpp` -- existing 13-test smoke test infrastructure
- `tools/relay_uds_tap.cpp` -- reference for wire format payloads and UDS handshake
- `relay/wire/blob_codec.h/cpp` -- FlatBuffer blob encode/decode
- `db/wire/codec.h/cpp` -- build_signing_input() canonical signing format (LE endian for ttl/timestamp)
- `relay/identity/relay_identity.h` -- sign() method, key sizes
- `.planning/phases/106-bug-fixes/106-03-SUMMARY.md` -- Phase 106 live test results and deviations

### Secondary (MEDIUM confidence)
- Phase 106 findings on wire format sizes (ListRequest 44B, NamespaceListRequest 36B, TimeRangeRequest 52B)

### Tertiary (LOW confidence)
- TimeRangeRequest translator output size vs node expectation -- needs live testing verification

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all code already exists and is linked
- Architecture: HIGH -- extending existing working pattern with well-understood additions
- Pitfalls: HIGH -- identified from direct source code analysis (binary WS frames, signing endianness, fire-and-forget behavior)
- Error paths: HIGH -- error response format directly visible in ws_session.cpp source

**Research date:** 2026-04-11
**Valid until:** 2026-05-11 (stable -- relay code frozen, only extending test tool)
