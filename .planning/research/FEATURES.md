# Feature Landscape: v1.5.0 Documentation & Distribution

**Domain:** Production distribution packaging and documentation refresh for C++ daemon
**Researched:** 2026-03-28

## Scope

This milestone covers two distinct feature areas for the existing chromatindb project:

1. **Documentation refresh** -- README.md and PROTOCOL.md need updating to reflect v1.3.0 and v1.4.0 additions (18 new message types, request pipelining, concurrent dispatch, expanded relay filter).
2. **Production distribution kit** -- systemd service units, default configs, logrotate, tmpfiles.d, sysusers.d, and an install script for deploying chromatindb and chromatindb_relay on bare-metal Linux servers.

Both binaries (chromatindb node on port 4200, chromatindb_relay on port 4201) need distribution support. Docker already works (Dockerfile exists with multi-stage build); this milestone targets non-containerized Linux deployment.

## Table Stakes

Features operators expect from a production-ready daemon distribution. Missing = deployment feels amateur.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| systemd service unit for chromatindb | Every production Linux daemon ships a .service file. Without it, operators write their own (incorrectly) or run in tmux/screen. systemd provides restart, logging, dependency ordering, resource limits. | Low | Type=simple (chromatindb does not fork). ExecStart with --config flag. Restart=on-failure. Needs User=chromatindb, data directory, and config path. |
| systemd service unit for chromatindb_relay | Relay is a separate binary needing its own unit. Operators run both on the same host, relay depends on node being up (After=chromatindb.service). | Low | Similar structure to node unit. Needs identity key path, UDS path to node, config file. |
| sysusers.d configuration | Creates the `chromatindb` system user/group at install time. Dockerfile already does `groupadd/useradd`; sysusers.d is the systemd-native equivalent for bare metal. | Low | Single line: `u chromatindb - "chromatindb daemon"`. Standard pattern, no decisions. |
| tmpfiles.d configuration | Creates /var/lib/chromatindb (data), /run/chromatindb (UDS socket, PID), /var/log/chromatindb (logs) with correct ownership at boot. Without this, operators manually mkdir. | Low | 3-4 lines. Standard format: `d /path mode user group -`. |
| Default config files | Ship node.json and relay.json with sensible production defaults. Without reference configs, operators guess at field names from README. | Low | JSON files with all fields documented via comments (or adjacent .example files since JSON has no comments). Use production-appropriate defaults (bind 0.0.0.0, log to file, JSON log format). |
| Logrotate configuration | Production servers need log rotation. chromatindb already has spdlog rotating_file_sink built in (log_max_size_mb, log_max_files), but operators expect a /etc/logrotate.d/ entry for consistency with other system services. | Low | **Key decision**: spdlog handles rotation internally, so logrotate should use `copytruncate` to avoid conflicting with spdlog's own rotation. However, since spdlog already rotates, logrotate is arguably redundant. Ship it anyway because ops teams expect it, but configure it as a safety net (larger thresholds than spdlog's own limits). See analysis below. |
| Install script | Single script to copy binaries, configs, systemd units, and activate services. Operators should not need to know where every file goes. | Low-Med | Shell script. Copies files to standard locations, runs systemd-sysusers, systemd-tmpfiles --create, systemctl daemon-reload, enables services. Needs --prefix and --uninstall options. |
| README.md updated to current state | README.md currently says "v1.3.0" but the project is at v1.4.0. The wire protocol section says "40 message types" but there are now 58. Feature list is missing v1.4.0 query types. Config reference is missing fields added in v1.4.0. | Low-Med | Editing existing content. db/README.md is the main doc (370 lines). Root README.md is a thin pointer. Update version strings, message type count, feature descriptions, config reference, deployment scenarios. |
| PROTOCOL.md updated to current state | PROTOCOL.md already has v1.4.0 types documented (types 41-58 are there from Phase 67 work). Main gaps: the message type reference table and any inconsistencies with the v1.3.0 additions (request_id, concurrent dispatch model). | Low | Review and verify completeness. PROTOCOL.md appears mostly current from Phase 64/67 documentation work. May need minor corrections only. |

