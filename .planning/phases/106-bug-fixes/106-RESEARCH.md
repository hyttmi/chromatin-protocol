# Phase 106: Bug Fixes - Research

**Researched:** 2026-04-11
**Domain:** Relay binary protocol translation, C++ coroutine safety, sanitizer testing
**Confidence:** HIGH

## Summary

Phase 106 addresses two categories of bugs in the relay: (1) wire format mismatches between the node's actual binary response format and the relay's compound response decoders, and (2) coroutine safety patterns around `std::visit` and lambda captures in async code.

Research uncovered **three critical wire format mismatches** in compound response decoders by cross-referencing the node's `message_dispatcher.cpp` response handlers against the relay's `translator.cpp` decode helpers. Additionally, `StatsResponse(36)` is treated as a flat-decoded type in the relay schema but the node sends a completely different 24-byte per-namespace format. One `std::visit` call site exists in relay code (in `shutdown_socket()`) and is provably safe (synchronous, non-coroutine context). The db/ audit scope covers ~3,631 lines across 5 peer/ component files.

**Primary recommendation:** Fix the four translation mismatches (NodeInfoResponse, StatsResponse, TimeRangeResponse, DelegationListResponse JSON field names) by updating the relay decoders to match the node's actual wire format. The single `std::visit` in `shutdown_socket()` is safe and needs only a documenting comment. The db/ coroutine audit is a read-only sweep of peer/ decomposition code.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Verification uses both captured binary fixtures AND live node testing. Fixtures for reproducible unit tests, live tests for catching real-world drift.
- **D-02:** Fix bugs in place but add consistent bounds checking to all 10 compound decoders (some currently lack overflow guards). Not a full rewrite.
- **D-03:** Binary fixtures stored in `relay/tests/fixtures/` for large responses, inline hex in test code for small/simple types.
- **D-04:** Build a reusable UDS tap tool that connects to the node's UDS, sends each request type, and dumps raw binary responses to files. Reusable for Phase 107 message type verification.
- **D-05:** Broader sweep beyond std::visit -- covers all four categories: lambda captures in coroutines, shared_ptr lifetimes across co_await, container invalidation across suspension points, and strand confinement.
- **D-06:** Sweep both relay/ AND db/, but only fix issues in relay/. db/ findings documented in a separate DB-COROUTINE-FINDINGS.md for user's later manual sweep.
- **D-07:** db/ audit focuses on three areas: peer/ decomposed code (Phase 96), sync protocol (Phase A/B/C), and connection lifecycle (on_peer_connected, keepalive, disconnect).
- **D-08:** Fix all issues found in relay/ within this phase (no deferral). Clean foundation before E2E testing in Phases 107-108.
- **D-09:** Documentation: code comments at each fix site explaining the coroutine safety issue, PLUS a COROUTINE-AUDIT.md summary for the phase record. Separate DB-COROUTINE-FINDINGS.md for db/ read-only findings.
- **D-10:** All three sanitizers: ASAN+UBSAN in one build, TSAN in a separate build. Run both.
- **D-11:** Write a minimal smoke test in Phase 106 that exercises key paths (write, read, subscribe, compound responses) through a live relay+node. Run under all sanitizers. Phase 107 extends this to all 38 types.
- **D-12:** CMake sanitizer preset already exists -- use it. No new build infrastructure needed.

### Claude's Discretion
- Specific bounds-check patterns for compound decoders (cursor vs manual offset tracking)
- Smoke test framework choice (standalone script vs Catch2 integration test)
- COROUTINE-AUDIT.md format and severity rating scheme

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| FIX-01 | binary_to_json succeeds for all compound response types (NodeInfoResponse, StatsResponse, etc.) against live node data | Research identified 3 critical wire format mismatches + 1 semantic field name issue. Node wire format verified from message_dispatcher.cpp lines 517-1085. |
| FIX-02 | All std::visit + coroutine lambda patterns in relay/ audited and replaced with get_if/get branching | Research found 1 std::visit call site in relay/ (shutdown_socket line 751), confirmed safe (synchronous non-coroutine). async_read/async_write already fixed in commit 16e6caf. db/ audit scope: 3,631 lines across 5 files. |
</phase_requirements>

