---
plan: 127-03-cli-decoder
phase: 127
type: execute
wave: 2
depends_on: [127-01-encoder]
files_modified:
  - cli/src/commands.cpp
autonomous: true
requirements: [NODEINFO-01, NODEINFO-02, NODEINFO-03, NODEINFO-04]
must_haves:
  truths:
    - "`cdb info` reads 4 new fixed-width fields between storage_max (u64) and types_count (u8) in order: blob(u64), frame(u32), rate(u64), subs(u32)"
    - "`cdb info` prints four new output lines: Max blob, Max frame, Rate limit, Max subs — in that order, after the Quota: line"
    - "Rate limit value 0 renders as `unlimited`; non-zero renders via humanize_bytes with `/s` suffix"
    - "Max subs value 0 renders as `unlimited`; non-zero renders as a plain integer"
    - "Max blob and Max frame always render via humanize_bytes (non-zero invariant holds in practice)"
    - "The stale git_hash layout comment block is rewritten to document the full post-Phase-127 wire layout (no git_hash mention left)"
    - "No phase numbers, REQ IDs, or D-XX tokens appear in any `std::printf` / `std::fprintf` output"
  artifacts:
    - path: "cli/src/commands.cpp"
      provides: "Updated `cdb info` decoder + renderer supporting the 4 new NodeInfoResponse fields"
      contains: "Max blob:"
  key_links:
    - from: "cli/src/commands.cpp info()"
      to: "NodeInfoResponse payload bytes (post-Phase-127 wire layout)"
      via: "read_u64 + read_u32 calls in the existing decoder, inserted between storage_max and types_count"
      pattern: "read_u64\\(\\).*(max_blob|max_frame|rate_limit|max_subs)"
---

<objective>
Extend `cdb info` at `cli/src/commands.cpp:2188-2287` to decode and render the 4 new NodeInfoResponse fields shipped by Plan 127-01. In passing, rewrite the stale `git_hash` layout comment (CONTEXT.md D-12) to document the full post-Phase-127 layout.

Purpose: Operators and users can see the node's advertised caps (blob size, frame size, rate limit, subscription cap) on a single `cdb info` call. This is Phase 127's user-facing deliverable; Phase 130 later wires the same decoded values into a CLI session cache for auto-tune, but that is out of scope here (D-08).

Output: One modified source file (`cli/src/commands.cpp`) — rewrites a comment block, adds 4 `read_*()` calls, and adds 4 `std::printf` lines.
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
Current `cdb info` render block (cli/src/commands.cpp:2274-2284) — reproduced for reference, DO NOT rewrite the existing lines, only ADD new ones AFTER:

```cpp
    std::printf("Version:    %s\n", version.c_str());
    std::printf("Uptime:     %s\n", humanize_uptime(uptime).c_str());
    std::printf("Peers:      %u\n", peer_count);
    std::printf("Namespaces: %u\n", namespace_count);
    std::printf("Blobs:      %llu\n", (unsigned long long)total_blobs);
    std::printf("Used:       %s\n", humanize_bytes(storage_used).c_str());
    if (storage_max == 0) {
        std::printf("Quota:      unlimited\n");
    } else {
        std::printf("Quota:      %s\n", humanize_bytes(storage_max).c_str());
    }
```

Current stale comment block (cli/src/commands.cpp:2233-2237) — this is the D-12 passing fix:

```cpp
    // Parse NodeInfoResponse:
    // [version_len:1][version_str][git_hash_len:1][git_hash_str]
    // [uptime:8BE][peer_count:4BE][namespace_count:4BE][total_blobs:8BE]
    // [storage_used:8BE][storage_max:8BE]
    // [types_count:1][supported_types: types_count bytes]
```

Note: `git_hash_len` / `git_hash_str` were NEVER actually encoded on the node side (verified: `grep -c 'git_hash' db/peer/message_dispatcher.cpp` == 0). The comment has been wrong since the file was written. D-12 rewrites it to match reality.

Relevant existing helpers already in scope inside `info()`:
- `read_u8()` (cli/src/commands.cpp:2241-2244) — reads 1 byte, throws on truncation.
- `read_u32()` (cli/src/commands.cpp:2251-2256) — reads 4 BE bytes via `load_u32_be`, throws on truncation.
- `read_u64()` (cli/src/commands.cpp:2257-2262) — reads 8 BE bytes via `load_u64_be`, throws on truncation.
- `humanize_bytes(uint64_t)` at cli/src/commands.cpp:116 — static file-local helper, returns std::string like `"500 MiB"`.
- `humanize_uptime(uint64_t)` at cli/src/commands.cpp:134 — static file-local helper (used for Uptime line).

`humanize_bytes` signature (static, file-local):
```cpp
static std::string humanize_bytes(uint64_t n);
```