## Differentiators

Features that elevate the distribution beyond minimum viable. Not expected from every daemon, but signal production maturity.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Hardened systemd unit (security sandbox) | systemd sandboxing (ProtectSystem=strict, PrivateTmp, NoNewPrivileges, PrivateDevices, ProtectKernelTunables, etc.) scores well on `systemd-analyze security`. Shows security consciousness appropriate for a crypto daemon. Kubo/IPFS ships both a standard and hardened variant. | Low | Adding ~15 security directives to the .service file. chromatindb needs: ReadWritePaths=/var/lib/chromatindb /var/log/chromatindb /run/chromatindb, network access (AF_INET, AF_INET6, AF_UNIX). No /home, /boot, kernel modules, or clock access needed. |
| Separate hardened unit file | Ship both chromatindb.service (simple, works everywhere) and chromatindb-hardened.service (maximum sandboxing). Operators choose based on their environment. IPFS/Kubo pattern. | Low | Copy of main unit with security directives added. Keeps the default simple for debugging. |
| Environment file support | ExecStart reads config path from /etc/default/chromatindb or EnvironmentFile. Allows operators to override config path, data dir, and log level without editing the unit file. Standard distro pattern (Debian uses /etc/default/, RHEL uses /etc/sysconfig/). | Low | EnvironmentFile=-/etc/default/chromatindb in unit. Ship a template. |
| PROTOCOL.md as standalone specification | Current PROTOCOL.md is excellent (829 lines, byte-level format docs). Making it a complete, self-contained specification that someone could implement a client from scratch (without reading C++ source) is the differentiator. Needs a table of contents, version stamping, and any coverage gaps filled. | Med | Review for completeness. Currently covers handshake, blob format, sync protocol, all 58 message types. May need: error handling summary, message flow diagrams, connection state machine. |
| Relay deployment scenario in README | README has scenarios for single node, two-node sync, closed mode, rate limiting, trusted peers, and logging. Missing: relay deployment (the node+relay+client architecture). Since relay is a shipped binary, deployment docs are expected. | Low | New section in db/README.md showing relay config, UDS setup, and client connection flow. |
| dist/ directory structure | Organize all distribution files in a dist/ directory tree mirroring the target filesystem layout. Makes it easy to understand what goes where and enables `cp -r dist/ /` style deployment (or proper packaging later). | Low | Directory structure only, no code. dist/usr/lib/systemd/system/, dist/usr/lib/sysusers.d/, dist/usr/lib/tmpfiles.d/, dist/etc/chromatindb/, dist/etc/logrotate.d/. |
| Uninstall support in install script | Install scripts that cannot be reversed are frustrating. An --uninstall flag that stops services, disables units, removes files is professional. | Low | Reverse of install: systemctl stop/disable, rm files. |

## Anti-Features

Features to explicitly NOT build in this milestone.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| .deb/.rpm packaging | Real package management requires maintainer scripts, dependency resolution, architecture matrix, CI/CD for each distro. Massive scope for a project with two users. Premature optimization. | Ship install script + dist/ tree. Packaging can be added later when there is demand. |
| sd_notify / Type=notify integration | Would require adding libsystemd as a build dependency and calling sd_notify("READY=1") after init completes. chromatindb does not currently link against libsystemd. Type=simple with Restart=on-failure is sufficient and keeps the binary portable (runs on non-systemd systems too). | Type=simple. Readiness can be probed via TCP port check (the existing Docker HEALTHCHECK pattern: `exec 3<>/dev/tcp/127.0.0.1/4200`). |
| Watchdog integration (WatchdogSec) | Requires periodic sd_notify("WATCHDOG=1") calls from the daemon. Same libsystemd dependency concern as Type=notify. The inactivity timeout + auto-reconnect already handle most failure modes. | Restart=on-failure handles crashes. Operators can use external monitoring (health check scripts) for hung processes. |
| Configuration management (Ansible/Puppet/Chef) | Writing CM modules is out of scope. The install script is the building block that CM tools invoke. | Document install script usage. CM users wrap it themselves. |
| Man pages | Useful but high effort for low visibility in a pre-1.0-user-base project. README and --help are sufficient. | --help output on both binaries. README.md is the documentation. |
| Init.d scripts (SysV init) | systemd is universal on modern Linux. SysV init is legacy. Supporting both doubles maintenance for near-zero audience. | systemd only. Document that systemd is required. |
| Automatic TLS/Let's Encrypt for relay | Relay uses PQ crypto (ML-KEM-1024), not TLS. Adding TLS would be a protocol change, not a distribution concern. | PQ handshake is the transport security. |
| Prometheus metrics endpoint | Would require an HTTP server or exposition format. Out of scope for a distribution milestone. SIGUSR1 + JSON logs serve monitoring needs today. | JSON structured logs can be ingested by log aggregators (Loki, ELK). SIGUSR1 for ad-hoc checks. |

