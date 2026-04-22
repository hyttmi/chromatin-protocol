---
plan: 127-01-encoder
phase: 127
type: execute
wave: 1
depends_on: []
files_modified:
  - db/peer/message_dispatcher.cpp
autonomous: true
requirements: [NODEINFO-01, NODEINFO-02, NODEINFO-03, NODEINFO-04]
must_haves:
  truths:
    - "NodeInfoResponse payload size grows by exactly 24 bytes in the fixed section (+8 blob, +4 frame, +8 rate, +4 subs)"
    - "The 4 new fields are written BEFORE the [types_count:1][supported_types:N] tail"
    - "max_blob_data_bytes is sourced from db/net/framing.h::MAX_BLOB_DATA_SIZE (not config)"
    - "max_frame_bytes is sourced from db/net/framing.h::MAX_FRAME_SIZE (not config)"
    - "rate_limit field is rate_limit_bytes_per_sec (u64 BE), read from existing MessageDispatcher::rate_limit_bytes_per_sec_ member"
    - "max_subscriptions_per_connection (u32 BE) is read from existing MessageDispatcher::max_subscriptions_ member"
    - "Encoder uses chromatindb::util::store_u64_be and store_u32_be helpers (no inline byte-packing)"
  artifacts:
    - path: "db/peer/message_dispatcher.cpp"
      provides: "Extended NodeInfoResponse encoder with 4 new capability fields"
      contains: "store_u64_be(response.data() + off, chromatindb::net::MAX_BLOB_DATA_SIZE)"
  key_links:
    - from: "db/peer/message_dispatcher.cpp NodeInfoRequest handler"
      to: "db/net/framing.h MAX_BLOB_DATA_SIZE, MAX_FRAME_SIZE"
      via: "#include \"db/net/framing.h\" + namespace-qualified reference"
      pattern: "chromatindb::net::MAX_BLOB_DATA_SIZE|chromatindb::net::MAX_FRAME_SIZE"
    - from: "db/peer/message_dispatcher.cpp NodeInfoRequest handler"
      to: "MessageDispatcher state members"
      via: "this-> member access inside the handler"
      pattern: "rate_limit_bytes_per_sec_|max_subscriptions_"
---

<objective>
Extend the `NodeInfoResponse` encoder in `db/peer/message_dispatcher.cpp` to carry 4 new capability fields (max_blob_data_bytes, max_frame_bytes, rate_limit_bytes_per_sec, max_subscriptions_per_connection) inserted BEFORE `[types_count][supported_types]`. This is the node-side half of the v4.2.0 wire-format extension — CLI decode + tests land in Wave 2 plans (127-03, 127-04) which depend on this plan.

Purpose: Clients and peers must discover the node's caps in a single NodeInfoResponse round-trip so Phase 129/130 consumers can make cap-aware decisions without hardcoding. This plan only ships the encoder; consumption is downstream.

Output: One modified source file (`db/peer/message_dispatcher.cpp`) — adds a `#include "db/net/framing.h"` and writes 4 additional fields inside the existing `TransportMsgType_NodeInfoRequest` handler (lines 661-733).
</objective>

<execution_context>
@$HOME/.claude/get-shit-done/workflows/execute-plan.md
@$HOME/.claude/get-shit-done/templates/summary.md
</execution_context>

<context>
@.planning/STATE.md
@.planning/ROADMAP.md
@.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md

<interfaces>
Current NodeInfoResponse wire layout (pre-Phase 127), extracted from db/peer/message_dispatcher.cpp:690-719:

```
[version_len:1][version:version_len bytes]
[uptime:8 BE uint64]
[peer_count:4 BE uint32]
[namespace_count:4 BE uint32]
[total_blobs:8 BE uint64]
[storage_used:8 BE uint64]
[storage_max:8 BE uint64]
[types_count:1 uint8]
[supported_types:types_count bytes]
```

Target layout (post-Phase 127, per CONTEXT.md D-01 and D-02 — 4 new fields inserted BETWEEN `storage_max` and `types_count`):

