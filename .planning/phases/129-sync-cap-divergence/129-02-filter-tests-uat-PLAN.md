---
plan: 129-02-filter-tests-uat
phase: 129
type: execute
wave: 2
depends_on: [129-01-peerinfo-snapshot-counter]
files_modified:
  - db/peer/blob_push_manager.cpp
  - db/peer/blob_push_manager.h
  - db/sync/reconciliation.cpp
  - db/tests/peer/test_sync_cap_filter.cpp
  - .planning/phases/129-sync-cap-divergence/129-UAT.md
autonomous: true
requirements: [SYNC-02, SYNC-03, VERI-03, VERI-05]
must_haves:
  truths:
    - "Blobs whose size > peer.advertised_blob_cap are omitted from BlobNotify fan-out to that peer (cap>0 guard — cap==0 means do not skip)"
    - "Blobs whose size > peer.advertised_blob_cap are not sent in BlobFetch responses to that peer; response is either skipped with not-available or a minimal reject indicator, matching the existing not-found semantic"
    - "PULL set-reconciliation announce omits blob fingerprints that exceed the remote peer's cap; counter increments per-blob-skipped"
    - "Each of the 3 filter sites calls metrics_collector_.increment_sync_skipped_oversized(peer_addr) exactly once per skip event"
    - "VERI-03: 4 scenarios (peer cap smaller / larger / equal / zero) × 3 filter sites = 12 TEST_CASEs or equivalent SECTIONs passing under tag [sync][cap-filter]"
    - "VERI-05: 129-UAT.md documents the 2-node cdb --node home / --node local scenario with expected assertions; status=pending"
  artifacts:
    - path: "db/peer/blob_push_manager.cpp"
      provides: "BlobNotify fan-out filter + BlobFetch response filter"
    - path: "db/sync/reconciliation.cpp"
      provides: "announce-set filter"
    - path: "db/tests/peer/test_sync_cap_filter.cpp"
      provides: "12 TEST_CASEs covering the 4 scenarios × 3 sites under tag [sync][cap-filter]"
    - path: ".planning/phases/129-sync-cap-divergence/129-UAT.md"
      provides: "Manual 2-node test procedure for VERI-05"
---