## Architecture Patterns

### Confirmed Wire Format Mismatches (FIX-01)

These are the critical bugs found by cross-referencing `db/peer/message_dispatcher.cpp` (node) against `relay/translate/translator.cpp` (relay).

#### Bug 1: NodeInfoResponse(40) -- CRITICAL

**Node sends** (message_dispatcher.cpp lines 551-579):
```
[version_len:u8][version:N]
[git_hash_len:u8][git_hash:N]
[uptime:u64BE][peer_count:u32BE][namespace_count:u32BE]
[total_blobs:u64BE][storage_used:u64BE][storage_max:u64BE]
[types_count:u8][type_bytes:types_count]
```
- String lengths are **1-byte** (u8), not u16BE
- Supported types are **raw bytes** (each 1 byte), not u16BE-prefixed strings
- `types_count` is **1-byte** (u8), not u16BE

**Relay decoder expects** (translator.cpp lines 633-675):
```
[version_len:u16BE][version:N]
[git_hash_len:u16BE][git_hash:N]
[uptime:u64BE][peer_count:u32BE][namespace_count:u32BE]
[total_blobs:u64BE][storage_used:u64BE][storage_max:u64BE]
[supported_count:u16BE][ [str_len:u16BE][type_string:N] * count ]
```

**Fix:** Rewrite `decode_node_info_response` to use u8 length prefixes for version/git_hash, u8 count, and raw byte array for supported_types. Translate type bytes to type name strings in the JSON output (using `type_to_string()` from type_registry.h).

#### Bug 2: StatsResponse(36) -- CRITICAL

**Node sends** (message_dispatcher.cpp lines 473-479):
```
[blob_count:u64BE][total_bytes:u64BE][byte_limit:u64BE]
```
Total: 24 bytes. This is per-namespace stats (StatsRequest takes a namespace parameter).

**Relay schema expects** (json_schema.h lines 137-144, flat-decoded):
```
[total_blobs:u64BE][storage_used:u64BE][storage_max:u64BE]
[namespace_count:u32BE][peer_count:u32BE][uptime:u64BE]
```
Total: 48 bytes. This is global stats format.

**Fix:** StatsResponse(36) must become a compound type (mark `is_compound = true` in schema) with a custom decoder matching the node's 24-byte format: `[blob_count:u64BE][total_bytes:u64BE][byte_limit:u64BE]`. JSON field names should match the per-namespace semantics.

#### Bug 3: TimeRangeResponse(58) -- CRITICAL

**Node sends** (message_dispatcher.cpp lines 1155-1167):
```
[truncated:u8]
[count:u32BE]
[ [hash:32][seq_num:u64BE][timestamp:u64BE] * count ]
```
- Truncated byte is **first** (before count)
- Entries are **48 bytes** each (hash:32 + seq_num:8 + timestamp:8)

**Relay decoder expects** (translator.cpp lines 568-588):
```
[count:u32BE]
[ [hash:32][timestamp:u64BE] * count ]
[truncated:u8]
```
- Truncated byte is **last** (after entries)
- Entries are **40 bytes** each (hash:32 + timestamp:8) -- **missing seq_num**

**Fix:** Rewrite `decode_time_range_response` to read truncated first, then count, then 48-byte entries with seq_num included.

#### Bug 4: DelegationListResponse(52) -- SEMANTIC

**Node sends** (message_dispatcher.cpp lines 884-892):
```
[count:u32BE][ [delegate_pk_hash:32][delegation_blob_hash:32] * count ]
```

**Relay decoder produces** (translator.cpp lines 518-530):
```json
{"delegations": [{"namespace": "...", "pubkey_hash": "..."}]}
```

Binary format is structurally correct (count + 64-byte pairs). JSON field names are semantically wrong -- they should be `delegate_pk_hash` and `delegation_blob_hash`, not `namespace` and `pubkey_hash`.

**Fix:** Rename JSON field names in `decode_delegation_list_response`.

### Verified Correct Decoders

These compound decoders match the node's wire format (confirmed by cross-referencing):