## Logrotate vs. spdlog Analysis

**Key tension**: chromatindb already has built-in log rotation via spdlog's rotating_file_sink (configured via log_max_size_mb and log_max_files). Shipping a logrotate config creates two rotation mechanisms.

**Resolution**: Ship the logrotate config as a safety net with deliberately higher thresholds than spdlog's defaults:
- spdlog default: 10 MB per file, 3 files retained
- logrotate config: rotate weekly, maxsize 100M, rotate 4, compress, copytruncate

This means spdlog handles normal rotation (fine-grained, application-aware), while logrotate catches edge cases:
- If spdlog's rotation fails (disk full during rename)
- If operators set very high log_max_size_mb values
- For the relay binary (which may or may not have identical log rotation)
- For consistency with ops teams' expectations (`logrotate --force /etc/logrotate.d/chromatindb` just works)

**Must use `copytruncate`** because spdlog holds the file handle open. The `create` mode would rename the file away, and spdlog would keep writing to the renamed file via its existing file descriptor. spdlog does not support external rotation signals (no SIGHUP log reopen). The `copytruncate` approach has a tiny window of potential log loss, but this is acceptable for a safety-net config.

**Confidence:** HIGH -- spdlog GitHub issue #3464 confirms spdlog lacks external logrotate support, and `copytruncate` is the documented workaround for daemons that hold file handles.

## Documentation Gap Analysis

### README.md (db/README.md) -- Current State

The db/README.md is 370 lines and covers:
- Crypto stack table (current)
- Architecture description (current through v1.2.0)
- Build instructions (current)
- Testing (says "551 unit tests" -- needs update to 560+)
- Usage (keygen, run, version -- current)
- Configuration reference (22 config fields -- current through v1.1.0, missing v1.3.0/v1.4.0 additions if any)
- Signals (SIGTERM, SIGHUP, SIGUSR1 -- current)
- Wire protocol summary (says "40 message types" -- needs update to "58 message types")
- Scenarios (6 scenarios -- current but missing relay scenario)
- Features (22 features listed -- missing v1.4.0 query features)

**Specific updates needed:**
1. Version string: v1.3.0 -> v1.5.0
2. Test count: "551 unit tests" -> current count (560+)
3. Wire protocol section: "40 message types" -> "58 message types"
4. Features section: add v1.4.0 query features (NamespaceList, StorageStatus, NamespaceStats, Metadata, BatchExists, DelegationList, BatchRead, PeerInfo, TimeRange)
5. Config reference: verify no new config fields were added in v1.3.0/v1.4.0
6. Deployment scenario: add relay deployment example
7. Production deployment section: reference dist/ install script

### PROTOCOL.md -- Current State

The PROTOCOL.md is 829 lines and appears comprehensive through v1.4.0:
- Transport layer (current)
- Connection lifecycle with PQ and trusted handshakes (current)
- Blob schema and signing (current)
- Sync protocol Phases A/B/C (current)
- All 58 message types in reference table (current -- added in Phase 67)
- v1.4.0 query extensions with byte-level formats (current)

**Specific updates needed:**
1. Version stamp (does not currently have one)
2. Table of contents (not present -- useful for 829 lines)
3. Verify consistency between main body text and message type reference table
4. Possibly add a connection state machine summary or error handling section

