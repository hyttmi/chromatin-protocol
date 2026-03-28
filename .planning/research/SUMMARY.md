# Project Research Summary

**Project:** chromatindb v1.5.0
**Domain:** Production distribution packaging and documentation refresh for C++20 database daemon
**Researched:** 2026-03-28
**Confidence:** HIGH

## Executive Summary

v1.5.0 is a packaging and documentation milestone with zero C++ code changes, zero build system modifications, and zero new runtime dependencies. The project ships two production binaries (chromatindb node on TCP 4200, chromatindb_relay on TCP 4201) with a Docker deployment path but no bare-metal deployment kit. This milestone fills that gap: a `dist/` directory of 8 files (systemd units, sysusers.d, tmpfiles.d, logrotate, default configs, install script) and documentation updates to README.md, db/README.md, and db/PROTOCOL.md to accurately reflect the v1.4.0 state including all 58 message types.

All required packaging primitives (systemd 252+, sysusers.d, tmpfiles.d, logrotate, bash) are available on the target platform (Debian Bookworm). No tooling choices need to be made — these are standard Linux daemon packaging patterns with well-documented behavior. The one non-obvious technical decision: logrotate must use `copytruncate` because spdlog holds file descriptors open and does not support external rotation signals. However, the pitfalls research is unambiguous: shipping a logrotate config for spdlog-managed logs creates dual rotation with a log-loss window. The recommended resolution is to skip logrotate entirely and document that spdlog's built-in `rotating_file_sink` (controlled by `log_max_size_mb` and `log_max_files`) handles rotation. This eliminates Pitfall 1.

The primary execution risk is path consistency: 8 dist/ files reference shared paths (data dir, UDS socket, log dir, config paths, binary paths) across different file formats (INI, JSON, shell). A mismatch between node config's `uds_path` and relay config's `uds_path` causes silent connection failure. Mitigation is to build all dist/ files in a single phase in dependency order, using a single canonical path reference throughout.

## Key Findings

### Recommended Stack

The dist/ packaging layer is fully decoupled from CMake — no changes to CMakeLists.txt, no CPack, no install() targets. The install script takes binary paths as arguments and copies to FHS-standard locations, working with both local builds and Docker-extracted binaries. This decoupling is intentional: the CMakeLists.txt is already 190 lines with 8 FetchContent dependencies, and mixing packaging concerns would add complexity with zero benefit.

**Core technologies:**
- systemd unit files (systemd 252+, Debian Bookworm baseline): Service management for both binaries — Type=simple (no fork, no sd_notify), ExecReload=/bin/kill -HUP leverages existing SIGHUP handler
- sysusers.d: Declarative system user/group creation — one line, idempotent, integrates with package managers
- tmpfiles.d: Only needed for /run/chromatindb since systemd StateDirectory/RuntimeDirectory directives handle the rest at service start
- bash install.sh: Copies artifacts to FHS paths, runs systemd-sysusers/tmpfiles, daemon-reload, prints next steps — no automation, operator controls service activation
- Markdown (hand-written): PROTOCOL.md and README.md — no doc generators; custom binary protocol does not benefit from auto-generation

### Expected Features

**Must have (table stakes):**
- systemd service units for both binaries — every production Linux daemon ships a .service file; without one operators write incorrect files or run in tmux
- sysusers.d + tmpfiles.d configs — creates system user and runtime directories declaratively
- Default production config files — absolute paths throughout, JSON log format for structured ingestion, production-appropriate defaults
- bash install script — single entry point that handles all installation steps
- README.md updated to v1.4.0/v1.5.0 — version strings, test count (560+), message type count (58), relay deployment scenario, dist/ deployment section
- PROTOCOL.md verification pass — confirms all 58 message types are documented (not just 40 from v1.3.0)

**Should have (differentiators):**
- systemd security hardening directives — ProtectSystem=strict, MemoryDenyWriteExecute=yes, NoNewPrivileges=yes, etc.; target systemd-analyze security score below 5.0; appropriate for a crypto daemon processing untrusted data
- Idempotent install script — running twice is safe; config files never overwritten (protects operator customizations); binaries and service files always updated
- Table of contents in PROTOCOL.md — 828 lines with 58 message types is unnavigable without one

**Defer (v2+):**
- .deb/.rpm packages — YAGNI until there are users requesting distro packages
- sd_notify / Type=notify integration — requires linking libsystemd, adds a build dependency for no benefit since the daemon starts in <100ms
- Ansible/Puppet/Chef playbooks — CM is operator's choice; ship raw files they can wrap
- Man pages — README.md and --help are sufficient at this scale
- Relay-only deployment (remote node) — out of scope for v1.5.0; current kit assumes relay+node colocated

### Architecture Approach

The dist/ directory mirrors the target filesystem layout across 8 files. Build order is dictated by dependency: sysusers.d first (user/group definition everything else references), then tmpfiles.d, then default configs (cross-checked against config.h and relay_config.h field names), then systemd units (which reference config paths, binary paths, user/group), then install.sh (which orchestrates all of the above). Documentation comes after dist/ because README.md references dist/ installation paths and commands.

