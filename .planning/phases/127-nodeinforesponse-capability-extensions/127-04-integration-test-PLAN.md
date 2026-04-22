---
plan: 127-04-integration-test
phase: 127
type: execute
wave: 2
depends_on: [127-01-encoder]
files_modified:
  - db/tests/peer/test_peer_manager.cpp
autonomous: true
requirements: [VERI-02, NODEINFO-01, NODEINFO-02, NODEINFO-03, NODEINFO-04]
must_haves:
  truths:
    - "The existing [peer][nodeinfo] TEST_CASE at test_peer_manager.cpp:2773 asserts all 4 new fields decode to the configured values"
    - "Boundary coverage: default config, zero values for rate+subs, max values (UINT64_MAX for rate, UINT32_MAX for subs)"
    - "A hard-coded wire-size delta assertion (+24 bytes vs the pre-Phase-127 fixed-section total) pins the encoder against silent offset drift"
    - "The test_case is NOT deleted or re-written — it is extended additively"
    - "Existing assertions (version, uptime, storage_max, types_count == 39, etc.) remain intact"
  artifacts:
    - path: "db/tests/peer/test_peer_manager.cpp"
      provides: "Extended [peer][nodeinfo] TEST_CASE covering the 4 new fields + wire-size delta"
      contains: "max_blob_data_bytes"
  key_links:
    - from: "db/tests/peer/test_peer_manager.cpp [peer][nodeinfo] TEST_CASE"
      to: "db/peer/message_dispatcher.cpp NodeInfoResponse encoder (plan 127-01)"
      via: "Test fires a real NodeInfoRequest through PeerManager and walks the response bytes"
      pattern: "chromatindb::net::MAX_BLOB_DATA_SIZE|chromatindb::net::MAX_FRAME_SIZE"
---

<objective>
Extend the existing `NodeInfoRequest returns version and node state` TEST_CASE at `db/tests/peer/test_peer_manager.cpp:2773` with VERI-02 coverage: assert all 4 new fields decode to their configured values under default config, zero boundary, and max boundary; plus a hard-coded wire-size delta assertion catching future offset drift.

Purpose: VERI-02 demands "unit tests for NodeInfoResponse encode/decode covering the four new fields." This is the canonical integration test for Phase 127 — fires a real NodeInfoRequest through the real PeerManager/MessageDispatcher and walks the response bytes.

Output: One modified test file (`db/tests/peer/test_peer_manager.cpp`) — extends an existing TEST_CASE with ~50-80 lines of assertions across 3 SECTION blocks (or 3 separate TEST_CASEs sharing the same fixture, whichever is cleaner).
</objective>

<execution_context>
@$HOME/.claude/get-shit-done/workflows/execute-plan.md
@$HOME/.claude/get-shit-done/templates/summary.md
</execution_context>

<context>
@.planning/STATE.md
@.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md
@.planning/phases/127-nodeinforesponse-capability-extensions/127-01-encoder-PLAN.md

<interfaces>
Existing TEST_CASE (db/tests/peer/test_peer_manager.cpp:2773-2906) reproduced verbatim in the read_first section below for planning context.

Summary of the existing fixture (lines 2773-2906):
- Creates a `Config cfg` with `cfg.max_storage_bytes = 1048576` (1 MiB) so `storage_max` is non-zero.
- Spins up a real `PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl)` on `127.0.0.1:0`.
- Connects a client via `chromatindb::net::Connection::create_outbound`.
- Waits for handshake, sends a real `NodeInfoRequest` with empty payload, captures the `NodeInfoResponse` payload into `info_response` under a mutex.
- Walks the response bytes manually (hand-rolled `info_response[off++]` big-endian decode), asserting version+uptime+peer_count+namespace_count+total_blobs+storage_used+storage_max+types_count+at-least-4-known-types.
- Ends at line 2906 with `pm.stop(); ioc.run_for(std::chrono::milliseconds(500));`.

