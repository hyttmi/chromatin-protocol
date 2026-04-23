---
plan: 130-01-session-cap-derive-manifest
phase: 130
type: execute
wave: 1
depends_on: []
files_modified:
  - cli/src/connection.h
  - cli/src/connection.cpp
  - cli/src/wire.h
  - cli/src/wire.cpp
  - cli/src/chunked.h
  - cli/src/chunked.cpp
  - cli/src/commands.cpp
  - cli/tests/test_chunked.cpp
  - cli/tests/test_wire.cpp
  - .planning/phases/130-cli-auto-tuning/130-UAT.md
autonomous: true
requirements: [CLI-01, CLI-02, CLI-03, CLI-04, CLI-05, VERI-06]
must_haves:
  truths:
    - "Connection object carries a uint64_t session_blob_cap_ field, set once per successful connect from NodeInfoResponse's max_blob_data_bytes"
    - "Hardcoded CHUNK_SIZE_BYTES_DEFAULT (16 MiB), CHUNK_SIZE_BYTES_MAX (256 MiB), CHUNK_THRESHOLD_BYTES (400 MiB) are removed from cli/src/wire.h + cli/src/chunked.h; no new hardcoded blob-cap literals introduced"
    - "Chunking consumers read session cap via the Connection reference — chunk size == threshold == cap for v4.2.0+"
    - "Manifest validator rejects CPAR manifests whose chunk_size_bytes > session_blob_cap_ with a clear diagnostic (both values named)"
    - "MAX_CHUNKS retained at 65536 — see CONTEXT.md D-05"
    - "Pre-v4.2.0 node (short NodeInfoResponse payload) triggers hard-fail with error naming the version gap per D-07"
    - "VERI-06 UAT markdown captures the 64 MiB put+get + SHA3-256 verify against the user's local + home nodes; status: pending"
  artifacts:
    - path: "cli/src/connection.h"
      provides: "session_blob_cap_ member + accessor"
    - path: "cli/src/commands.cpp"
      provides: "on-connect NodeInfoRequest seeds session cap"
    - path: "cli/src/wire.h"
      provides: "no CHUNK_SIZE_BYTES_{DEFAULT,MAX} constants"
    - path: "cli/src/chunked.h"
      provides: "no CHUNK_THRESHOLD_BYTES constant"
    - path: "cli/tests/test_chunked.cpp + test_wire.cpp"
      provides: "session-cap-threaded chunking + manifest validator tests"
    - path: ".planning/phases/130-cli-auto-tuning/130-UAT.md"
      provides: "VERI-06 user-delegated procedure"
---