**Major components:**
1. `dist/sysusers.d/chromatindb.conf` — creates chromatindb system user (UID auto-allocated, home=/var/lib/chromatindb, shell=/usr/sbin/nologin)
2. `dist/systemd/chromatindb.service` + `chromatindb-relay.service` — service management with hardening; relay unit has After=chromatindb.service + Wants=chromatindb.service; TimeoutStopSec=120 for graceful shutdown with in-flight sync
3. `dist/config/chromatindb.json` + `chromatindb-relay.json` — absolute paths throughout; UDS socket paths must match between node and relay configs
4. `dist/install.sh` — idempotent, never overwrites configs, calls daemon-reload after service file installation
5. `db/README.md` — comprehensive refresh: version, test count, message type count, relay section, dist/ deployment section
6. `db/PROTOCOL.md` — verification pass confirming all 58 types documented; add table of contents; verify binary format byte offsets against encoder source

### Critical Pitfalls

1. **logrotate + spdlog dual rotation** — spdlog holds the log file descriptor open and manages rotation internally via rotating_file_sink. Shipping a logrotate config creates two rotation mechanisms that conflict, causing log loss. Avoid: do not ship a logrotate config for spdlog-managed paths; document spdlog's built-in rotation as the intended mechanism.

2. **Wrong systemd service Type** — chromatindb runs in the foreground (ioc.run() blocks until SIGTERM). Type=forking causes systemd to think the process died; Type=notify causes startup timeout if sd_notify is never called. Use Type=simple or Type=exec. Verify with `systemctl start && systemctl status` showing "active (running)".

3. **Path inconsistency across dist/ files** — 8 files reference the same paths in different formats with no compiler validation. A uds_path mismatch between node config and relay config causes silent UDS connection failure. Avoid: build all dist/ files in a single session in dependency order; define canonical paths once at the start.

4. **Config overwrite on upgrade** — install script that unconditionally copies configs destroys operator customizations (bootstrap_peers, allowed_keys, quotas). Avoid: check before copying (`[ ! -f /etc/chromatindb/chromatindb.json ]`); binaries and service files always overwrite, config files never overwrite.

5. **Stale numbers in documentation** — PROTOCOL.md says "40 message types" (v1.3.0 era), README says v1.3.0, test count is wrong. Partial updates are worse than fully stale docs because readers trust recently-touched files. Avoid: build a verification checklist before writing (count from FlatBuffers schema enum, `ctest -N` for tests, version from git tags) and verify all numerical claims against current source.

6. **Wire format documentation errors** — PROTOCOL.md documents byte-level binary formats for hand-crafted response types (NodeInfoResponse, ExistsResponse, BatchReadResponse, etc.). Transcription errors cause SDK implementers to write code that the node silently drops. Avoid: for every binary format, verify byte offset arithmetic adds up and cross-reference against the encoder source code.

## Implications for Roadmap

Based on research, suggested phase structure:

### Phase 68: Production Distribution Kit

**Rationale:** dist/ is self-contained with no documentation dependencies. Documentation naturally references dist/ installation paths, so dist/ must be built first. All 8 dist/ files are highly interdependent through shared paths — splitting them across multiple phases increases path inconsistency risk.

**Delivers:** Complete bare-metal deployment kit: sysusers.d, tmpfiles.d, systemd units for node and relay, default production configs, install.sh with --uninstall. Operators can go from build output to running production service with `sudo dist/install.sh`.

**Addresses:** All distribution features from FEATURES.md table stakes list: systemd units, sysusers.d, tmpfiles.d, default configs, install script.

**Avoids:** Path inconsistency pitfall (build in single session in dependency order); config overwrite pitfall (conditional copy in install.sh); wrong service Type pitfall (Type=simple verified against main.cpp); missing security hardening pitfall (hardening directives from the start, validate with systemd-analyze).

**Build order within phase:**
1. sysusers.d (no dependencies)
2. tmpfiles.d (user must exist)
3. default configs (cross-check against config.h and relay_config.h)
4. systemd units (reference config paths + user)
5. install.sh (orchestrates all above; finalized last so all paths are known)

### Phase 69: Documentation Refresh

**Rationale:** Documentation describes the final state of the project, including dist/ installation. Writing it after Phase 68 means all paths and commands are finalized and can be accurately referenced.

**Delivers:** README.md, db/README.md, and db/PROTOCOL.md all accurate for v1.5.0: correct version strings, test counts, message type count (58), complete relay section, dist/ deployment instructions, verified wire format documentation.

**Addresses:** All documentation features from FEATURES.md: README.md refresh, PROTOCOL.md verification, relay deployment scenario, production deployment section.

**Avoids:** Stale numbers pitfall (checklist-driven verification before writing); wire format errors pitfall (cross-reference every binary format against encoder source); documentation duplication pitfall (README links to PROTOCOL.md for wire details, not duplicating them).