| Decoder | Wire Type | Status |
|---------|-----------|--------|
| decode_list_response | 34 | Correct |
| decode_namespace_list_response | 42 | Correct |
| decode_metadata_response | 48 | Correct |
| decode_batch_exists_response | 50 | Correct |
| decode_peer_info_response | 56 | Correct (trusted format) |
| decode_read_response | 32 | Correct (FlatBuffer) |
| decode_batch_read_response | 54 | Correct (FlatBuffer) |

### Recommended Bounds-Check Pattern (Claude's Discretion)

All compound decoders should use the same pattern: manual `size_t off` cursor with bounds check before every read:

```cpp
static std::optional<nlohmann::json> decode_xxx(std::span<const uint8_t> p) {
    size_t off = 0;
    // Guard before every read
    if (off + N > p.size()) return std::nullopt;
    auto val = read_uNN_be(p.data() + off); off += N;
    // ...
}
```

This is already the pattern in most decoders. Ensure consistency across all 10.

### std::visit Audit (FIX-02)

#### Relay std::visit Call Sites

| File | Line | Context | Safe? | Reason |
|------|------|---------|-------|--------|
| ws_session.cpp | 751 | `shutdown_socket()` | YES | Synchronous lambda in non-coroutine function. No co_await, no stack frame destruction. |
| ws_session.cpp | 238-263 | `async_read`/`async_write` | Already fixed | Commit 16e6caf replaced std::visit with get_if/get pattern. |

**Conclusion:** The single remaining `std::visit` in `shutdown_socket()` is safe and needs only a documenting comment. No code change required.

#### Broader Coroutine Safety Categories to Audit

Per D-05, the sweep covers four categories across all relay/ and db/ code:

1. **Lambda captures in coroutines:** Any lambda that captures local variables/references and is passed to `co_spawn` or similar. Risk: captured stack references dangle after co_await.
2. **shared_ptr lifetimes across co_await:** Any coroutine that uses raw pointers/references to objects whose lifetime depends on shared_ptr held elsewhere. Risk: object destroyed during suspension.
3. **Container invalidation across suspension points:** Any code that holds iterators, pointers, or references to container elements across co_await. Risk: container modified by another coroutine during suspension.
4. **Strand confinement:** Any shared state accessed from coroutines without proper strand/mutex protection. Risk: data races.

#### Relay Coroutine Patterns Already Safe

The relay uses consistent patterns that prevent most issues:
- `shared_from_this()` captured in all co_spawn lambdas (WsSession::start, send_json, send_binary, close, idle timer, auth timer)
- get_if/get pattern for variant access in coroutines (async_read, async_write)
- No iterators held across co_await in session code

#### db/ Audit Scope

| File | Lines | Focus Areas |
|------|-------|-------------|
| connection_manager.cpp | 426 | on_peer_connected, keepalive_loop, dedup, disconnect |
| sync_orchestrator.cpp | 1299 | Phase A/B/C, cursor management, safety-net cycle |
| message_dispatcher.cpp | 1252 | co_spawn handlers, PeerInfo re-lookup pattern |
| blob_push_manager.cpp | 265 | pending_fetches_, BlobNotify/BlobFetch |
| pex_manager.cpp | 389 | PEX protocol coroutines |
| **Total** | **3,631** | |

### UDS Tap Tool Architecture (D-04)