Constants referenced in the new assertions:
- `chromatindb::net::MAX_BLOB_DATA_SIZE` (500 MiB, from db/net/framing.h:18) — source of `max_blob_data_bytes` per D-04.
- `chromatindb::net::MAX_FRAME_SIZE` (110 MiB, from db/net/framing.h:14) — source of `max_frame_bytes` per D-04.
- `Config::rate_limit_bytes_per_sec` (default 0, from db/config/config.h:26) — source of `rate_limit_bytes_per_sec`.
- `Config::max_subscriptions_per_connection` (default 256, from db/config/config.h:23) — source of `max_subscriptions_per_connection`.

Wire-size bookkeeping (from Plan 127-01's resp_size calc after extension):
- Pre-Phase-127 fixed-section total (counting everything except `supported[]` bytes): `1 + version.size() + 8 + 4 + 4 + 8 + 8 + 8 + 1 + types_count` — i.e., 1 (version_len) + version.size() + 8 (uptime) + 4 (peers) + 4 (ns) + 8 (total_blobs) + 8 (storage_used) + 8 (storage_max) + 1 (types_count) + types_count (supported[]).
- Post-Phase-127 fixed-section total adds `8 + 4 + 8 + 4 = 24` bytes BEFORE `types_count`.
- So for a known `version` (e.g. `"2.3.0-gd360ca17"` from db/version.h:2, length 15) and a known `types_count == 39` (from the existing assertion at line 2894), the total `info_response.size()` should equal:
  `1 + 15 + 8 + 4 + 4 + 8 + 8 + 8 + 24 + 1 + 39 = 120` bytes.
  BUT the exact version string can change between builds (it includes the git short hash), so the wire-size assertion should be phrased as DELTA rather than absolute:
  `CHECK(info_response.size() == 1 + version.size() + 8 + 4 + 4 + 8 + 8 + 8 + 24 + 1 + types_count);`
  or equivalently, the rearranged form with the literal `+ 24` signaling the Phase 127 delta (this is what D-10 specifies).

CONTEXT.md decisions governing this plan:
- **D-09**: VERI-02 is satisfied by ONE integration TEST_CASE extending the existing fixture. Three scenarios: default config values; zero-boundary (rate=0, subs=0); max-boundary (rate=UINT64_MAX, subs=UINT32_MAX).
- **D-10**: Wire-size assertion — hard-coded `+ 24` byte delta verifying the fixed section grew by exactly that amount. Catches offset drift in future phases.
- **D-11**: No CLI-side unit test. Only the node-side integration test.

Claude's-discretion items (CONTEXT.md lines 114-118):
- Test-helper extraction (`make_test_config_for_nodeinfo()`) vs inline boundary values — INLINE is preferred here. The existing test is inline; extending inline preserves readability and avoids a helper that is used exactly 3 times and never again. If a helper emerges naturally while extending, that is fine but not required.
- Tag naming: keep `[peer][nodeinfo]` as the anchor. Extended TEST_CASEs or SECTION blocks inherit or reuse this tag.

Structure choice: THREE SECTION blocks inside the existing TEST_CASE (one per boundary) vs THREE separate TEST_CASEs sharing the fixture via copy-paste. PREFER three separate TEST_CASEs because:
1. Each boundary needs a DIFFERENT `Config` (default vs zero vs max-value) — SECTION blocks inside one TEST_CASE would need to restart `PeerManager` per SECTION, which is awkward given Catch2's fixture-per-SECTION semantics.
2. The existing fixture at line 2773 stays as-is (extended with the default-path assertions + wire-size delta). Two NEW TEST_CASEs get added immediately after it for zero-boundary and max-boundary, each a ~50-line copy of the existing fixture with a tweaked Config.
3. Yes — this duplicates fixture scaffold. feedback_no_duplicate_code.md applies to production code first; for test scaffolding Catch2 idiom wins, and the duplication is local to one TEST_CASE cluster.

Feedback hooks:
- `feedback_no_duplicate_code.md` (production code) — N/A here; tests use existing `store_u64_be`-free BE walk.
- `feedback_delegate_tests_to_user.md` — this plan's own `<automated>` verify is a targeted build + run of only the `[peer][nodeinfo]` Catch2 tag filter, NOT the full suite. Full-suite runs are user-delegated at the wave boundary.
- `feedback_no_test_automation_in_executor_prompts.md` — executor MUST NOT append `&& ctest --output-on-failure` or `&& ./chromatindb_tests` with no tag filter.
- `feedback_no_or_true.md` — never suppress build/test errors with `|| true`.

Build target: `chromatindb_tests` (standard node test binary per Phase 125 Plan 04 note in STATE.md: "cli_tests 197614 assertions / 98 cases ... chromatindb_tests [peer] 506 / 77").
</interfaces>
</context>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: Extend the default-path [peer][nodeinfo] TEST_CASE with new-field assertions + wire-size delta</name>
  <files>db/tests/peer/test_peer_manager.cpp</files>
  <read_first>
    - db/tests/peer/test_peer_manager.cpp (read lines 2773-2906 — the FULL existing [peer][nodeinfo] TEST_CASE — verbatim; confirm line numbers before editing because prior patches may have shifted them)
    - db/tests/peer/test_peer_manager.cpp (read the top of the file for #include lines — confirm `#include "db/net/framing.h"` is already present or needs to be added for the MAX_BLOB_DATA_SIZE / MAX_FRAME_SIZE references; if absent, add it in the test file only)
    - db/peer/message_dispatcher.cpp (read the Plan 127-01-landed version lines 661-740 to confirm the exact post-plan wire layout this test asserts against)
    - db/config/config.h (confirm defaults: `max_subscriptions_per_connection = 256`, `rate_limit_bytes_per_sec = 0`)
    - db/net/framing.h (confirm MAX_BLOB_DATA_SIZE and MAX_FRAME_SIZE values)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md (D-09 three-scenario coverage, D-10 wire-size assertion)
  </read_first>
  <action>
    Make exactly two edits to `db/tests/peer/test_peer_manager.cpp`. All edits are inside (or immediately after) the existing `TEST_CASE("NodeInfoRequest returns version and node state", "[peer][nodeinfo]")` at line 2773.

    **Edit A — If `#include "db/net/framing.h"` is not yet present in this test file, add it.**

    Grep first: `grep -c '#include "db/net/framing.h"' db/tests/peer/test_peer_manager.cpp`. If 0, add the include near the top of the file alongside the other `db/net/` or `db/peer/` includes. If already present (>=1), skip this edit.

    **Edit B — Extend the existing TEST_CASE body (inside the lock-guarded block at lines 2835-2902).**

    After the `CHECK(storage_max == 1048576);` line (currently line 2889) and BEFORE the `// Supported types` comment (currently line 2891), insert the following assertion block. Do NOT remove the existing `// Supported types` section or the `types_count == 39` check — they stay as the tail.

    Note: the existing decoder walks bytes manually via `info_response[off++]`. The new block follows the same idiom (no `store_u64_be` — we are asserting on wire bytes by value, which means we decode them by hand from `info_response` for the assertion).

    ```cpp
            // Phase 127 wire extension — 4 new fixed-width fields BEFORE [types_count][supported_types]
            // per D-01 insertion point and D-02 order.

            // max_blob_data_bytes (8 BE) — sourced from chromatindb::net::MAX_BLOB_DATA_SIZE per D-04
            REQUIRE(off + 8 <= info_response.size());
            uint64_t max_blob_data_bytes = 0;
            for (int i = 0; i < 8; ++i)
                max_blob_data_bytes = (max_blob_data_bytes << 8) | info_response[off++];
            CHECK(max_blob_data_bytes == chromatindb::net::MAX_BLOB_DATA_SIZE);

            // max_frame_bytes (4 BE) — sourced from chromatindb::net::MAX_FRAME_SIZE per D-04
            REQUIRE(off + 4 <= info_response.size());
            uint32_t max_frame_bytes = 0;
            for (int i = 0; i < 4; ++i)
                max_frame_bytes = (max_frame_bytes << 8) | info_response[off++];
            CHECK(max_frame_bytes == chromatindb::net::MAX_FRAME_SIZE);

            // rate_limit_bytes_per_sec (8 BE) — default config is 0
            REQUIRE(off + 8 <= info_response.size());
            uint64_t rate_limit_bytes_per_sec = 0;
            for (int i = 0; i < 8; ++i)
                rate_limit_bytes_per_sec = (rate_limit_bytes_per_sec << 8) | info_response[off++];
            CHECK(rate_limit_bytes_per_sec == 0);

            // max_subscriptions_per_connection (4 BE) — default config is 256
            REQUIRE(off + 4 <= info_response.size());
            uint32_t max_subscriptions = 0;
            for (int i = 0; i < 4; ++i)
                max_subscriptions = (max_subscriptions << 8) | info_response[off++];
            CHECK(max_subscriptions == 256);
    ```

    After the existing final `CHECK(types.count(39));` line (currently line 2901) and BEFORE the closing `}` of the lock-guarded block (currently line 2902), insert the wire-size delta assertion (D-10):

    ```cpp
            // D-10: Phase 127 wire-size invariant — the fixed section grew by exactly 24 bytes
            // vs the pre-Phase-127 layout (+ 8 blob + 4 frame + 8 rate + 4 subs = + 24).
            // Hard-coded to catch offset drift if a future change reorders or re-adds fields.
            CHECK(info_response.size() ==
                  1 + version.size()           // version_len + version bytes
                  + 8 + 4 + 4 + 8 + 8 + 8      // uptime + peers + ns + total + used + max
                  + 24                          // Phase 127 delta: blob + frame + rate + subs
                  + 1 + types_count);           // types_count + supported[]
    ```

    **Do NOT** modify any existing assertion in the fixture. Do NOT delete the supported-types checks. Do NOT renumber request IDs.

    **Task 2 deliverables** (below) add two SEPARATE TEST_CASEs immediately AFTER this fixture, covering the zero and max boundaries.
  </action>
  <verify>
    <automated>
      cmake --build build-debug -j$(nproc) --target chromatindb_tests && ./build-debug/chromatindb_tests "[peer][nodeinfo]"
    </automated>
  </verify>
  <acceptance_criteria>
    1. Test file still has the original TEST_CASE name present exactly once:
       `grep -c 'NodeInfoRequest returns version and node state' db/tests/peer/test_peer_manager.cpp` == 1

    2. New max_blob_data_bytes assertion present:
       `grep -c 'max_blob_data_bytes == chromatindb::net::MAX_BLOB_DATA_SIZE' db/tests/peer/test_peer_manager.cpp` == 1

    3. New max_frame_bytes assertion present:
       `grep -c 'max_frame_bytes == chromatindb::net::MAX_FRAME_SIZE' db/tests/peer/test_peer_manager.cpp` == 1

    4. Default-config rate and subs assertions present:
       `grep -c 'rate_limit_bytes_per_sec == 0' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'max_subscriptions == 256' db/tests/peer/test_peer_manager.cpp` >= 1

    5. Wire-size delta (+ 24) assertion present (D-10):
       `grep -c '+ 24' db/tests/peer/test_peer_manager.cpp` >= 1
       AND one of those matches is on a line referencing `info_response.size()`:
       `grep -B1 '+ 24' db/tests/peer/test_peer_manager.cpp | grep -c 'info_response.size()'` >= 1

    6. Framing header included if needed:
       `grep -c '#include "db/net/framing.h"' db/tests/peer/test_peer_manager.cpp` >= 1

    7. No existing assertion deleted — the original types_count == 39 check and the supported-types set checks still exist:
       `grep -c 'types_count == 39' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'types.count(39)' db/tests/peer/test_peer_manager.cpp` >= 1

    8. Build + targeted test run acceptance — `chromatindb_tests` binary builds with `-j$(nproc)`, NEVER `--parallel`:
       `cmake --build build-debug -j$(nproc) --target chromatindb_tests` exits 0.
       `./build-debug/chromatindb_tests "[peer][nodeinfo]"` exits 0 (at least the original + extended default-path TEST_CASE passes; Task 2 adds zero/max boundary TEST_CASEs that also match the `[peer][nodeinfo]` tag and run in this same invocation).

    9. Test file is the only modified file:
       `git diff --name-only | sort` produces exactly `db/tests/peer/test_peer_manager.cpp` (plus any Task-2 additions, which also touch the same file).
  </acceptance_criteria>
  <done>
    The existing `[peer][nodeinfo]` TEST_CASE now asserts: (a) max_blob_data_bytes == MAX_BLOB_DATA_SIZE, (b) max_frame_bytes == MAX_FRAME_SIZE, (c) rate_limit_bytes_per_sec == 0 (default), (d) max_subscriptions == 256 (default), (e) wire-size delta == + 24 bytes vs pre-Phase-127. Test compiles and the default-path case passes under the targeted Catch2 tag filter.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 2: Add two boundary TEST_CASEs — zero values and max values — for rate+subs</name>
  <files>db/tests/peer/test_peer_manager.cpp</files>
  <read_first>
    - db/tests/peer/test_peer_manager.cpp (read the post-Task-1 version of the [peer][nodeinfo] TEST_CASE to use as the template for copy-paste)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md (D-09 boundary scenarios: zero-value + max-value)
  </read_first>
  <action>
    Immediately AFTER the closing `}` of the Task-1-extended TEST_CASE (currently near line 2906 before Phase 127, will shift by ~40 lines after Task 1's additions), insert two new `TEST_CASE` blocks, each a near-copy of the fixture with a different `Config` and different expected values.

    Both new TEST_CASEs share the tag `[peer][nodeinfo]` so they run under the same filter as the existing case. Use unambiguous names to avoid Catch2 name collisions:

    **TEST_CASE 2 — zero boundary (rate=0, subs=0):**

    ```cpp
    TEST_CASE("NodeInfoResponse — zero boundary for rate_limit and max_subscriptions", "[peer][nodeinfo]") {
        TempDir tmp;
        auto server_id = NodeIdentity::load_or_generate(tmp.path);
        auto client_id = NodeIdentity::generate();

        Config cfg;
        cfg.bind_address = "127.0.0.1:0";
        cfg.data_dir = tmp.path.string();
        cfg.max_storage_bytes = 1048576;
        cfg.rate_limit_bytes_per_sec = 0;              // D-09 zero boundary
        cfg.max_subscriptions_per_connection = 0;      // D-09 zero boundary ("unlimited" on the wire)

        Storage store(tmp.path.string());
        asio::thread_pool pool{1};
        BlobEngine eng(store, pool);
        chromatindb::test::register_pubk(store, server_id);
        chromatindb::test::register_pubk(store, client_id);

        asio::io_context ioc;
        AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
        PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
        pm.start();
        ioc.run_for(std::chrono::milliseconds(200));
        auto pm_port = pm.listening_port();

        chromatindb::net::Connection::Ptr client_conn;
        std::mutex mtx;
        std::vector<uint8_t> info_response;

        asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
            asio::ip::tcp::socket socket(ioc);
            auto [ec] = co_await socket.async_connect(
                asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
                chromatindb::net::use_nothrow);
            if (ec) co_return;
            client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
            client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t) {
                if (type == chromatindb::wire::TransportMsgType_NodeInfoResponse) {
                    std::lock_guard<std::mutex> lock(mtx);
                    info_response = std::move(payload);
                }
            });
            co_await client_conn->run();
        }, asio::detached);

        asio::steady_timer send_timer(ioc);
        send_timer.expires_after(std::chrono::seconds(2));
        send_timer.async_wait([&](asio::error_code ec) {
            if (ec) return;
            if (client_conn && client_conn->is_authenticated()) {
                asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                    co_await client_conn->send_message(
                        chromatindb::wire::TransportMsgType_NodeInfoRequest, {}, 78);
                }, asio::detached);
            }
        });

        ioc.run_for(std::chrono::seconds(5));

        {
            std::lock_guard<std::mutex> lock(mtx);
            REQUIRE(!info_response.empty());

            // Walk to the 4 new fields. Fast-forward through the existing fixed section:
            size_t off = 0;
            uint8_t version_len = info_response[off++];
            off += version_len;                          // version
            off += 8 + 4 + 4 + 8 + 8 + 8;                // uptime + peers + ns + total + used + max

            // max_blob_data_bytes (8 BE)
            REQUIRE(off + 8 <= info_response.size());
            off += 8;                                    // skip blob — not asserted in this case
            // max_frame_bytes (4 BE)
            REQUIRE(off + 4 <= info_response.size());
            off += 4;                                    // skip frame — not asserted in this case

            // rate_limit_bytes_per_sec (8 BE) — ZERO BOUNDARY
            REQUIRE(off + 8 <= info_response.size());
            uint64_t rate = 0;
            for (int i = 0; i < 8; ++i) rate = (rate << 8) | info_response[off++];
            CHECK(rate == 0);

            // max_subscriptions_per_connection (4 BE) — ZERO BOUNDARY
            REQUIRE(off + 4 <= info_response.size());
            uint32_t subs = 0;
            for (int i = 0; i < 4; ++i) subs = (subs << 8) | info_response[off++];
            CHECK(subs == 0);
        }

        pm.stop();
        ioc.run_for(std::chrono::milliseconds(500));
    }
    ```

    **TEST_CASE 3 — max boundary (rate=UINT64_MAX, subs=UINT32_MAX):**

    Copy TEST_CASE 2 and change exactly 4 things:
    - Title string: `"NodeInfoResponse — max boundary for rate_limit and max_subscriptions"`
    - `cfg.rate_limit_bytes_per_sec = UINT64_MAX;`
    - `cfg.max_subscriptions_per_connection = UINT32_MAX;`
    - Final `CHECK` lines become:
      ```cpp
      CHECK(rate == UINT64_MAX);
      // ...
      CHECK(subs == UINT32_MAX);
      ```
    - Optional: bump the `send_message(..., 79);` request ID so each test has a unique ID for easier debugging (not required by Catch2 — requests in separate TEST_CASEs run on separate ioc instances).

    Required `#include <cstdint>` for UINT64_MAX/UINT32_MAX — verify it is already included via a transitive include in the test file; if not, add it near the top.

    **Do NOT**:
    - Modify Task 1's changes.
    - Touch any non-test file.
    - Touch db/PROTOCOL.md.
    - Run the full test suite per-commit (targeted tag filter only — feedback_delegate_tests_to_user.md).
  </action>
  <verify>
    <automated>
      cmake --build build-debug -j$(nproc) --target chromatindb_tests && ./build-debug/chromatindb_tests "[peer][nodeinfo]"
    </automated>
  </verify>
  <acceptance_criteria>
    1. Three TEST_CASE blocks exist with the `[peer][nodeinfo]` tag (original + two new):
       `grep -c '"\[peer\]\[nodeinfo\]"' db/tests/peer/test_peer_manager.cpp` >= 3

    2. Zero-boundary TEST_CASE has the distinctive config fields AND the distinctive assertions:
       `grep -c 'rate_limit_bytes_per_sec = 0' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'max_subscriptions_per_connection = 0' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'zero boundary' db/tests/peer/test_peer_manager.cpp` >= 1

    3. Max-boundary TEST_CASE has the distinctive config fields AND the distinctive assertions:
       `grep -c 'rate_limit_bytes_per_sec = UINT64_MAX' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'max_subscriptions_per_connection = UINT32_MAX' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'rate == UINT64_MAX' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'subs == UINT32_MAX' db/tests/peer/test_peer_manager.cpp` >= 1
       `grep -c 'max boundary' db/tests/peer/test_peer_manager.cpp` >= 1

    4. Catch2 TEST_CASE names are unique (grep by unique name fragments):
       `grep -c 'NodeInfoResponse — zero boundary' db/tests/peer/test_peer_manager.cpp` == 1
       `grep -c 'NodeInfoResponse — max boundary' db/tests/peer/test_peer_manager.cpp` == 1

    5. Build + targeted test run passes (all 3 TEST_CASEs under the `[peer][nodeinfo]` tag):
       `cmake --build build-debug -j$(nproc) --target chromatindb_tests` exits 0.
       `./build-debug/chromatindb_tests "[peer][nodeinfo]"` exits 0. The output should report at least 3 test cases under this tag (Catch2 prints `3 test cases` or similar).

    6. Only `db/tests/peer/test_peer_manager.cpp` modified by this plan:
       `git diff --name-only | sort` produces exactly `db/tests/peer/test_peer_manager.cpp`.
  </acceptance_criteria>
  <done>
    Three TEST_CASEs under `[peer][nodeinfo]` (default / zero / max) all compile and all pass under the targeted Catch2 tag filter. VERI-02 is structurally satisfied.
  </done>
</task>

</tasks>

<threat_model>
## Trust Boundaries

| Boundary | Description |
|----------|-------------|
| test-harness-to-node | The Catch2 tests spin up a real PeerManager/MessageDispatcher in-process and connect a real-wire client via loopback TCP. The boundary crossed is the same one `cdb` crosses in production — the AEAD post-handshake transport — but inside a single process. |

## STRIDE Threat Register

| Threat ID | Category | Component | Disposition | Mitigation Plan |
|-----------|----------|-----------|-------------|-----------------|
| T-127-08 | T (Tampering — offset drift) | Wire-size delta assertion | mitigate | D-10's `+ 24` hard-coded delta in the size check will fail loudly if a future encoder change reorders or adds/removes any of the 4 new fields without also updating this test. This is precisely the guard the CONTEXT document asks for. |
| T-127-09 | D (flaky test, ioc timing) | New boundary TEST_CASEs | accept | Both boundary TEST_CASEs use the same `ioc.run_for(std::chrono::seconds(5))` pattern as the existing fixture, which has been stable in CI through Phase 125/126. No new timing risk introduced. |
</threat_model>

<verification>
- Targeted Catch2 run: `./build-debug/chromatindb_tests "[peer][nodeinfo]"` runs exactly the 3 TEST_CASEs touched by this plan and exits 0.
- Full-suite run is delegated to the user at the wave boundary.
- The wire-size delta assertion (D-10) catches silent byte-count drift in future phases.
</verification>

<success_criteria>
1. Three `[peer][nodeinfo]` TEST_CASEs exist (default / zero boundary / max boundary).
2. Default TEST_CASE asserts all 4 new fields decode to their configured values AND the total payload size is `fixed_section + 24 + types_count + 1 + version.size()`.
3. Zero-boundary TEST_CASE asserts rate == 0, subs == 0 with `cfg.rate_limit_bytes_per_sec = 0` and `cfg.max_subscriptions_per_connection = 0`.
4. Max-boundary TEST_CASE asserts rate == UINT64_MAX, subs == UINT32_MAX with the matching config.
5. `./build-debug/chromatindb_tests "[peer][nodeinfo]"` exits 0.
6. Only `db/tests/peer/test_peer_manager.cpp` modified.
</success_criteria>

<output>
After completion, create `.planning/phases/127-nodeinforesponse-capability-extensions/127-04-SUMMARY.md`.
</output>
</content>
</invoke>