CONTEXT.md decisions governing this plan:
- **D-06**: `cdb info` displays all 4 new caps after the existing `Quota:` line, in order blob → frame → rate → subs. Use `humanize_bytes` for byte-quantified values. Subscriptions is a plain integer.
- **D-07**: Zero-value handling — `rate_limit_bytes_per_sec == 0` prints `unlimited`; `max_subscriptions_per_connection == 0` prints `unlimited`. `max_blob_data_bytes` and `max_frame_bytes` are always non-zero in practice → always humanized.
- **D-08**: CLI auto-tune / session cache is NOT in Phase 127. This plan decodes the fields and PRINTS them — no caching into `ConnectOpts` or `Session`, no wiring to chunk-size logic. Phase 130 owns that.
- **D-12**: Rewrite the stale git_hash comment to document the full post-Phase-127 layout. No functional consequence — just comment accuracy.
- **D-11**: No separate Catch2 CLI unit test. Decoder is exercised transitively by Plan 127-04's integration test (which walks the wire bytes) plus developer sanity.

Feedback hooks (mandatory):
- `feedback_no_phase_leaks_in_user_strings.md`: `cdb info` printf strings MUST NOT contain "Phase 127", "NODEINFO-01", "D-06", "v4.2.0", `rate_limit_bytes_per_sec`, `MAX_FRAME_SIZE`, or any other identifier-tier token. User-visible strings are `Max blob:`, `Max frame:`, `Rate limit:`, `Max subs:`.
- `feedback_no_duplicate_code.md`: reuse the existing `read_u32` / `read_u64` lambdas and `humanize_bytes` helper — do NOT inline a second byte-reader or hand-roll a humanizer.
- `feedback_delegate_tests_to_user.md`: this plan runs only a targeted build; full-suite test runs are delegated to the user at the wave boundary.