```
[version_len:1][version:version_len bytes]
[uptime:8 BE uint64]
[peer_count:4 BE uint32]
[namespace_count:4 BE uint32]
[total_blobs:8 BE uint64]
[storage_used:8 BE uint64]
[storage_max:8 BE uint64]
[max_blob_data_bytes:8 BE uint64]            <-- NEW (NODEINFO-01)
[max_frame_bytes:4 BE uint32]                 <-- NEW (NODEINFO-02)
[rate_limit_bytes_per_sec:8 BE uint64]        <-- NEW (NODEINFO-03, per D-03)
[max_subscriptions_per_connection:4 BE uint32] <-- NEW (NODEINFO-04)
[types_count:1 uint8]
[supported_types:types_count bytes]
```

Relevant existing state on MessageDispatcher (from db/peer/message_dispatcher.h:84-86):

```cpp
uint64_t rate_limit_bytes_per_sec_ = 0;   // set via set_rate_limits()
uint64_t rate_limit_burst_ = 0;
uint32_t max_subscriptions_ = 256;        // set via set_max_subscriptions()
```

PeerManager wires these on startup (db/peer/peer_manager.cpp:165-166) and on SIGHUP reload (lines 552-565). No changes to that wiring are needed — Phase 127 only reads the existing members.

From db/net/framing.h:14-18:

```cpp
constexpr uint32_t MAX_FRAME_SIZE = 110 * 1024 * 1024;              // 110 MiB
constexpr uint64_t MAX_BLOB_DATA_SIZE = 500ULL * 1024 * 1024;       // 500 MiB
```

Both live in namespace `chromatindb::net`. (The handler scope uses `using namespace chromatindb;` via the `wire::` references — you will need explicit `chromatindb::net::` qualifiers because `net` is a sibling namespace to `peer`.)

From db/util/endian.h:47-59 (used by the existing encoder at lines 700-716):

```cpp
void store_u32_be(uint8_t* out, uint32_t val);
void store_u64_be(uint8_t* out, uint64_t val);
```

Called as `chromatindb::util::store_u32_be(...)` / `chromatindb::util::store_u64_be(...)` in the dispatcher (see existing usage line 700).

CONTEXT.md decisions governing this plan:

- **D-01**: 4 new fixed fields inserted BEFORE `[types_count][supported_types]` to keep the single variable-length section last.
- **D-02**: Intra-group order is blob(u64) → frame(u32) → rate(u64) → subs(u32); total +24 bytes.
- **D-03**: Rate field is `rate_limit_bytes_per_sec` (u64 BE). This overrides REQUIREMENTS.md NODEINFO-03's original `u32 messages/sec` — see Plan 127-02 for the REQ text update.
- **D-04**: Source for blob/frame is the `framing.h` constant, NOT config. Phase 128 (BLOB-01/FRAME-01) changes the source; Phase 127 does not.
- **D-05**: Read the constants directly from `framing.h` — no new constructor parameter, no new lambda, no config wiring change.
- **D-13**: DO NOT touch db/PROTOCOL.md. Phase 131 DOCS-03 owns the protocol doc refresh.
- **D-14**: `supported[]` array at lines 678-687 is unchanged (no new message types in Phase 127).

