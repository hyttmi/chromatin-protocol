# Phase 128: Configurable Blob Cap + Frame Shrink + Config Gauges — Context

**Gathered:** 2026-04-22
**Status:** Ready for planning

<domain>
## Phase Boundary

Make `blob_max_bytes` operator-tunable via `config.json` with SIGHUP hot-reload and `/metrics` observability; shrink `MAX_FRAME_SIZE` to a 2 MiB protocol constant; emit a `chromatindb_config_*` gauge per numeric `Config` field.

**In scope:**
- `Config::blob_max_bytes` field: default 4 MiB, bounds `[1 MiB, 64 MiB]`, SIGHUP-reloadable
- Ingest-path enforcement of `blob_max_bytes` against current live value
- `MAX_BLOB_DATA_SIZE` rename to `MAX_BLOB_DATA_HARD_CEILING = 64 MiB` (upper-bound invariant only)
- `MAX_FRAME_SIZE` drops to 2 MiB in both `db/net/framing.h` AND `cli/src/connection.cpp` (single atomic commit)
- Paired lower + upper bound `static_assert`s on `MAX_FRAME_SIZE` relationship to `STREAMING_THRESHOLD`
- `chromatindb_config_*` gauges: one per numeric Config field, 1:1 mirror of struct names
- NodeInfoResponse encoder swapped to read live `Config::blob_max_bytes` (replaces Phase 127's direct `MAX_BLOB_DATA_SIZE` read)
- Unit tests: config load/bounds/SIGHUP reload + gauge emission

**Out of scope (belongs to other phases):**
- Sync-out cap filtering → Phase 129 (SYNC-01..04)
- CLI auto-tuning from advertised cap → Phase 130 (CLI-01..05)
- Documentation reconciliation (PROTOCOL.md etc.) → Phase 131 (DOCS-01..08)
- String-valued Config fields as gauges (REQUIREMENTS.md Out-of-Scope)
- Blob re-migration or deletion when cap lowered (BLOB-03 explicitly forbids)

</domain>

<decisions>
## Implementation Decisions

### Config value propagation
- **D-01:** Mirror the Phase 127 pattern: store `blob_max_bytes_` as a plain member on the owning objects (`Engine`, `MessageDispatcher`), not a shared atomic or new `RuntimeLimits` struct. `PeerManager::reload_config()` seeds these members on startup AND on SIGHUP, same call path already proven for `max_subscriptions_` / `rate_limit_bytes_per_sec_` (peer_manager.cpp:165-166, 552-565). No `std::atomic` — SIGHUP reload is serialized against ingest, and a u64 read is torn-free on both x86_64 and aarch64 when naturally aligned.
- **D-02:** No `const Config&` reference-passing into deep call paths. Callsites read from the owning object's own seeded member. Keeps the `Config` struct out of hot code paths and matches the existing dispatcher/engine encapsulation.

### `MAX_BLOB_DATA_SIZE` disposition
- **D-03:** Rename `MAX_BLOB_DATA_SIZE` (currently in `db/net/framing.h:18`) to `MAX_BLOB_DATA_HARD_CEILING = 64ULL * 1024 * 1024` (64 MiB). The renamed constant survives ONLY as (a) the upper bound in `validate_config`'s bounds check on `blob_max_bytes`, and (b) a `static_assert` cementing the operator-cap-vs-protocol-ceiling relationship at build time.
- **D-04:** All RUNTIME callsites (`Engine::ingest` at engine.cpp:110, `Connection::handle_chunked_data` at connection.cpp:854, `MessageDispatcher::NodeInfoRequest` handler at message_dispatcher.cpp:721) switch from reading `chromatindb::net::MAX_BLOB_DATA_SIZE` to reading the seeded `blob_max_bytes_` member (D-01).
- **D-05:** Naming rationale: `MAX_BLOB_DATA_SIZE` was ambiguous ("size" could mean current cap OR hard upper bound); `MAX_BLOB_DATA_HARD_CEILING` makes the "build-time invariant, not operational cap" distinction unambiguous. Grep discoverability improves because there is now a single symbol per concern.

### Gauge naming + unit suffix
- **D-06:** Mechanical 1:1 mirror. Gauge name is `chromatindb_config_<struct_field_name>` for every numeric field in `Config`, verbatim. No retroactive unit-suffix normalization (`max_peers` stays `chromatindb_config_max_peers`, NOT `_max_peers_count`). Keeps emission code to a single registration loop with no per-field ledger.
- **D-07:** String-valued Config fields (`bind_address`, `storage_path`, `data_dir`, `log_level`, `log_file`, `log_format`, `uds_path`, `metrics_bind`, `config_path`) do not emit gauges — explicit REQUIREMENTS.md Out-of-Scope. Vector-valued fields (`bootstrap_peers`, `allowed_client_keys`, `allowed_peer_keys`, `trusted_peers`, `sync_namespaces`) and the `namespace_quotas` map also excluded (not numeric, not "every numeric field").
- **D-08:** Gauges emit alphabetically sorted by field name for scrape stability (diff-friendly /metrics output across reloads).
- **D-09:** Prometheus style-guide unit suffixes (`_bytes`, `_seconds`) are intentionally not applied retroactively. If an operator wants a suffixed alias for a specific dashboard, that's a future phase — the struct is the source of truth here.

### FRAME_SIZE shrink order of operations
- **D-10:** Single atomic commit drops `MAX_FRAME_SIZE` from 110 MiB to 2 MiB in BOTH `db/net/framing.h:14` AND `cli/src/connection.cpp:36` simultaneously. No two-step, no CLI trailing. Justified by `feedback_no_backward_compat.md` — both binaries on the same dev track.
- **D-11:** Add an upper-bound `static_assert` colocated with the existing lower-bound one in `framing.h`:
  ```cpp
  static_assert(MAX_FRAME_SIZE <= 2 * STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN,
      "MAX_FRAME_SIZE must stay close to one streaming sub-frame + AEAD margin; "
      "raising it decouples framing from streaming and breaks Phase 126's audit.");
  ```
  This pins both directions of the `MAX_FRAME_SIZE ≈ 2 × STREAMING_THRESHOLD + AEAD margin` relationship from FRAME-02.
- **D-12:** `TRANSPORT_ENVELOPE_MARGIN` (currently referenced inline at connection.cpp:988) is promoted to `framing.h` as a named `constexpr` so both `static_assert`s + the existing assertion share a single definition.
- **D-13:** CLI's hand-mirrored constant pattern is preserved (current arrangement). A shared-header unification between db/net/ and cli/src/ is explicitly deferred — NOT a Phase 128 concern. If drift becomes a problem, a dedicated future phase extracts the shared header.

### SIGHUP semantics for existing blobs
- **D-14:** Cap enforcement is **ingest-only**. Lowering `blob_max_bytes` does NOT brick previously-stored oversized blobs.
- **D-15:** Enforcement sites in Phase 128 scope:
  - `Engine::ingest` (engine.cpp:110) — reject oversized NEW writes against live cap.
  - `Connection::handle_chunked_data` (connection.cpp:854) — reject chunked stream declaring `total_payload_size > blob_max_bytes_`. Existing site; just switches the constant → seeded member.
  - `MessageDispatcher::NodeInfoResponse` encoder (message_dispatcher.cpp:721) — advertises live cap. Read path only; not an "enforcement" site, but the value source changes per D-01/D-04.
- **D-16:** Non-enforcement sites (explicit — these MUST stay cap-free in Phase 128):
  - Blob read / ReadResponse — returns stored blob regardless of size.
  - Sync-out (announce, BlobNotify, BlobFetch response) — uncapped in Phase 128. Phase 129 (SYNC-01..04) owns peer-cap-aware filtering of outbound sync based on PeerInfo's snapshotted advertised cap. Phase 128 deliberately leaves this lane open.
  - GC / prune / compaction — lifecycle owned by TTL/tombstones, not size.
- **D-17:** BLOB-04 error message reads `Config::blob_max_bytes` via the seeded member from D-01 so the operator sees the *current* cap in the rejection, not a stale constant.

### Claude's Discretion
- Exact wording of /metrics HELP/TYPE comment lines for each new gauge (Prometheus convention dictates format; specific wording is cosmetic).
- Exact struct field→gauge emission mechanism (macro vs constexpr array vs code-gen) as long as D-06/D-08 hold.
- Order in which the three per-plan commits land within the phase (as long as FRAME shrink is atomic per D-10 and encoder swap lands together with Engine/Connection switch per D-04).
- Whether `blob_max_bytes_` is seeded via a new setter method or via the existing `set_rate_limits()` / `set_max_subscriptions()` style — symmetry with Phase 127's approach is acceptable either way.

</decisions>

<specifics>
## Specific Ideas

- **Phase 127 pattern is the blueprint.** D-01's member-seeding-via-PeerManager-reload_config approach is a direct reuse of what already ships for `max_subscriptions_` and `rate_limit_bytes_per_sec_`. Plan tasks should grep for those existing setters/members as the worked example and mirror their lifecycle verbatim. This keeps SIGHUP reload semantics identical across all three of (subscriptions, rate limit, blob cap).
- **Phase boundary with 129 is load-bearing.** Sync-out filtering must NOT appear in Phase 128. If implementation discovers a case where sync-out "obviously" needs the cap check, stop and escalate — it probably belongs in Phase 129 PeerInfo snapshot logic, not here.
- **FRAME-01 is a protocol break.** 110 MiB → 2 MiB. Test suite may have tests with large-payload literals that relied on the 110 MiB headroom; those need to drop to ≤ 2 MiB or be deleted as irrelevant. Expected breakage, not a regression.

</specifics>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Project-level
- `.planning/PROJECT.md` — vendor neutrality, no-backward-compat stance, YAGNI
- `.planning/REQUIREMENTS.md` — BLOB-01..04, FRAME-01..02, METRICS-01..02, VERI-01, VERI-04 (authoritative acceptance criteria)
- `.planning/ROADMAP.md` §"Phase 128" — goal + 5 success criteria
- `.planning/ROADMAP.md` §"Phase 129" — defines sync-out lane that Phase 128 must NOT touch
- `.planning/ROADMAP.md` §"Phase 130" — defines CLI auto-tune consumption of the advertised cap

### Phase-level carryover
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md` D-04, D-05 — Phase 127's "source is framing.h, not config" decision that Phase 128 explicitly reverses
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-01-SUMMARY.md` — exact encoder wiring Phase 128 swaps (line range, helper choice)
- `.planning/phases/126-pre-shrink-audit/126-VERIFICATION.md` — streaming invariant audit that FRAME-01 shrink must not break
- `.planning/phases/126-pre-shrink-audit/126-01-SUMMARY.md` — streaming invariant definition consumed by D-11's static_assert wording

### Codebase anchors
- `db/config/config.h` — `Config` struct (target of BLOB-01 field addition + METRICS-01 gauge loop)
- `db/config/config.cpp` — `validate_config` (target of BLOB-02 bounds check)
- `db/net/framing.h:14,18,22,31` — `MAX_FRAME_SIZE`, `MAX_BLOB_DATA_SIZE` (rename target per D-03), `STREAMING_THRESHOLD`, existing `static_assert`
- `db/net/connection.cpp:36-39,854-857,988` — CLI `MAX_FRAME_SIZE` mirror, chunked size check, `TRANSPORT_ENVELOPE_MARGIN` inline reference
- `cli/src/connection.cpp:36-39` — CLI's `MAX_FRAME_SIZE` hand-mirror (target of D-10 atomic drop)
- `db/engine/engine.cpp:110-116` — ingest oversize check (target of BLOB-04 error-message update)
- `db/peer/message_dispatcher.cpp:721,725` — NodeInfoResponse encoder reading `MAX_BLOB_DATA_SIZE` / `MAX_FRAME_SIZE` (targets of D-04 swap)
- `db/peer/peer_manager.h:111,138` + `db/peer/peer_manager.cpp:165-166,345,552-565` — `reload_config()` path (extension target for seeding `blob_max_bytes_`)
- `db/peer/metrics_collector.cpp:251-310` — `format_prometheus_metrics` (extension target for METRICS-01 gauge loop)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **Phase 127's setter/member pattern** — `MessageDispatcher::set_rate_limits()` + `set_max_subscriptions()` called from `PeerManager::reload_config()`. Directly reusable shape for `set_blob_max_bytes()`.
- **`validate_config` accumulator** — already accumulates errors across multiple fields and throws with all of them; extending for BLOB-02 bounds check is one new block, no framework change needed.
- **`format_prometheus_metrics` builder** — currently hand-writes each gauge as a string-concat; METRICS-01 extends the same pattern with a new block per numeric Config field (D-06's mechanical mirror maps cleanly onto this).

### Established Patterns
- **SIGHUP reload is synchronous on PeerManager's strand** — no cross-thread reload races to design around (validates D-01's no-atomic call).
- **Config-sourced values stored as members on consuming objects** — applies to `MessageDispatcher` (Phase 127) and `Engine` (existing `max_storage_bytes` handling). Phase 128 adds `blob_max_bytes_` to the same set.
- **Big-endian fixed-width helpers in `db/util/endian.h`** — relevant only for the NodeInfoResponse encoder site; Phase 127 already locked `chromatindb::util::store_u64_be` as the helper, no change.

### Constraints Discovered
- **`STREAMING_THRESHOLD` is 1 MiB** (framing.h:22) — drives the lower bound on `MAX_FRAME_SIZE` in D-11's static_assert.
- **`TRANSPORT_ENVELOPE_MARGIN` is inline at connection.cpp:988** — needs promotion to framing.h per D-12 to give the new upper-bound static_assert a named constant to reference.
- **CLI's `MAX_FRAME_SIZE` is duplicated** (`cli/src/connection.cpp:36`) — D-10/D-13 acknowledge the drift risk and defer structural fix.

</code_context>

<deferred>
## Deferred Ideas

- **Shared `framing.h` between db/ and cli/** — CLI currently hand-mirrors `MAX_FRAME_SIZE` and `STREAMING_THRESHOLD`. A clean fix extracts a shared header (e.g., `common/protocol_limits.h`). Out of scope for Phase 128 per D-13; revisit if drift causes a production bug.
- **Prometheus-style unit-suffix normalization** — renaming `max_peers` → `max_peers_count`, `worker_threads` → `worker_threads_count`, etc. Cosmetic; could land as a future "metrics polish" phase if dashboards demand it.
- **Per-field gauge HELP text** — D-06 implements a mechanical mirror with a generic HELP line. Hand-tuned HELP per field (explaining semantics) is useful but separable; file as a future doc/metrics polish pass if operators request.
- **`Config` field reflection / code-gen** — a macro or reflection-based registry would auto-generate gauges from the struct declaration. Overkill for 25 fields; hand-listing is fine.
- **Dynamic `blob_max_bytes` upper ceiling** — some operators may want > 64 MiB. Requires a new phase to lift `MAX_BLOB_DATA_HARD_CEILING` AND re-audit every enforcement site. Explicit future work, not a Phase 128 or Phase 129 concern.

</deferred>

<open_questions>
## Open Questions

None. All 5 gray areas decided interactively 2026-04-22.

</open_questions>
</content>
</invoke>