Consumer dependency: this plan ASSUMES Plan 127-01 has landed. If the encoder has not been extended, the `cdb info` call will read garbage (the bytes between what was `storage_max` and `types_count` will actually BE `types_count`+`supported[]`, and the new `read_u64` at the `max_blob_data_bytes` offset will consume 8 bytes of supported_types — symptom: decode error or truncated output). The integration between 127-01 and 127-03 is implicitly validated by Plan 127-04's Catch2 test.
</interfaces>
</context>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: Rewrite stale layout comment + add 4 new read_*() calls + 4 new printf lines in cdb info</name>
  <files>cli/src/commands.cpp</files>
  <read_first>
    - cli/src/commands.cpp (read lines 2188-2290 verbatim — the full `info()` function — before editing; in particular confirm the exact current line numbers for the comment block at 2233-2237, the reads at 2264-2272, and the printf block at 2274-2284)
    - cli/src/commands.cpp (read lines 110-160 to confirm `humanize_bytes` and `humanize_uptime` signatures — both `static std::string (uint64_t)` — and verify no third existing helper does something similar)
    - cli/src/wire.h (confirm MsgType::NodeInfoResponse=40 still matches the node enum; this task does NOT edit wire.h)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md (D-06 output format, D-07 zero-value handling, D-08 session-cache carve-out, D-12 stale-comment fix)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-01-encoder-PLAN.md (wire layout shipped upstream — confirm the field order blob → frame → rate → subs that this decoder mirrors)
  </read_first>
  <action>
    Make exactly three edits to `cli/src/commands.cpp`. All three are inside the `info(...)` function between lines 2188 and 2287.

    **Edit A — Rewrite the stale layout comment (D-12).**

    Replace the 5-line block at current lines 2233-2237:

    ```cpp
        // Parse NodeInfoResponse:
        // [version_len:1][version_str][git_hash_len:1][git_hash_str]
        // [uptime:8BE][peer_count:4BE][namespace_count:4BE][total_blobs:8BE]
        // [storage_used:8BE][storage_max:8BE]
        // [types_count:1][supported_types: types_count bytes]
    ```

    with the following (document the full post-127 layout; no `git_hash` mention):

    ```cpp
        // Parse NodeInfoResponse (wire layout):
        //   [version_len:1][version:version_len bytes]
        //   [uptime:8 BE]
        //   [peer_count:4 BE][namespace_count:4 BE]
        //   [total_blobs:8 BE]
        //   [storage_used:8 BE][storage_max:8 BE]
        //   [max_blob_data_bytes:8 BE]
        //   [max_frame_bytes:4 BE]
        //   [rate_limit_bytes_per_sec:8 BE]
        //   [max_subscriptions_per_connection:4 BE]
        //   [types_count:1][supported_types:types_count bytes]
    ```

    **Edit B — Add 4 new `read_*()` calls to decode the 4 new fields.**

    Immediately AFTER the existing `auto storage_max = read_u64();` line (currently line 2272) and BEFORE the first existing `std::printf("Version:    %s\n", ...);` line (currently line 2274), insert:

    ```cpp
        auto max_blob_data_bytes = read_u64();
        auto max_frame_bytes = read_u32();
        auto rate_limit_bytes_per_sec = read_u64();
        auto max_subscriptions = read_u32();
    ```

    Order is critical: blob(u64), frame(u32), rate(u64), subs(u32) — mirrors the encoder from Plan 127-01 per D-02. Do NOT reorder.

    **Edit C — Add 4 new `std::printf` output lines.**

    Immediately AFTER the closing `}` of the existing `if (storage_max == 0) { ... } else { ... }` block (currently at line 2284) and BEFORE the `return 0;` at line 2286, insert exactly:

    ```cpp
        std::printf("Max blob:   %s\n", humanize_bytes(max_blob_data_bytes).c_str());
        std::printf("Max frame:  %s\n", humanize_bytes(max_frame_bytes).c_str());
        if (rate_limit_bytes_per_sec == 0) {
            std::printf("Rate limit: unlimited\n");
        } else {
            std::printf("Rate limit: %s/s\n", humanize_bytes(rate_limit_bytes_per_sec).c_str());
        }
        if (max_subscriptions == 0) {
            std::printf("Max subs:   unlimited\n");
        } else {
            std::printf("Max subs:   %u\n", max_subscriptions);
        }
    ```

    Column alignment: all labels are followed by a colon + whitespace-padding so the value column starts at the same visual position as `Version:    `, `Uptime:     `, etc. (12 columns: label+colon+padding). Verify by eye and by `grep -E '^(\s*)std::printf\("[A-Za-z ]+:\s+' cli/src/commands.cpp | head`.

    Strings to USE: `Max blob:`, `Max frame:`, `Rate limit:`, `Max subs:`. Strings NOT to use: anything containing `127`, `D-`, `NODEINFO`, `v4.2.0`, `bytes_per_sec`, `framing.h`, `Phase`, or any other planner-identifier token (feedback_no_phase_leaks_in_user_strings.md).

    **Do NOT:**
    - Add any new `#include` to this file (`humanize_bytes`, `read_u32`, `read_u64`, and `std::printf` are all already in scope).
    - Add caching of any decoded value into a `ConnectOpts`, `Session`, or any struct — Phase 130 owns that (D-08).
    - Touch any other file (`cli/src/wire.h`, `cli/src/main.cpp`, `cli/src/chunked.h`, etc.).
    - Touch `db/PROTOCOL.md` (D-13).
    - Run the full test suite per-commit (feedback_delegate_tests_to_user.md / feedback_no_test_automation_in_executor_prompts.md) — only the targeted build.
  </action>
  <verify>
    <automated>
      cmake --build build-debug -j$(nproc) --target cdb
    </automated>
  </verify>
  <acceptance_criteria>
    All of the following grep / file / build checks must pass from the repo root:

    1. Stale `git_hash` reference fully removed from the CLI (D-12):
       `grep -c 'git_hash' cli/src/commands.cpp` == 0

    2. New comment block documents the 4 new fields:
       `grep -c 'max_blob_data_bytes:8 BE' cli/src/commands.cpp` >= 1
       `grep -c 'max_frame_bytes:4 BE' cli/src/commands.cpp` >= 1
       `grep -c 'rate_limit_bytes_per_sec:8 BE' cli/src/commands.cpp` >= 1
       `grep -c 'max_subscriptions_per_connection:4 BE' cli/src/commands.cpp` >= 1

    3. Four new reads present, using the existing helpers:
       `grep -c 'auto max_blob_data_bytes = read_u64();' cli/src/commands.cpp` == 1
       `grep -c 'auto max_frame_bytes = read_u32();' cli/src/commands.cpp` == 1
       `grep -c 'auto rate_limit_bytes_per_sec = read_u64();' cli/src/commands.cpp` == 1
       `grep -c 'auto max_subscriptions = read_u32();' cli/src/commands.cpp` == 1

    4. Four new printf labels present with the exact user-visible wording:
       `grep -c '"Max blob:   %s\\\\n"' cli/src/commands.cpp` == 1
       `grep -c '"Max frame:  %s\\\\n"' cli/src/commands.cpp` == 1
       `grep -c '"Rate limit: unlimited\\\\n"' cli/src/commands.cpp` == 1
       `grep -c '"Rate limit: %s/s\\\\n"' cli/src/commands.cpp` == 1
       `grep -c '"Max subs:   unlimited\\\\n"' cli/src/commands.cpp` == 1
       `grep -c '"Max subs:   %u\\\\n"' cli/src/commands.cpp` == 1

       (Shell escaping: if the above `\\\\n` literal-quoting is awkward in your shell, equivalent is:
        `grep -F 'Max blob:   %s' cli/src/commands.cpp | wc -l` == 1, etc. The point is the exact label string appears once each.)

    5. No phase-leak tokens in printf output strings (feedback_no_phase_leaks_in_user_strings.md):
       `grep -En 'std::printf\(.*"(Phase|127|NODEINFO|D-[0-9]|v4\.2\.0|rate_limit_bytes_per_sec|MAX_FRAME|framing\.h)' cli/src/commands.cpp` returns zero lines.

    6. Zero-value branches exist (D-07):
       `grep -c 'rate_limit_bytes_per_sec == 0' cli/src/commands.cpp` == 1
       `grep -c 'max_subscriptions == 0' cli/src/commands.cpp` == 1
       `grep -c 'Rate limit: unlimited' cli/src/commands.cpp` == 1
       `grep -c 'Max subs:   unlimited' cli/src/commands.cpp` == 1

    7. Field-read order preserved (blob → frame → rate → subs). Quick check (all integers, strictly increasing):
       first line of `max_blob_data_bytes` < first line of `max_frame_bytes` < first line of `rate_limit_bytes_per_sec` < first line of `max_subscriptions` (inside `cli/src/commands.cpp`):
       `awk '/auto max_blob_data_bytes/{print NR; exit}' cli/src/commands.cpp` < `awk '/auto max_frame_bytes/{print NR; exit}' cli/src/commands.cpp` < `awk '/auto rate_limit_bytes_per_sec/{print NR; exit}' cli/src/commands.cpp` < `awk '/auto max_subscriptions/{print NR; exit}' cli/src/commands.cpp`

    8. No new `#include` added to commands.cpp as part of this plan (feedback_no_duplicate_code.md — humanize_bytes / read_u64 / read_u32 are already in scope):
       `git diff cli/src/commands.cpp | grep -c '^+#include'` == 0

    9. No other file is modified by this plan:
       `git diff --stat | grep -v 'cli/src/commands.cpp' | grep -E '\|' | wc -l` == 0

    10. Build acceptance: cdb target builds clean with `-j$(nproc)` (NEVER `--parallel`).
        `cmake --build build-debug -j$(nproc) --target cdb` exits 0.
  </acceptance_criteria>
  <done>
    `cdb info`, when run against a node shipping the Plan 127-01 encoder, prints 4 new lines (Max blob, Max frame, Rate limit, Max subs) after `Quota:`. The stale `git_hash` layout comment is gone. The CLI binary builds.
  </done>