**Build order within phase:**
1. Build verification checklist (message type count from FlatBuffers, test count from ctest -N, current version)
2. PROTOCOL.md verification pass (confirm all 58 types documented; add TOC; verify binary format byte offsets)
3. db/README.md refresh (largest writing effort; references verified PROTOCOL.md and finalized dist/ paths)
4. README.md (root) update (links to db/README.md; version string only change)

### Phase Ordering Rationale

- dist/ before documentation: documentation references dist/ paths and the install script's command-line interface
- Both phases are pure file creation with no C++ compilation required — fast execution cycle
- Two phases, not one: dist/ and documentation are logically distinct deliverables with different audiences (operators deploying vs. developers integrating)
- Two phases, not three: dist/ files are too interconnected to split; documentation is a coherent sequential pass

### Research Flags

Phases with standard patterns (skip research-phase):
- **Phase 68:** systemd unit files, sysusers.d, tmpfiles.d, logrotate, bash install scripts — all have well-documented official specifications. The hardening directives are from systemd.exec(5) manpage. No novel integration problems.
- **Phase 69:** Documentation refresh is content verification work. The wire protocol and code are stable. No new protocols or APIs to research.

Neither phase needs a `/gsd:research-phase` invocation. All research questions for v1.5.0 are resolved by this research sprint.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All technologies verified against freedesktop.org official docs. systemd 252 on Debian Bookworm is confirmed baseline from existing Dockerfile. logrotate copytruncate behavior confirmed by spdlog issue #3464 maintainer response. |
| Features | HIGH | Scope is clearly defined by the v1.5.0 milestone specification. No ambiguity. The feature list is mechanically derivable from "what does a bare-metal deployment need." |
| Architecture | HIGH | dist/ decoupled from CMake approach verified by examining existing CMakeLists.txt (no install targets), Dockerfile (existing container path), deploy/ (existing docker-compose). No integration risk with existing build system. |
| Pitfalls | HIGH | Pitfalls derived from source code analysis (config.h, relay_config.h, main.cpp, relay_main.cpp, logging.cpp) not just community posts. Path consistency risk is directly observable. spdlog + logrotate conflict is confirmed by primary source (spdlog issue #3464). |

**Overall confidence:** HIGH

### Gaps to Address

- **systemd-analyze security score:** Hardened unit file security score validation requires a live systemd installation. This is Phase 68 implementation-time work: run `systemd-analyze verify` and `systemd-analyze security` during implementation, adjust directives if MemoryDenyWriteExecute or other directives cause runtime issues.
- **Config field completeness:** dist/ default configs must be cross-checked against config.h and relay_config.h field names during Phase 68 implementation. Confirmed at writing time, not research time.
- **PROTOCOL.md completeness assertion:** The claim that PROTOCOL.md covers all 58 types is based on Phase 67 documentation work and line count. Phase 69 must do a line-by-line verification against the transport.fbs MessageType enum — this is the phase's primary deliverable.
- **logrotate decision:** PITFALLS.md contradicts FEATURES.md on whether to ship logrotate. PITFALLS.md is authoritative: do not ship logrotate for spdlog-managed log paths. Document spdlog's built-in rotation instead. If the operator disables spdlog file rotation (empty log_file), they get console output captured by systemd journal which has its own rotation.

## Sources

### Primary (HIGH confidence)
- [freedesktop.org sysusers.d(5)](https://www.freedesktop.org/software/systemd/man/latest/sysusers.d.html) — file format spec, type codes, field definitions
- [freedesktop.org tmpfiles.d(5)](https://www.freedesktop.org/software/systemd/man/latest/tmpfiles.d.html) — directory creation rules, ownership semantics
- [freedesktop.org systemd.service(5)](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html) — unit file reference, Type= semantics
- [freedesktop.org systemd.exec(5)](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html) — execution environment, security hardening directives
- [Red Hat systemd unit files guide](https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/8/html-single/using_systemd_unit_files_to_customize_and_optimize_your_system/index) — unit authoring best practices
- [Red Hat logrotate guide](https://www.redhat.com/en/blog/setting-logrotate) — logrotate configuration best practices
- [Arch Wiki logrotate](https://wiki.archlinux.org/title/Logrotate) — copytruncate behavior documentation
- [spdlog issue #3464](https://github.com/gabime/spdlog/issues/3464) — spdlog + external logrotate interaction; maintainer confirms no external rotation support
- Direct source analysis: CMakeLists.txt, Dockerfile, db/config/config.h, relay/config/relay_config.h, db/main.cpp, relay/relay_main.cpp, db/logging/logging.cpp

### Secondary (MEDIUM confidence)
- [systemd hardening options gist (ageis)](https://gist.github.com/ageis/f5595e59b1cddb1513d1b425a323db04) — comprehensive hardening directive reference, cross-verified with Red Hat docs
- [openSUSE systemd packaging guidelines](https://en.opensuse.org/openSUSE:Systemd_packaging_guidelines) — vendor (/usr/lib) vs operator (/etc) file path conventions
- [Kubo/IPFS systemd service files](https://github.com/ipfs/kubo/tree/master/misc) — reference implementation of hardened + standard service unit pattern

---
*Research completed: 2026-03-28*
*Ready for roadmap: yes*