Feedback hooks:
- `feedback_no_duplicate_code.md`: use `store_u64_be` / `store_u32_be` — no inline bit-shifting.
- `feedback_no_backward_compat.md`: no compat shim — this is a protocol-breaking change that pre-v4.2.0 clients will fail to decode. Expected per REQUIREMENTS.md line 94.
- `feedback_no_test_automation_in_executor_prompts.md`: this plan does NOT run the full test suite per-commit; it only runs a targeted build to prove the file compiles. Phase-level test runs are delegated to the user at the wave boundary (before 127-03/127-04 land).
</interfaces>
</context>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: Extend NodeInfoResponse encoder with 4 new capability fields</name>
  <files>db/peer/message_dispatcher.cpp</files>
  <read_first>
    - db/peer/message_dispatcher.cpp (read lines 1-30 for includes; read lines 661-733 to see the current encoder verbatim before touching it)
    - db/peer/message_dispatcher.h (note members rate_limit_bytes_per_sec_ at line 84 and max_subscriptions_ at line 86 — these are read by the new encoder)
    - db/net/framing.h (constants MAX_FRAME_SIZE line 14, MAX_BLOB_DATA_SIZE line 18 — both in namespace chromatindb::net)
    - db/util/endian.h (store_u32_be and store_u64_be signatures — already used at lines 700-716 via chromatindb::util:: qualifier)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md (D-01 insertion position, D-02 field order, D-04/D-05 source-of-truth rule, D-14 supported[] unchanged)
    - .planning/REQUIREMENTS.md (NODEINFO-01..04 — NOTE: NODEINFO-03 text still reads `rate_limit_messages_per_second u32`; the doc rewrite to `rate_limit_bytes_per_sec u64` per D-03 is Plan 127-02's job. This plan ships the D-03 version.)
  </read_first>
  <action>
    Make exactly two edits to `db/peer/message_dispatcher.cpp`.

    **Edit A — Add include for framing.h (one new line in the include block at the top of the file).**

    The existing include block starts at line 1 with `#include "db/peer/message_dispatcher.h"` and contains project headers ordered alphabetically under `db/`. After line 13 (`#include "db/util/blob_helpers.h"`) and before line 14 (`#include "db/util/endian.h"`), insert:

    ```cpp
    #include "db/net/framing.h"
    ```

    Rationale: the insertion point puts `db/net/` between `db/logging/` (line 11) and `db/storage/` (line 12)... wait — re-check alphabetic order. The existing block is not strictly alphabetic (`db/engine/` at line 10 comes before `db/logging/` at 11 before `db/storage/` at 12 before `db/util/` at 13). Insert the new `#include "db/net/framing.h"` between line 11 (`db/logging/logging.h`) and line 12 (`db/storage/storage.h`) to preserve ascending path order. If that is awkward, inserting immediately after `db/logging/logging.h` is acceptable. What matters: the include must be present BEFORE the first reference to `chromatindb::net::MAX_BLOB_DATA_SIZE` (which lands in Edit B).

    **Edit B — Extend the NodeInfoResponse encoder inside the existing `TransportMsgType_NodeInfoRequest` branch (lines 661-733).**

    At the current line 691 (after this plan lands, the line numbers shift by 1 due to Edit A, but the semantic location is unchanged — inside the `try` block, right after `storage_max` is computed and the `supported[]` array is declared):

    1. **Extend the `resp_size` calculation at lines 690-692.** Current:

       ```cpp
       size_t resp_size = 1 + version.size()
                        + 8 + 4 + 4 + 8 + 8 + 8
                        + 1 + types_count;
       ```

       Replace the numeric row so the fixed-section subtotal grows by +24 bytes (8+4+8+4). Use this form (readable, preserves grouping):

       ```cpp
       size_t resp_size = 1 + version.size()
                        + 8 + 4 + 4 + 8 + 8 + 8   // uptime + peers + ns + total_blobs + storage_used + storage_max
                        + 8 + 4 + 8 + 4            // NODEINFO-01..04: blob + frame + rate + subs
                        + 1 + types_count;         // types_count + supported[] tail
       ```

       The comment markers `NODEINFO-01..04` are acceptable at wire-encoder level because this is internal source, not user-facing output — `feedback_no_phase_leaks_in_user_strings.md` governs `cdb info` stdout and help text, not source comments.

    2. **Insert the 4 new `store_*_be` calls BETWEEN the existing `storage_max` write (currently line 715-716) and the `response[off++] = types_count;` write (currently line 718).**

       After this block (current lines 715-716):

       ```cpp
       chromatindb::util::store_u64_be(response.data() + off, storage_max);
       off += 8;
       ```

       insert the following exactly, preserving the 4-space indentation of the enclosing coroutine body:

       ```cpp
               // NODEINFO-01: max_blob_data_bytes (u64 BE, per D-04 sourced from framing.h)
               chromatindb::util::store_u64_be(response.data() + off, chromatindb::net::MAX_BLOB_DATA_SIZE);
               off += 8;

               // NODEINFO-02: max_frame_bytes (u32 BE, per D-04 sourced from framing.h)
               chromatindb::util::store_u32_be(response.data() + off, chromatindb::net::MAX_FRAME_SIZE);
               off += 4;

               // NODEINFO-03: rate_limit_bytes_per_sec (u64 BE, per D-03 — renamed/re-typed from REQ's u32 messages/sec)
               chromatindb::util::store_u64_be(response.data() + off, rate_limit_bytes_per_sec_);
               off += 8;

               // NODEINFO-04: max_subscriptions_per_connection (u32 BE)
               chromatindb::util::store_u32_be(response.data() + off, max_subscriptions_);
               off += 4;
       ```

    3. **Do NOT modify** the `supported[]` array (lines 678-687) per D-14, the `types_count` byte (line 718), or the `supported[]` memcpy (line 719). The `co_await conn->send_message(...)` call at line 721-722 stays unchanged.

    4. **Do NOT** add new constructor parameters, new callbacks, new lambdas, or new header declarations. The encoder reads `MAX_BLOB_DATA_SIZE` / `MAX_FRAME_SIZE` via `#include "db/net/framing.h"` and reads `rate_limit_bytes_per_sec_` / `max_subscriptions_` as plain `this->`-qualified member access (both are already state on MessageDispatcher per message_dispatcher.h:84,86; PeerManager already seeds them on start and SIGHUP per peer_manager.cpp:165-166 and 552-565 — no change to that wiring).

    5. **Do NOT touch** `db/PROTOCOL.md` (D-13 — Phase 131 owns protocol doc changes).

    6. **Do NOT touch** any test file. VERI-02 test extension lives in Plan 127-04, which depends on this plan.
  </action>
  <verify>
    <automated>
      cmake --build build-debug -j$(nproc) --target chromatindb
    </automated>
  </verify>
  <acceptance_criteria>
    All of the following grep / file checks must pass from the repo root:

    1. Include added (exactly one new line, no duplicates):
       `grep -c '^#include "db/net/framing.h"$' db/peer/message_dispatcher.cpp` == 1

    2. Four new fixed-width writes present exactly once each in the file:
       `grep -c 'store_u64_be(response.data() + off, chromatindb::net::MAX_BLOB_DATA_SIZE)' db/peer/message_dispatcher.cpp` == 1
       `grep -c 'store_u32_be(response.data() + off, chromatindb::net::MAX_FRAME_SIZE)' db/peer/message_dispatcher.cpp` == 1
       `grep -c 'store_u64_be(response.data() + off, rate_limit_bytes_per_sec_)' db/peer/message_dispatcher.cpp` == 1
       `grep -c 'store_u32_be(response.data() + off, max_subscriptions_)' db/peer/message_dispatcher.cpp` == 1

    3. resp_size row grew by the `+ 8 + 4 + 8 + 4` group:
       `grep -c '+ 8 + 4 + 8 + 4' db/peer/message_dispatcher.cpp` >= 1

    4. No new constructor params / lambdas / header declarations were added (verify message_dispatcher.h unchanged):
       `git diff --stat db/peer/message_dispatcher.h` prints no lines (or just path with 0 insertions / 0 deletions — i.e. the header is NOT modified by this plan)

    5. No inline bit-packing (feedback_no_duplicate_code.md):
       `grep -En '>> (56|48|40|32|24|16|8)[^0-9]' db/peer/message_dispatcher.cpp | grep -v 'util/endian' | wc -l` returns only matches that already existed pre-plan (no NEW inline byte-packing introduced by this plan). Informal: only the existing `store_*_be` helpers are used for the new fields.

    6. PROTOCOL.md is NOT modified (D-13):
       `git diff --stat db/PROTOCOL.md` prints no changes.

    7. `supported[]` array unchanged (D-14). The sequence `5, 6, 7, 8,\n` on a line that starts the array literal must still be present:
       `grep -c '5, 6, 7, 8,' db/peer/message_dispatcher.cpp` == 1

    8. Build acceptance: the file compiles cleanly as part of the chromatindb target. Use `cmake --build build-debug -j$(nproc) --target chromatindb` (user preference: `-j$(nproc)`, NEVER `--parallel`). Exit code 0.

    9. Order of field writes matches D-02 (blob, frame, rate, subs). Confirm by checking that in the file, the first match of `MAX_BLOB_DATA_SIZE` appears at a smaller line number than the first match of `MAX_FRAME_SIZE`, which is smaller than the first match of `rate_limit_bytes_per_sec_` inside the resp encoder, which is smaller than the first match of `max_subscriptions_` inside the resp encoder. Quick check:
       `awk '/MAX_BLOB_DATA_SIZE/{print NR; exit}' db/peer/message_dispatcher.cpp` < `awk '/MAX_FRAME_SIZE/{print NR; exit}' db/peer/message_dispatcher.cpp` (both integers, strictly increasing). Repeat for the `rate_limit_bytes_per_sec_` and `max_subscriptions_` first-match line numbers inside the NodeInfoResponse handler (lines ~661-740 post-plan).
  </acceptance_criteria>
  <done>
    The NodeInfoResponse encoder writes 24 additional bytes in the order (u64 blob) (u32 frame) (u64 rate) (u32 subs) between `storage_max` and `types_count`. The file compiles. No other source file is touched. No header signature changed. No PROTOCOL.md edit. No test file edit.
  </done>
</task>

</tasks>

<threat_model>
## Trust Boundaries

| Boundary | Description |
|----------|-------------|
| post-handshake wire | The change extends an already-authenticated, AEAD-encrypted, length-prefixed response payload. The 4 new fields cross the same trust boundary as the existing `storage_max` / `storage_used` fields — i.e., they are delivered inside a `NodeInfoResponse` that traverses the authenticated post-handshake transport. |

## STRIDE Threat Register

| Threat ID | Category | Component | Disposition | Mitigation Plan |
|-----------|----------|-----------|-------------|-----------------|
| T-127-01 | I (Information Disclosure) | NodeInfoResponse encoder | accept | The 4 new fields expose public node capability values (max blob/frame caps + rate-limit cap + subscription cap). These are operationally public — their entire purpose is capability advertisement to clients and peers. No PII, no secrets, no stateful data. REQUIREMENTS.md NODEINFO-01..04 explicitly scope these as public-by-design. |
| T-127-02 | T (Tampering) | NodeInfoResponse encoder | mitigate | Response is delivered inside the existing AEAD-encrypted post-handshake frame (ChaCha20-Poly1305 with per-direction counter nonce per db/net/framing.h). Tampering is structurally prevented by the transport layer — no new mitigation needed. |
| T-127-03 | D (Denial of Service) | resp_size calculation | mitigate | The `resp_size` calc is extended by a fixed +24 bytes; it is NOT driven by attacker input (all 4 sources are server-local constants or server-local state). Buffer `response` is sized via `std::vector<uint8_t>(resp_size)` BEFORE writing, so a miscalc would trip AddressSanitizer / Valgrind in CI rather than silently corrupt memory. Plan 127-04's wire-size assertion (D-10) is the external canary. |
</threat_model>

<verification>
- File compiles under `cmake --build build-debug -j$(nproc) --target chromatindb`.
- Wire-size and field-order assertions verifying the encoder change are owned by Plan 127-04 (depends on this plan). That plan will run the Catch2 [peer][nodeinfo] test fixture and assert byte-exact positions + total payload delta of +24 bytes.
- CLI decode of the new fields is owned by Plan 127-03 (depends on this plan).
- No test suite is run at per-commit granularity in this plan per `feedback_delegate_tests_to_user.md` and `feedback_no_test_automation_in_executor_prompts.md`. Build acceptance is sufficient gate; full suite is user-delegated at the wave boundary.
</verification>

<success_criteria>
1. `db/peer/message_dispatcher.cpp` contains the 4 new `store_*_be` calls between `storage_max` and `types_count` in the documented order (blob u64, frame u32, rate u64, subs u32).
2. `#include "db/net/framing.h"` is present exactly once.
3. `db/peer/message_dispatcher.h`, `db/peer/peer_manager.cpp`, `db/net/framing.h`, and `db/PROTOCOL.md` are NOT modified by this plan.
4. `chromatindb` target builds clean with `-j$(nproc)`.
5. Encoder uses existing `chromatindb::util::store_u64_be` / `store_u32_be` helpers (no inline bit-shifting — `feedback_no_duplicate_code.md`).
</success_criteria>

<output>
After completion, create `.planning/phases/127-nodeinforesponse-capability-extensions/127-01-SUMMARY.md`.
</output>
</content>
</invoke>