A standalone C++ program that:
1. Connects to the node's UDS socket
2. Performs TrustedHello + HKDF + Auth handshake (reusing relay's existing wire/ and identity/ code)
3. Sends each of the 38 client-allowed request types with minimal valid payloads
4. Saves raw binary responses to files (one per type) in a specified output directory
5. Reusable by Phase 107 for comprehensive type verification

Should link against `chromatindb_relay_lib` to reuse wire/AEAD/handshake code, and against the node's wire library for TransportCodec.

### Smoke Test Architecture (D-11, Claude's Discretion)

**Recommendation: Standalone C++ program, not Catch2 integration test.**

Rationale:
- Smoke test needs a live relay+node running (external process dependency)
- Catch2 tests are unit tests that run in-process with no external dependencies
- A standalone program can be run manually or scripted under sanitizers
- Phase 107 will extend this into a full E2E test harness

The smoke test should:
1. Connect via WebSocket to the relay
2. Complete ML-DSA-87 challenge-response auth
3. Send write (Data), read (ReadRequest), subscribe, and compound queries (NodeInfoRequest, StatsRequest, StorageStatusRequest)
4. Verify JSON responses parse correctly and contain expected fields
5. Exit 0 on success, non-zero on failure

### COROUTINE-AUDIT.md Format (Claude's Discretion)

**Recommendation:**

```markdown
# Coroutine Safety Audit - Phase 106

## Severity Levels
- CRITICAL: Use-after-free or data race confirmed (fix immediately)
- HIGH: Pattern likely to cause UB under specific conditions (fix)
- MEDIUM: Defensive improvement needed (bounds check, lifetime guard)
- LOW: Style/documentation issue only (comment)
- SAFE: Reviewed and confirmed safe (document why)

## Findings
### [ID] [Severity] [File:Line] [Category]
**Pattern:** [what the code does]
**Risk:** [what could go wrong]
**Fix:** [what was changed] or **Status:** Safe (reason)
```

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| WebSocket client for smoke test | Custom WS client | Existing relay wire/ + ws/ code | Already have frame encode/decode, handshake |
| UDS handshake for tap tool | New handshake impl | Existing relay wire/aead + identity code | TrustedHello/HKDF already implemented |
| Binary response serialization for fixtures | Manual byte arrays | Capture from live node via tap tool | Ensures fixtures match reality |

## Common Pitfalls

### Pitfall 1: Decoder Trusts Schema Without Node Verification
**What goes wrong:** Relay decoder was written from the JSON schema spec, not from the node's actual wire format. Schema says u16BE strings; node sends u8 strings.
**Why it happens:** Phase 103 designed the JSON schema and decoders in parallel without testing against live node data.
**How to avoid:** Always verify decoder against the actual node handler code in message_dispatcher.cpp.
**Warning signs:** binary_to_json returning nullopt for types that should work.

### Pitfall 2: StatsResponse Schema vs Reality
**What goes wrong:** StatsResponse(36) is marked as flat-decoded in the schema, but the node sends a completely different format (24 bytes per-namespace vs 48 bytes global).
**Why it happens:** The schema was designed as a global stats response, but the node's StatsRequest handler actually takes a namespace parameter and returns per-namespace stats.
**How to avoid:** Check what the node's handler actually does before defining the relay schema.

### Pitfall 3: std::visit in Coroutine Context
**What goes wrong:** std::visit with a lambda that captures local variables. When the lambda's operator() returns an awaitable, the enclosing function's stack frame is destroyed on suspension, leaving dangling references.
**Why it happens:** std::visit feels natural for variant dispatch, but doesn't compose safely with C++20 coroutines.
**How to avoid:** Use `std::get_if` for the first alternative, `std::get` for the known-safe fallback.
**Warning signs:** ASAN stack-use-after-return errors.

### Pitfall 4: Synchronous std::visit is Safe
**What goes wrong:** Over-correcting by replacing ALL std::visit calls, including synchronous ones in non-coroutine functions.
**Why it happens:** Blanket "no std::visit" rule without understanding WHY it's unsafe in coroutines.
**How to avoid:** Only replace std::visit when it appears in a coroutine body (function returning `asio::awaitable<T>`). Synchronous std::visit in regular functions is perfectly fine.

### Pitfall 5: Node Code is Frozen
**What goes wrong:** Attempting to fix wire format mismatches by changing the node's response handlers.
**Why it happens:** Forgetting that db/ is frozen for v3.1.0.
**How to avoid:** All fixes must be in relay/ code. The relay decoders must adapt to match the node's format.

## Code Examples

### Example 1: Fixed NodeInfoResponse Decoder

The decoder must match the node's actual format (u8 lengths, raw type bytes):

```cpp
// Source: db/peer/message_dispatcher.cpp lines 551-579
static std::optional<nlohmann::json> decode_node_info_response(std::span<const uint8_t> p) {
    nlohmann::json j;
    j["type"] = "node_info_response";
    size_t off = 0;

    // version: u8 length-prefixed string (NOT u16BE)
    if (off + 1 > p.size()) return std::nullopt;
    uint8_t ver_len = p[off++];
    if (off + ver_len > p.size()) return std::nullopt;
    j["version"] = std::string(reinterpret_cast<const char*>(p.data() + off), ver_len);
    off += ver_len;

    // git_hash: u8 length-prefixed string (NOT u16BE)
    if (off + 1 > p.size()) return std::nullopt;
    uint8_t gh_len = p[off++];
    if (off + gh_len > p.size()) return std::nullopt;
    j["git_hash"] = std::string(reinterpret_cast<const char*>(p.data() + off), gh_len);
    off += gh_len;

    // Fixed-size fields
    if (off + 8 + 4 + 4 + 8 + 8 + 8 > p.size()) return std::nullopt;
    j["uptime"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["peer_count"] = util::read_u32_be(p.data() + off); off += 4;
    j["namespace_count"] = util::read_u32_be(p.data() + off); off += 4;
    j["total_blobs"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["storage_used"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["storage_max"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;

    // supported_types: u8 count + raw type bytes
    if (off + 1 > p.size()) return std::nullopt;
    uint8_t st_count = p[off++];
    if (off + st_count > p.size()) return std::nullopt;
    auto supported = nlohmann::json::array();
    for (uint8_t i = 0; i < st_count; ++i) {
        auto name = translate::type_to_string(p[off++]);
        if (name) {
            supported.push_back(std::string(*name));
        } else {
            supported.push_back(std::to_string(p[off - 1]));  // Unknown type: emit as number string
        }
    }
    j["supported_types"] = std::move(supported);

    return j;
}
```

### Example 2: Fixed TimeRangeResponse Decoder

```cpp
// Source: db/peer/message_dispatcher.cpp lines 1155-1167
static std::optional<nlohmann::json> decode_time_range_response(std::span<const uint8_t> p) {
    if (p.size() < 5) return std::nullopt;  // truncated(1) + count(4) minimum

    // truncated is FIRST byte (not last)
    bool truncated = (p[0] != 0);
    uint32_t count = util::read_u32_be(p.data() + 1);

    // Entries are 48 bytes each: hash(32) + seq_num(8) + timestamp(8)
    size_t expected = 5 + count * 48;
    if (p.size() < expected) return std::nullopt;

    auto entries = nlohmann::json::array();
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 5 + i * 48;
        nlohmann::json entry;
        entry["hash"] = util::to_hex(p.subspan(off, 32));
        entry["seq_num"] = std::to_string(util::read_u64_be(p.data() + off + 32));
        entry["timestamp"] = std::to_string(util::read_u64_be(p.data() + off + 40));
        entries.push_back(std::move(entry));
    }

    nlohmann::json j;
    j["type"] = "time_range_response";
    j["entries"] = std::move(entries);
    j["truncated"] = truncated;
    return j;
}
```

### Example 3: Safe std::visit Documentation Comment

```cpp
void WsSession::shutdown_socket() {
    // SAFETY: std::visit is safe here because shutdown_socket() is NOT a coroutine.
    // The lambda is synchronous (no co_await), so the enclosing stack frame stays
    // valid for the entire visit call. The coroutine-unsafe pattern only applies
    // when a lambda returns an awaitable inside a coroutine body.
    std::visit([this](auto& stream) {
        // ... synchronous shutdown ...
    }, stream_);
}
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | relay/tests/CMakeLists.txt |
| Quick run command | `cd build && ctest --test-dir relay/tests -j$(nproc) --output-on-failure` |
| Full suite command | `cd build && ctest -j$(nproc) --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| FIX-01a | NodeInfoResponse decode matches node wire format | unit | `ctest -R "translator.*node_info" --output-on-failure` | Exists (extend test_translator.cpp) |
| FIX-01b | StatsResponse decode matches node wire format | unit | `ctest -R "translator.*stats" --output-on-failure` | Exists (update test_translator.cpp) |
| FIX-01c | TimeRangeResponse decode matches node wire format | unit | `ctest -R "translator.*time_range" --output-on-failure` | Wave 0 |
| FIX-01d | DelegationListResponse field names correct | unit | `ctest -R "translator.*delegation" --output-on-failure` | Wave 0 |
| FIX-01e | All 10 compound decoders have bounds checks | unit | `ctest -R "translator" --output-on-failure` | Partial (extend) |
| FIX-01f | Live node compound responses translate successfully | smoke | `./build/relay_smoke_test` | Wave 0 |
| FIX-02a | All std::visit sites audited | manual | Review COROUTINE-AUDIT.md | Wave 0 |
| FIX-02b | ASAN clean for basic request/response cycle | smoke+ASAN | `ASAN_OPTIONS=detect_stack_use_after_return=1 ./build/relay_smoke_test` | Wave 0 |
| FIX-02c | db/ coroutine audit documented | manual | Review DB-COROUTINE-FINDINGS.md | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ctest --test-dir relay/tests -j$(nproc) --output-on-failure`
- **Per wave merge:** Full relay test suite + ASAN build + smoke test
- **Phase gate:** Full suite green, ASAN clean, all documents written

### Wave 0 Gaps
- [ ] `relay/tests/fixtures/` directory -- binary fixture files for compound responses
- [ ] TimeRangeResponse and DelegationListResponse unit tests in test_translator.cpp
- [ ] UDS tap tool (`tools/relay_uds_tap.cpp` or similar)
- [ ] Smoke test program (`tools/relay_smoke_test.cpp` or similar)
- [ ] COROUTINE-AUDIT.md template
- [ ] DB-COROUTINE-FINDINGS.md template

## Sanitizer Build Commands

Existing CMake sanitizer support from the root CMakeLists.txt:

```bash
# ASAN + UBSAN build (combined, per D-10)
cmake -B build-asan -DSANITIZER=asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-asan -j$(nproc)

# TSAN build (separate, per D-10)
cmake -B build-tsan -DSANITIZER=tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-tsan -j$(nproc)

# UBSAN build (if needed separately)
cmake -B build-ubsan -DSANITIZER=ubsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-ubsan -j$(nproc)
```

Note: ASAN and UBSAN cannot be combined in a single `-DSANITIZER=` flag with the current CMake setup. The CMake config supports one at a time: `asan`, `tsan`, or `ubsan`. Per D-10, run ASAN and UBSAN as separate builds, plus TSAN as a third build.

## Open Questions

1. **StatsResponse(36) semantics: per-namespace or global?**
   - What we know: Node handler takes a namespace param and returns 3 u64s (blob_count, total_bytes, byte_limit). Relay schema assumes 6-field global stats.
   - What's unclear: What should the JSON field names be? The relay schema uses "total_blobs", "storage_used", etc. but these don't match the per-namespace semantics.
   - Recommendation: Match the node's actual semantics. Use field names like `blob_count`, `storage_bytes`, `quota_bytes_limit` to match what the data actually represents.

2. **DelegationListResponse field semantics**
   - What we know: Node sends `[delegate_pk_hash:32][delegation_blob_hash:32]`. Relay labels them `namespace` and `pubkey_hash`.
   - Recommendation: Use `delegate_pk_hash` and `delegation_blob_hash` to match the node's actual semantics. These are the SHA3-256 hash of the delegate's public key and the hash of the delegation blob.

## Sources

### Primary (HIGH confidence)
- `db/peer/message_dispatcher.cpp` lines 461-1085 -- Node response handlers, actual wire format (verified by reading source)
- `relay/translate/translator.cpp` lines 420-806 -- Relay compound decoders (verified by reading source)
- `relay/translate/json_schema.cpp` lines 20-101 -- Schema registry with compound flags (verified by reading source)
- `relay/ws/ws_session.cpp` lines 238-263, 750-765 -- std::visit patterns (verified by reading source)
- Commit `16e6caf` -- Prior ASAN fix establishing get_if/get pattern

### Secondary (MEDIUM confidence)
- `relay/tests/test_translator.cpp` -- Existing test patterns for translator (verified by reading source)
- Root `CMakeLists.txt` lines 26-49 -- Sanitizer build configuration (verified by reading source)

## Metadata

**Confidence breakdown:**
- Wire format mismatches: HIGH -- verified by reading both node and relay source directly
- Coroutine safety: HIGH -- only 1 std::visit in relay/, confirmed safe by code analysis
- db/ audit scope: HIGH -- file sizes and focus areas verified
- Sanitizer build: HIGH -- CMake config verified from source

**Research date:** 2026-04-11
**Valid until:** 2026-05-11 (stable -- node code is frozen, relay code changes only in this phase)
