# Phase 68: Production Distribution Kit - Context

**Gathered:** 2026-03-28
**Status:** Ready for planning

<domain>
## Phase Boundary

dist/ directory with systemd units, default configs, sysusers.d, tmpfiles.d, and install script for bare-metal deployment. Operators can deploy chromatindb from build output to running production service using a single install script. Zero C++ code changes.

</domain>

<decisions>
## Implementation Decisions

### Default config contents
- **D-01:** Minimal configs — only fields that differ from compiled defaults
- **D-02:** Node config (3 fields): `data_dir` (/var/lib/chromatindb), `log_file` (/var/log/chromatindb/node.log), `uds_path` (/run/chromatindb/node.sock)
- **D-03:** Relay config (3 fields): `uds_path` (/run/chromatindb/node.sock), `identity_key_path` (/var/lib/chromatindb/relay.key), `log_file` (/var/log/chromatindb/relay.log)
- **D-04:** All optional fields (storage limits, rate limiting, quotas, max_peers, sync_interval, etc.) omitted — compiled defaults (0/disabled) are the production defaults

### Install script behavior
- **D-05:** Takes binary paths as arguments (already decided: dist/ decoupled from CMake)
- **D-06:** Generates identity keys automatically if they don't exist (runs chromatindb keygen + chromatindb_relay keygen)
- **D-07:** Does NOT enable or start services — operator does that manually
- **D-08:** Supports --uninstall (stop services, disable units, remove installed files, preserve data dir and configs)
- **D-09:** Quiet output — only errors printed. No chatty per-step progress.

### systemd units
- **D-10:** Hardened by default — no separate standard/hardened variants. One unit per binary.
- **D-11:** Security directives: ProtectSystem=strict, NoNewPrivileges=yes, MemoryDenyWriteExecute=yes (from success criteria)
- **D-12:** Type=simple, Restart=on-failure. No sd_notify, no libsystemd dependency.
- **D-13:** chromatindb-relay.service has After=chromatindb.service dependency ordering

### Claude's Discretion
- Environment files (/etc/default/) — skip (YAGNI, operators edit JSON configs directly)
- Exact FHS paths for binaries (/usr/local/bin vs /usr/bin)
- Additional systemd security directives beyond the 3 specified
- tmpfiles.d directory modes and ownership
- Install script argument parsing and validation
- dist/ internal directory layout

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Distribution requirements
- `.planning/REQUIREMENTS.md` — DIST-01 through DIST-07 define all deliverables
- `.planning/ROADMAP.md` §Phase 68 — Success criteria (5 testable conditions)

### Existing deployment patterns
- `Dockerfile` — Multi-stage build, creates chromatindb user, binaries in /usr/local/bin
- `deploy/configs/node1.json` — Example minimal node config (reference for field names)

### Config schemas (source of truth for field names and defaults)
- `db/config/config.h` — Node config struct with all fields and defaults
- `relay/config/relay_config.h` — Relay config struct with all fields and defaults

### Binary entry points (keygen commands, CLI args)
- `db/main.cpp` — Node CLI: run/keygen/version, --config/--data-dir/--log-level
- `relay/relay_main.cpp` — Relay CLI: run/keygen/version, --config/--output/--force

### Prior research
- `.planning/research/FEATURES.md` — Feature landscape analysis with systemd patterns, anti-features, and dependency ordering

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Dockerfile` — Already creates chromatindb user/group and copies binaries to /usr/local/bin; patterns reusable for sysusers.d and install script
- `deploy/configs/node*.json` — Example configs showing field names (but dev-focused, not production paths)

### Established Patterns
- Node keygen: `chromatindb keygen --data-dir <path>` creates node.key/node.pub
- Relay keygen: `chromatindb_relay keygen --output <path>` creates .key/.pub pair
- Config loading: both binaries accept `--config <path>` to load JSON config
- spdlog rotation: built-in via log_max_size_mb (10) and log_max_files (3) — no external logrotate needed

### Integration Points
- Node UDS: `uds_path` config field enables Unix domain socket listener
- Relay→Node: relay connects to node via UDS path specified in relay config
- Both binaries read JSON config from path passed via --config flag

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 68-production-distribution-kit*
*Context gathered: 2026-03-28*