<objective>
Make `cdb` auto-tune its chunking from the server's advertised cap. Delete the three hardcoded constants. Keep the manifest validator honest against the session cap. Ship VERI-06 as UAT.
</objective>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: Session cap cache + on-connect seeding</name>
  <files>cli/src/connection.h, cli/src/connection.cpp, cli/src/commands.cpp</files>
  <read_first>
    - cli/src/connection.h (current Connection shape — find where session-scoped state lives)
    - cli/src/connection.cpp (connect flow)
    - cli/src/commands.cpp:2195-2306 (existing NodeInfoResponse decode pattern — copy the offset math)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-01-SUMMARY.md (authoritative offset)
  </read_first>
  <action>
    1. **Add session cap field to Connection** in `cli/src/connection.h`:
       ```cpp
       // Server's advertised max_blob_data_bytes, snapshotted on connect (CLI-01 / D-01).
       // 0 = un-seeded (pre-connect or pre-v4.2.0 node — see D-07 hard-fail).
       uint64_t session_blob_cap_ = 0;
       ```
       Add a public accessor:
       ```cpp
       uint64_t session_blob_cap() const { return session_blob_cap_; }
       ```

    2. **Seed the cap on connect.** After TCP/UDS handshake completes and BEFORE the first command's protocol traffic, send a `NodeInfoRequest` and decode `max_blob_data_bytes`. The cleanest place is inside `Connection::connect()` (or its equivalent) just before returning success. Use the same decode logic as `cli/src/commands.cpp:2279` (read_u64_be at the correct offset — skip version/uptime/peer_count/namespace_count/total_blobs/storage_used/storage_max first).

    3. **Short-payload detection (D-07).** If the NodeInfoResponse payload length is less than `(version_header + 8+4+4+8+8+8+8)` bytes (i.e., the response doesn't reach `max_blob_data_bytes`), refuse the connection with an error to stderr: `"server NodeInfoResponse is missing max_blob_data_bytes field — node is older than v4.2.0; upgrade the node or use an older cdb"`. Do NOT silently default. Return connect-failure up the stack.

    4. **Skip seeding only for `cdb info` calls that are explicitly the thing fetching it.** Wait — actually, since `cdb info` is how users manually see this info, and the cache-seed uses the same request, there's no conflict. `cdb info` will just get the same data twice (once cached, once for display). Don't bother optimizing.

    5. **No modification to NodeInfoRequest wire layout** — just reusing Phase 127's existing response shape.
  </action>
  <verify>
    <automated>
      grep -c 'session_blob_cap_' cli/src/connection.h
      # ^ >= 2 (field + accessor)
      grep -c 'NodeInfoRequest' cli/src/connection.cpp
      # ^ >= 1 (new seeding call)
      grep -c 'older than v4.2.0\|missing max_blob_data_bytes' cli/src/connection.cpp
      # ^ >= 1 (hard-fail error per D-07)
      cmake --build cli/build-debug -j$(nproc) --target cdb 2>&1 | tail -3
    </automated>
  </verify>
  <acceptance_criteria>
    - Connection carries `session_blob_cap_` field + accessor.
    - Connect flow sends NodeInfoRequest after handshake + before returning success.
    - Decoded `max_blob_data_bytes` stored in `session_blob_cap_`.
    - Short-response hard-fail error present with specific wording naming the version gap.
    - `cdb` target builds.
  </acceptance_criteria>
  <done>
    Every successful `cdb` connection starts with the cap cached in-memory. Commands downstream can read it via `conn.session_blob_cap()`.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 2: Delete hardcoded constants + rewire consumers to session cap</name>
  <files>cli/src/wire.h, cli/src/wire.cpp, cli/src/chunked.h, cli/src/chunked.cpp, cli/src/commands.cpp</files>
  <read_first>
    - cli/src/wire.h (lines 301-305)
    - cli/src/chunked.h (line 50 + helper comments)
    - cli/src/chunked.cpp:146,156,497 (all consumers of the deleted constants)
    - cli/src/commands.cpp:703,715 (additional consumers)
    - cli/src/wire.cpp:534 (manifest validator)
  </read_first>
  <action>
    1. **Delete** `CHUNK_SIZE_BYTES_DEFAULT`, `CHUNK_SIZE_BYTES_MAX`, `CHUNK_THRESHOLD_BYTES` lines from `cli/src/wire.h` and `cli/src/chunked.h`. Update adjacent doc-comments that reference the deleted names to cite the session cap instead.

    2. **Rewire the 5 consumer callsites** (line numbers pre-edit, will shift after deletion):
       - `cli/src/chunked.cpp:146`: `if (end_pos < CHUNK_THRESHOLD_BYTES || end_pos > MAX_CHUNKED_FILE_SIZE)` → `if (end_pos <= conn.session_blob_cap() || end_pos > MAX_CHUNKED_FILE_SIZE)`. The semantic is: files ≤ cap fit in one blob (no chunking); files > cap get chunked. Plus the MAX_CHUNKED_FILE_SIZE upper ceiling (based on MAX_CHUNKS × cap) stays.
       - `cli/src/chunked.cpp:151`: update the error message to reference `conn.session_blob_cap()`, naming the live value.
       - `cli/src/chunked.cpp:156`: `const uint32_t chunk_size = CHUNK_SIZE_BYTES_DEFAULT;` → `const uint32_t chunk_size = static_cast<uint32_t>(conn.session_blob_cap());` (with a check that the cap fits in u32 — at 64 MiB hard ceiling, it always does; static_assert or runtime check your choice).
       - `cli/src/chunked.cpp:497`: `std::vector<uint8_t> buf(CHUNK_SIZE_BYTES_DEFAULT);` → `std::vector<uint8_t> buf(conn.session_blob_cap());`.
       - `cli/src/commands.cpp:703`: `if (fsize >= chunked::CHUNK_THRESHOLD_BYTES)` → `if (fsize > conn.session_blob_cap())` — files strictly larger than cap trigger chunking.
       - `cli/src/commands.cpp:715`: update error message similarly.

    3. **Manifest validator (CLI-04)** at `cli/src/wire.cpp:534`:
       Before: `m.chunk_size_bytes > CHUNK_SIZE_BYTES_MAX` returns `std::nullopt`.
       After: add a `uint64_t session_cap` parameter to the validator (or plumb the Connection ref). Replace with: `m.chunk_size_bytes > session_cap`. Update the validator's signature + all callers. On rejection, log a diagnostic that names BOTH the declared value AND the allowed value (e.g., `"CPAR chunk_size_bytes=X exceeds server cap=Y"`).

    4. **MAX_CHUNKS policy (D-05):** Retain `MAX_CHUNKS = 65536`. Add a comment near its declaration documenting the decision rationale per CONTEXT.md D-05 — just a 2-line explainer.

    5. **MAX_CHUNKED_FILE_SIZE**: now derived as `MAX_CHUNKS × session_blob_cap()` where it's consumed. May need to be a function/accessor rather than a constant. Or just inline the calculation at each site.

    6. **Build gate:** `cmake --build cli/build-debug -j$(nproc) --target cdb` exits 0. Also build cli_tests to flush any test-code references: `cmake --build cli/build-debug -j$(nproc) --target cli_tests`.
  </action>
  <verify>
    <automated>
      # No surviving hardcoded constants
      grep -c 'CHUNK_SIZE_BYTES_DEFAULT\|CHUNK_SIZE_BYTES_MAX\|CHUNK_THRESHOLD_BYTES' cli/src/wire.h cli/src/chunked.h
      # ^ should be 0
      # No surviving 16/256/400 MiB literals related to chunking
      grep -cE '16 ?\* ?1024 ?\* ?1024|256 ?\* ?1024 ?\* ?1024|400ULL? ?\* ?1024 ?\* ?1024' cli/src/wire.h cli/src/chunked.h
      # ^ should be 0
      # session_blob_cap() used in consumers
      grep -c 'session_blob_cap' cli/src/chunked.cpp cli/src/wire.cpp cli/src/commands.cpp
      # ^ >= 5 (multiple consumer rewirings)
      cmake --build cli/build-debug -j$(nproc) --target cdb 2>&1 | tail -3
      cmake --build cli/build-debug -j$(nproc) --target cli_tests 2>&1 | tail -3
    </automated>
  </verify>
  <acceptance_criteria>
    - Zero surviving `CHUNK_SIZE_BYTES_DEFAULT` / `CHUNK_SIZE_BYTES_MAX` / `CHUNK_THRESHOLD_BYTES` references in cli/src/.
    - Manifest validator takes session cap as parameter / reference + diagnoses failures with both values named.
    - `cdb` AND `cli_tests` targets both build clean.
    - `MAX_CHUNKS = 65536` retained with D-05 rationale comment.
  </acceptance_criteria>
  <done>
    CLI chunking is now entirely session-cap-derived. The 3 hardcoded constants are gone.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 3: Unit tests + VERI-06 UAT</name>
  <files>cli/tests/test_chunked.cpp, cli/tests/test_wire.cpp, .planning/phases/130-cli-auto-tuning/130-UAT.md</files>
  <read_first>
    - cli/tests/test_chunked.cpp (existing patterns; mock-connection approach if any)
    - cli/tests/test_wire.cpp (manifest validator test scaffolding)
  </read_first>
  <action>
    **Unit tests — extend existing files, 4 scenarios per CONTEXT.md D-09:**

    In `cli/tests/test_chunked.cpp` under tag `[chunked][session-cap]`:
    - `session cap seeded from NodeInfoResponse mock` — construct a mock Connection, set `session_blob_cap_` to 4 MiB, assert chunk threshold == cap.
    - `file below cap is NOT chunked` — file-size 2 MiB with cap 4 MiB → passes through single-blob path.
    - `file above cap IS chunked at cap boundary` — file-size 10 MiB with cap 4 MiB → ceil(10/4) = 3 chunks.

    In `cli/tests/test_wire.cpp` under tag `[wire][manifest][session-cap]`:
    - `manifest at cap boundary accepts` — `chunk_size_bytes == cap` validates (boundary: `>` not `>=`).
    - `manifest above cap rejects with both values named` — `chunk_size_bytes > cap` rejects; diagnostic includes both values.

    If extending existing files is awkward (different fixture assumptions), it's acceptable to create new files `cli/tests/test_session_cap_chunking.cpp` and `cli/tests/test_session_cap_manifest.cpp` — whatever the executor finds less brittle. CMake list updates in `cli/tests/CMakeLists.txt` if new files are created.

    **Build + targeted run:**
    - `cmake --build cli/build-debug -j$(nproc) --target cli_tests` passes.
    - `./cli/build-debug/cli_tests "[session-cap]"` exits 0.

    **VERI-06 UAT markdown at `.planning/phases/130-cli-auto-tuning/130-UAT.md`:**

    ```markdown
    ---
    status: pending
    phase: 130-cli-auto-tuning
    requirement: VERI-06
    delegated_to: user
    reason: "Full CLI↔node integration against a running daemon; user already operates cdb --node local + --node home for exactly this scenario (CONTEXT.md D-08)."
    updated: 2026-04-23
    ---

    # Phase 130 UAT: CLI auto-tune + 64 MiB round-trip (VERI-06)

    **Goal:** Prove that `cdb` auto-discovers the server's advertised cap, chunks a 64 MiB file at the cap, uploads, downloads, and the downloaded bytes' SHA3-256 matches the source.

    ## Setup

    - Local node: `blob_max_bytes = 4194304` (4 MiB default).
    - Ensure node is running and reachable via `cdb --node local`.
    - Create a 64 MiB random file: `dd if=/dev/urandom of=/tmp/verify-06.bin bs=1M count=64`
    - Record the source SHA3-256: `sha3sum -a 256 /tmp/verify-06.bin > /tmp/verify-06.sha`

    ## Test 1: Put auto-chunked file

        cdb --node local put my-ns verify-06 /tmp/verify-06.bin

    **Expected:** upload succeeds; `cdb` decides to chunk based on auto-discovered 4 MiB cap (64 MiB / 4 MiB = 16 chunks + 1 CPAR manifest).

    ## Test 2: Get file and verify round-trip

        cdb --node local get my-ns verify-06 > /tmp/verify-06-back.bin
        sha3sum -a 256 /tmp/verify-06-back.bin

    **Expected:** hash matches the source hash saved earlier.

    ## Test 3: Rejection on short NodeInfoResponse

    Intentionally skip unless you have a pre-v4.2.0 binary handy. If so, point `cdb` at it and confirm it refuses to connect with an error naming the version gap (per CONTEXT.md D-07).

    ## Test 4: Cross-node with --node home (divergent caps)

    Optional, bonus: if home node has `blob_max_bytes = 8 MiB`, run the same flow with `--node home` and confirm the upload chunks at 8 MiB boundaries instead of 4 MiB.

    ## Results

    status: [ ] passed / [ ] failed
    notes:
    ```
  </action>
  <verify>
    <automated>
      grep -c '\[session-cap\]' cli/tests/test_chunked.cpp cli/tests/test_wire.cpp cli/tests/test_session_cap_chunking.cpp cli/tests/test_session_cap_manifest.cpp 2>/dev/null
      # ^ at least some non-zero count; exact match depends on executor's extend-vs-new-file choice
      test -f .planning/phases/130-cli-auto-tuning/130-UAT.md
      grep -c 'VERI-06' .planning/phases/130-cli-auto-tuning/130-UAT.md
      cmake --build cli/build-debug -j$(nproc) --target cli_tests 2>&1 | tail -3
      ./cli/build-debug/cli_tests "[session-cap]" 2>&1 | tail -3
    </automated>
  </verify>
  <acceptance_criteria>
    - At least 5 assertions under tag `[session-cap]` covering the 5 D-09 scenarios.
    - VERI-06 UAT file exists with status: pending.
    - cli_tests builds; `[session-cap]` tag passes.
  </acceptance_criteria>
  <done>
    CLI-01..05 have automated coverage. VERI-06 has a procedure the user runs when ready.
  </done>
</task>

</tasks>

<threat_model>
- **T-130-01 Capability downgrade**: Could a man-in-the-middle (no such thing post-AEAD, but hypothetically) force `cdb` to receive a smaller cap and thus chunk more aggressively? Answer: no — NodeInfoResponse is AEAD-encrypted post-handshake. If AEAD is broken, far bigger problems than chunking.
- **T-130-02 Short-response spoofing**: A malicious/bugged node omits the new fields to trigger D-07 hard-fail. Effect: cdb refuses connection. Fails closed — operator must upgrade node. Consistent with feedback_no_backward_compat.
</threat_model>

<verification>
- `cdb` + `cli_tests` targets build clean.
- `[session-cap]` tag passes.
- VERI-06 is user-delegated — UAT file is the deliverable here.
</verification>