### Root README.md -- Current State

The root README.md is 14 lines and says "Current release: v1.3.0". This is a thin pointer to db/README.md.

**Update needed:** Version string to v1.5.0.

## Feature Dependencies

```
sysusers.d --> tmpfiles.d --> install script  (user must exist before dirs, dirs before install)
systemd service units --> default configs     (units reference config paths)
default configs --> tmpfiles.d                (configs reference data/log/run paths)
logrotate config --> tmpfiles.d               (logrotate references /var/log/chromatindb)
install script --> all of the above           (script installs everything)
README.md update --> dist/ directory          (README references installation method)
PROTOCOL.md update --> none                   (standalone)
```

## MVP Recommendation

### Phase 1: dist/ Package (infrastructure)
Everything needed to deploy on bare metal:

1. **sysusers.d config** -- creates chromatindb user/group
2. **tmpfiles.d config** -- creates /var/lib/chromatindb, /run/chromatindb, /var/log/chromatindb
3. **Default configs** -- node.json and relay.json with production defaults
4. **systemd service units** -- chromatindb.service and chromatindb-relay.service
5. **Hardened systemd units** -- chromatindb-hardened.service and chromatindb-relay-hardened.service
6. **Environment files** -- /etc/default/chromatindb and /etc/default/chromatindb-relay
7. **Logrotate config** -- /etc/logrotate.d/chromatindb
8. **Install script** -- dist/install.sh with --uninstall support

### Phase 2: Documentation Refresh
Update existing docs to current state:

1. **PROTOCOL.md** -- add table of contents, version stamp, verify completeness
2. **db/README.md** -- update version, test counts, message type count, feature list, add relay scenario, add deployment section
3. **Root README.md** -- update version string

### Ordering Rationale

dist/ first because:
- It is self-contained (no doc dependency)
- It creates the directory structure that README.md will reference
- Install script path can be cited in documentation
- Documentation naturally comes last as it describes the final state

## Complexity Summary

| Feature | Effort | Risk |
|---------|--------|------|
| sysusers.d | Trivial (1 line) | None |
| tmpfiles.d | Trivial (4 lines) | None |
| Default configs | Low (JSON files, known schema) | None |
| systemd units | Low (standard patterns) | Testing on actual systemd needed |
| Hardened units | Low (additive security directives) | May need tuning per-distro |
| Environment files | Trivial | None |
| Logrotate config | Low | copytruncate interaction with spdlog |
| Install script | Low-Med (file copies, systemd commands) | Path assumptions, permission handling |
| PROTOCOL.md update | Low (mostly verification) | Already mostly current |
| README.md update | Low-Med (editing existing content) | Keeping accurate counts |

**Total estimated effort:** Small milestone. All features are low complexity with well-understood patterns. No new C++ code required. No protocol changes. No build system changes.

## Sources

- [systemd.service man page](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)
- [tmpfiles.d man page](https://www.freedesktop.org/software/systemd/man/latest/tmpfiles.d.html)
- [sysusers.d man page](https://man7.org/linux/man-pages/man5/sysusers.d.5.html)
- [Kubo/IPFS systemd service files](https://github.com/ipfs/kubo/tree/master/misc)
- [Kubo hardened service](https://github.com/ipfs/kubo/blob/master/misc/systemd/ipfs-hardened.service)
- [spdlog external logrotate support discussion](https://github.com/gabime/spdlog/issues/3464)
- [systemd security hardening gist](https://gist.github.com/ageis/f5595e59b1cddb1513d1b425a323db04)
- [systemd sandboxing - ArchWiki](https://wiki.archlinux.org/title/Systemd/Sandboxing)
- [Logrotate copytruncate documentation](https://man7.org/linux/man-pages/man8/logrotate.8.html)
- [sd_notify man page](https://man7.org/linux/man-pages/man3/sd_notify.3.html)
- [Fedora systemd security hardening](https://fedoraproject.org/wiki/Changes/SystemdSecurityHardening)
- [openSUSE systemd packaging guidelines](https://en.opensuse.org/openSUSE:Systemd_packaging_guidelines)