<objective>
Apply the cap-divergence filter at the 3 sync-out paths, increment the per-peer skip counter, cover it with unit tests (VERI-03), and document the 2-node manual UAT (VERI-05 — user-runnable).
</objective>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: Filter helper + 3 filter sites + counter increments</name>
  <files>db/peer/blob_push_manager.cpp, db/peer/blob_push_manager.h, db/sync/reconciliation.cpp</files>
  <read_first>
    - db/peer/blob_push_manager.cpp:55-90 (BlobNotify fan-out loop — exact site for filter)
    - db/peer/blob_push_manager.cpp:143-172 (BlobFetch response path)
    - db/peer/blob_push_manager.h (existing helper signatures; prefer adding new helper as a private member)
    - db/sync/reconciliation.cpp (full file; find where fingerprints are enumerated per peer announce — this is the PULL announce site)
    - db/peer/peer_types.h (PeerInfo layout, new advertised_blob_cap field from Wave 1)
    - db/peer/metrics_collector.h (increment_sync_skipped_oversized signature from Wave 1)
  </read_first>
  <action>
    1. **Add a free helper** near the top of `db/peer/blob_push_manager.cpp` (or in an anonymous namespace to keep it local):
       ```cpp
       namespace {
       // Returns true iff this blob should be skipped for the given peer based on the peer's
       // advertised blob cap. cap == 0 means "unknown" — DO NOT skip (CONTEXT.md D-01).
       inline bool should_skip_for_peer_cap(uint64_t blob_size, uint64_t advertised_blob_cap) {
           return advertised_blob_cap > 0 && blob_size > advertised_blob_cap;
       }
       } // anonymous namespace
       ```
       If you prefer, put it in `blob_push_manager.h` as a `static inline` free function so `reconciliation.cpp` can use the same helper instead of duplicating. Pick the less intrusive option; duplication of one 3-line function across two TUs is acceptable if header inclusion would drag in peer_types.h into reconciliation.cpp.

    2. **BlobNotify fan-out filter** — in the existing loop at `blob_push_manager.cpp:63-85`, after the existing source/role/namespace checks, add:
       ```cpp
       if (should_skip_for_peer_cap(blob_size, peer->advertised_blob_cap)) {
           metrics_collector_.increment_sync_skipped_oversized(peer->address);
           spdlog::debug("BlobNotify skip: blob {} bytes > peer {} cap {} bytes",
                         blob_size, peer->address, peer->advertised_blob_cap);
           continue;
       }
       ```
       Place this check after existing `continue` statements so other filters take priority. `blob_size` is already available as a parameter to the emission function.

    3. **BlobFetch response filter** — in the BlobFetch handler at `blob_push_manager.cpp:143-172`, after the blob has been loaded from storage and before it's sent to the requester, check the requester's cap. Inject:
       ```cpp
       const auto requester = find_peer_info(conn);  // or whatever accessor exists
       if (requester && should_skip_for_peer_cap(blob.data.size(), requester->advertised_blob_cap)) {
           metrics_collector_.increment_sync_skipped_oversized(requester->address);
           spdlog::debug("BlobFetch skip: blob {} bytes > requester {} cap {} bytes",
                         blob.data.size(), requester->address, requester->advertised_blob_cap);
           // Send a not-available response instead — reuse the existing not-found branch.
           co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse, /* not-available payload */);
           co_return;
       }
       ```
       Mirror the existing "expired" or "not-found" response format at lines 153-155 and 168-172 to stay consistent.

    4. **PULL set-reconciliation announce filter** — in `db/sync/reconciliation.cpp`, find the function that builds the fingerprint set or sends announce fingerprints per peer (search for `fingerprint`, `announce`, or `send_message` in this file). For every blob fingerprint enumerated, add the same size-vs-cap check. On skip:
       ```cpp
       metrics_collector_.increment_sync_skipped_oversized(peer_address);
       ```
       The exact insertion point depends on the current reconciliation structure; the executor reads the file and places the check where per-blob iteration happens. If `reconciliation.cpp` does not currently have access to `metrics_collector_`, pass it through via the existing dispatcher/orchestrator path — do NOT introduce a global. Minimal plumbing only.

    5. **No enforcement at other sites.** Do NOT add cap checks to read path, direct blob reads, GC, compaction. Phase 128 D-14/D-16 enforcement boundary stays intact; Phase 129 only adds sync-out filtering.

    6. **Targeted build gate:** `cmake --build build-debug -j$(nproc) --target chromatindb` exits 0.
  </action>
  <verify>
    <automated>
      grep -c 'should_skip_for_peer_cap' db/peer/blob_push_manager.cpp
      # ^ >= 2 (definition + call, or 2 calls if helper is in header)
      grep -c 'increment_sync_skipped_oversized' db/peer/blob_push_manager.cpp db/sync/reconciliation.cpp
      # ^ >= 3 total across both files (BlobNotify + BlobFetch + reconcile)
      grep -c 'advertised_blob_cap' db/peer/blob_push_manager.cpp db/sync/reconciliation.cpp
      # ^ >= 3 (read sites)
      cmake --build build-debug -j$(nproc) --target chromatindb 2>&1 | tail -3
    </automated>
  </verify>
  <acceptance_criteria>
    - Helper `should_skip_for_peer_cap` exists (anonymous namespace OR header) — single implementation.
    - 3 filter sites: BlobNotify fan-out, BlobFetch response, PULL reconciliation announce.
    - Each site calls `increment_sync_skipped_oversized(peer_address)` exactly once per skipped blob.
    - cap == 0 path: the helper returns false (do not skip). cap == blob_size path: false (boundary: `>` not `>=`).
    - No cap checks introduced at read/GC/compaction — Phase 128 enforcement boundary preserved.
    - `chromatindb` target builds clean.
  </acceptance_criteria>
  <done>
    All 3 sync-out paths respect peer's advertised cap; skips are counted per peer; the `chromatindb_sync_skipped_oversized_total{peer=...}` Prometheus counter now has live data.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 2: Unit tests (VERI-03) — 4 scenarios × 3 sites</name>
  <files>db/tests/peer/test_sync_cap_filter.cpp</files>
  <read_first>
    - db/tests/peer/test_peer_manager.cpp (test fixture patterns for PeerManager/BlobPushManager wiring; reuse as much scaffold as possible)
    - db/tests/peer/test_metrics_endpoint.cpp (scrape-assertion patterns for the counter check)
    - db/peer/blob_push_manager.cpp + db/sync/reconciliation.cpp (the 3 filter sites that need coverage)
    - db/peer/peer_types.h (PeerInfo fabrication)
  </read_first>
  <action>
    Create a new test file `db/tests/peer/test_sync_cap_filter.cpp` under the `[sync][cap-filter]` Catch2 tag. Share a common fixture that:
    - Spins up a minimal `PeerManager` + `MetricsCollector` with 2 mock peers (A, B).
    - Peer A: `advertised_blob_cap = 1 MiB`.
    - Peer B: `advertised_blob_cap = 8 MiB`.
    - (additional fixtures for cap=0 and cap=equal scenarios)

    **The 4 cap-divergence scenarios per VERI-03:**
    - `"peer cap smaller — blob skipped + counter increments"` — feed a 6 MiB blob; assert peer A skipped, counter `{peer="A"}` incremented.
    - `"peer cap larger — blob sent normally"` — feed a 6 MiB blob; assert peer B received it (mock connection captures the send), counter `{peer="B"}` unchanged.
    - `"peer cap equal — boundary test (blob_size == cap)"` — feed a 1 MiB blob to peer A (cap=1 MiB); assert it is SENT (boundary is `>` not `>=`), no counter increment.
    - `"peer cap zero — unknown cap MUST NOT skip"` — set peer C to `advertised_blob_cap=0`; feed an 8 MiB blob; assert it IS sent, no counter increment.

    **Per filter site:** repeat the 4 scenarios for each of BlobNotify fan-out, BlobFetch response, and PULL announce. Use SECTIONs within TEST_CASEs OR three TEST_CASEs — whichever is less boilerplate. 12 logical assertions total, minimum.

    If spinning up a full PeerManager is too heavy, test each filter site in isolation:
    - BlobNotify: call the fan-out function directly with mocked `peers_` vector.
    - BlobFetch: call the fetch handler with a mock incoming request.
    - Reconciliation announce: call the filter helper + counter increment in a unit-isolated way.

    **Build + targeted run:**
    - `cmake --build build-debug -j$(nproc) --target chromatindb_tests` passes.
    - `./build-debug/db/chromatindb_tests "[sync][cap-filter]"` exits 0 with all assertions passing.

    Do NOT run the full test suite (user-delegated). Do NOT write an in-process 2-node integration test — that's VERI-05 and it's UAT-delegated.
  </action>
  <verify>
    <automated>
      grep -c 'TEST_CASE.*\[sync\]\[cap-filter\]' db/tests/peer/test_sync_cap_filter.cpp
      # ^ >= 1 (could be 1 with SECTIONs, or several separate TEST_CASEs)
      grep -c 'advertised_blob_cap' db/tests/peer/test_sync_cap_filter.cpp
      # ^ multiple (setting caps on mock peers)
      cmake --build build-debug -j$(nproc) --target chromatindb_tests 2>&1 | tail -3
      ./build-debug/db/chromatindb_tests "[sync][cap-filter]" 2>&1 | tail -3
    </automated>
  </verify>
  <acceptance_criteria>
    - New test file at `db/tests/peer/test_sync_cap_filter.cpp` exists.
    - Catch2 tag `[sync][cap-filter]` covers at least 12 logical assertions (4 scenarios × 3 sites).
    - All assertions pass when run with the tag filter.
    - `chromatindb_tests` target compiles clean.
  </acceptance_criteria>
  <done>
    VERI-03 closed. The filter has automated regression coverage.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 3: VERI-05 UAT markdown for user-delegated 2-node scenario</name>
  <files>.planning/phases/129-sync-cap-divergence/129-UAT.md</files>
  <read_first>
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-HUMAN-UAT.md (if exists — reference for tone/format)
    - reference_live_node memory note (user has `--node home` and `--node local` infra)
  </read_first>
  <action>
    Create `.planning/phases/129-sync-cap-divergence/129-UAT.md`:

    ```markdown
    ---
    status: pending
    phase: 129-sync-cap-divergence
    requirement: VERI-05
    delegated_to: user
    reason: "User runs a 2-node deployment (cdb --node home / --node local); in-process fixture not required (CONTEXT.md D-09)."
    updated: 2026-04-23
    ---

    # Phase 129 UAT: 2-node cap divergence (VERI-05)

    **Goal:** Prove that a node with a smaller `blob_max_bytes` cap silently skips oversized blobs from a larger-cap peer, the skip counter increments, and sub-cap blobs still replicate normally.

    **Topology:**
    - Node A (smaller cap): `blob_max_bytes = 1 MiB` — this is the "home" node for this test.
    - Node B (larger cap): `blob_max_bytes = 8 MiB` — this is the "local" node.
    - Both peered in a `trusted_peers` mesh.

    ## Setup

    On node A (home): ensure `config.json` has `"blob_max_bytes": 1048576` (1 MiB). Send SIGHUP to apply.
    On node B (local): ensure `config.json` has `"blob_max_bytes": 8388608` (8 MiB). Send SIGHUP to apply.

    ## Test 1: Sub-cap blob replicates normally

    Write a 256 KiB blob to node B:

        cdb --node local put my-ns some-key --size 256K

    Wait ~1 sync round (e.g., 10s).

    Verify on node A:

        cdb --node home get my-ns some-key

    **Expected:** blob retrieved with matching SHA3-256 hash.

    ## Test 2: Over-A's-cap blob does NOT replicate to A

    Write a 6 MiB blob to node B:

        cdb --node local put my-ns big-blob --size 6M

    **Expected on B:** accepted (B's cap is 8 MiB).

    Wait ~1 sync round.

    On A, attempt retrieval:

        cdb --node home get my-ns big-blob

    **Expected:** not-found / blob absent on A.

    ## Test 3: Skip counter incremented on A

    Scrape A's `/metrics`:

        curl -s http://<node-A-metrics-bind>/metrics | grep sync_skipped_oversized

    **Expected:** `chromatindb_sync_skipped_oversized_total{peer="<B-address>"} N` where `N >= 1` (at least one skip recorded from Test 2's 6 MiB blob offer).

    ## Test 4: Direct BlobFetch from A for the 6 MiB blob

    Force A to request the big blob from B directly (exact mechanism depends on current `cdb` surface; if no direct fetch exists, skip this test).

    **Expected:** B refuses to send; skip counter on B increments with `{peer="<A-address>"}`.

    ## Results

    status: [ ] passed / [ ] failed
    notes:
    ```

    Do NOT include any actual test execution — this file is for the user to complete.
  </action>
  <verify>
    <automated>
      test -f .planning/phases/129-sync-cap-divergence/129-UAT.md
      grep -c 'VERI-05' .planning/phases/129-sync-cap-divergence/129-UAT.md
      grep -c 'status: pending' .planning/phases/129-sync-cap-divergence/129-UAT.md
    </automated>
  </verify>
  <acceptance_criteria>
    - UAT file exists with `status: pending` and `requirement: VERI-05` frontmatter.
    - 4 test steps documented with expected outcomes.
    - `--node home` and `--node local` CLI forms used (match user's existing infra).
  </acceptance_criteria>
  <done>
    VERI-05 has a concrete procedure the user can execute anytime. Audit trail is preserved via the UAT file; status flips to `passed` or `failed` after the user runs it.
  </done>
</task>

</tasks>

<verification>
- `chromatindb` + `chromatindb_tests` targets build clean.
- `[sync][cap-filter]` tag runs green.
- VERI-05 is explicitly user-delegated — UAT file is the deliverable here, not an automated pass.
</verification>