</task>

</tasks>

<threat_model>
## Trust Boundaries

| Boundary | Description |
|----------|-------------|
| node→CLI wire (post-handshake) | Untrusted bytes from a remote node cross into the CLI via `conn.recv()`. The existing `read_u8` / `read_u32` / `read_u64` lambdas all bounds-check and throw `std::runtime_error("NodeInfoResponse truncated")` on short buffers — the new reads inherit this protection. |

## STRIDE Threat Register

| Threat ID | Category | Component | Disposition | Mitigation Plan |
|-----------|----------|-----------|-------------|-----------------|
| T-127-05 | T (Tampering) | CLI decoder bounds-check | mitigate | 4 new reads use the existing `read_u32` / `read_u64` lambdas (cli/src/commands.cpp:2251-2262) which throw on truncation before reading past the payload tail. No raw pointer arithmetic. |
| T-127-06 | I (Information Disclosure via format string) | `std::printf` in cdb info | mitigate | All 4 new printf strings are static literals with explicit `%s` / `%u` format specifiers; decoded values are ints and `std::string::c_str()` — no user-controlled format string, no `printf(decoded_string)`. |
| T-127-07 | D (Denial of Service via malformed response) | CLI decoder | mitigate | `std::runtime_error` thrown by the lambdas is caught by the enclosing exception machinery; `cdb` exits non-zero with a visible error — no infinite loop, no memory exhaustion. Existing behavior for the pre-Phase-127 reads; new reads inherit it. |
</threat_model>

<verification>
- cdb target compiles clean.
- Runtime verification of decoder correctness against the encoder is owned by Plan 127-04 (Catch2 integration test walking the wire bytes end-to-end).
- No new Catch2 test is added at the CLI layer per D-11.
- Full test suite is user-delegated at the wave boundary.
</verification>

<success_criteria>
1. `cdb info` prints 4 new lines after `Quota:` — `Max blob:`, `Max frame:`, `Rate limit:`, `Max subs:` — in that order, per D-06.
2. Zero-value branches (D-07) produce `unlimited` for rate and subs.
3. No `git_hash` reference remains in `cli/src/commands.cpp`.
4. No phase / REQ / D-XX tokens appear in user-visible strings.
5. cdb target builds.
6. Only `cli/src/commands.cpp` is modified.
</success_criteria>

<output>
After completion, create `.planning/phases/127-nodeinforesponse-capability-extensions/127-03-SUMMARY.md`.
</output>
</content>
</invoke>