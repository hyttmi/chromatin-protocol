# Pitfalls Research

**Domain:** Production distribution packaging and documentation refresh for existing C++ daemon
**Researched:** 2026-03-28
**Confidence:** HIGH

## Critical Pitfalls

### Pitfall 1: spdlog Rotating Sink Conflicts with External logrotate

**What goes wrong:**
chromatindb uses spdlog's `rotating_file_sink_mt`, which performs its own rotation internally (rename old file, open new one). If an external logrotate config is also configured for the same log path, the two rotation mechanisms fight each other. logrotate may rename or truncate a file that spdlog is actively writing to with its own rotation logic, causing lost log entries, duplicate rotation, or spdlog writing to a stale file descriptor. Using `copytruncate` does not fully fix this -- there is a documented brief window between the copy and truncate operations where log entries can be lost, and spdlog's internal rotation counter becomes confused about the file size (it tracks bytes written, not actual file size after truncation).

**Why it happens:**
Developers see "log file" in config and assume external logrotate is needed, not realizing spdlog already handles rotation. The `log_max_size_mb` and `log_max_files` config options already provide full rotation control. spdlog issue #3464 confirms that the library opens files in append mode and does not support external rotation signals -- it holds the fd open for the lifetime of the sink.

**How to avoid:**
Do NOT ship a logrotate config for spdlog-managed log files. Document this explicitly in both the default config comments and the README: "chromatindb manages its own log rotation via spdlog. Do not configure external logrotate for the `log_file` path." If operators need logrotate for specific compliance reasons, they must set `log_file` to empty (console-only mode) and redirect stderr via systemd's `StandardOutput=journal`, then manage journal rotation via journald.conf.

**Warning signs:**
- Log files appearing with unexpected suffixes (both spdlog's `.1`, `.2` and logrotate's `.1.gz`)
- Log gaps during rotation windows
- More rotated files on disk than `log_max_files` would produce

**Phase to address:**
Distribution packaging phase. Do not ship a logrotate config for the node or relay log paths. Document spdlog's built-in rotation as the intended mechanism.

---

### Pitfall 2: Wrong systemd Service Type Causing False Start/Stop Detection

**What goes wrong:**
chromatindb is a foreground daemon (`main.cpp` calls `ioc.run()` which blocks until shutdown). Using `Type=forking` would cause systemd to think the process died immediately. Using `Type=notify` without calling `sd_notify("READY=1")` would cause systemd to time out waiting for readiness notification, then kill the service.

**Why it happens:**
Developers copy service files from other projects that use `Type=forking` (for old-style daemons that fork to background) or `Type=notify` (for services that have sd_notify integration). chromatindb has neither -- it runs in the foreground and exits the main thread only on SIGTERM/SIGINT via asio::signal_set.

**How to avoid:**
Use `Type=simple` (or `Type=exec` on systemd 240+). `Type=simple` is correct because chromatindb starts, blocks on `ioc.run()`, and never forks or calls `sd_notify()`. `Type=exec` is slightly better because systemd considers the service started only after the `execve()` succeeds (catches bad binary path immediately). Do not add sd_notify integration -- it adds a build dependency (libsystemd) for zero benefit since chromatindb initializes fast and logs its startup.

**Warning signs:**
- `systemctl start chromatindb` reports failed/timed-out despite the daemon running fine
- `systemctl status` shows "activating" forever
- systemd kills the daemon after `TimeoutStartSec`

**Phase to address:**
systemd unit file creation phase. Verify with `systemctl start chromatindb && systemctl status chromatindb` during testing.

---

### Pitfall 3: TimeoutStopSec Too Short for Graceful Shutdown with In-Flight Sync

**What goes wrong:**
chromatindb has a graceful shutdown path: SIGTERM -> drain coroutines -> save peer list -> thread pool join -> exit. With large blobs (up to 100 MiB) in transit during sync, draining can take meaningful time. If `TimeoutStopSec` is set too low, systemd sends SIGKILL before the drain completes, potentially losing the peer list save or cursor persistence. libmdbx is crash-safe (ACID), but the peer list and cursor compaction writes may be incomplete.

**Why it happens:**
Developers set short stop timeouts thinking "it's just a database, how long can shutdown take?" without considering that in-flight sync of a 100 MiB blob over a slow connection can take tens of seconds, plus the thread pool join waits for in-flight crypto operations.

**How to avoid:**
Set `TimeoutStopSec=120` in the service file (matches the `inactivity_timeout_seconds` default). Add a comment in the service file explaining why. Document that operators running nodes with very large blobs over slow networks may need to increase this value.

**Warning signs:**
- `systemctl stop` logs show "Timed out waiting for service to stop" followed by SIGKILL
- Node logs show startup integrity scan reporting issues after restart
- Peer list file is empty or missing after restart despite having had peers

**Phase to address:**
systemd unit file creation phase. The service file must include an appropriate `TimeoutStopSec` with a comment explaining the rationale.

---

### Pitfall 4: Path Inconsistency Across dist/ Files

**What goes wrong:**
The dist/ package has 6-8 files that reference shared paths (data directory, UDS socket path, log directory, config directory, binary paths). A mismatch between any two files causes silent runtime failure:
- Node config sets `uds_path: /run/chromatindb/node.sock` but relay config references a different socket path -- relay cannot connect.
- systemd unit has `ExecStart=/usr/local/bin/chromatindb run --config /etc/chromatindb/chromatindb.conf` but install.sh copies config as `chromatindb.json` -- daemon fails to start.
- tmpfiles.d creates a directory that StateDirectory= also creates with different permissions.

**Why it happens:**
Files are written independently across different formats (INI for systemd, JSON for configs, shell for install.sh, specialized for sysusers/tmpfiles). No compiler or linter validates cross-file path consistency.

**How to avoid:**
1. Define all canonical paths in a single location (e.g., comment block at top of install.sh or a shared `paths.env`).
2. Build all dist/ files in a single phase so path decisions are made once and propagated immediately.
3. Integration test: run install.sh on a clean system, start both services, verify both are running and relay connects to node via UDS.

**Warning signs:**
- `systemctl status` shows "failed" with path-related errors
- Relay logs "failed to connect to node via UDS" despite node running fine
- Data appears in unexpected directories

**Phase to address:**
Distribution packaging phase. All dist/ files should be created in the same session with a single source of truth for paths.

---

### Pitfall 5: Documentation Describes Features That Do Not Exist or Omits Features That Do

**What goes wrong:**
db/PROTOCOL.md header says "40 message types" but the project has 58 message type enums. Root README.md says "Current release: v1.3.0" but v1.4.0 shipped. db/README.md says "551 unit tests" but there are 560+. Partial updates create a worse situation than fully stale docs because readers trust "recently updated" documents more.

**Why it happens:**
Multiple documentation surfaces (root README.md, db/README.md, db/PROTOCOL.md) reference overlapping facts. Updating one file while forgetting another creates internal inconsistency.

**How to avoid:**
Create a documentation audit checklist before writing. Every numerical claim must be verified against current source:
- Message type enum count from FlatBuffers schema
- Unit test count from `ctest --test-dir build -N | tail -1`
- Docker integration test count from test script
- Relay filter allowed type count from source (currently 38)
- NodeInfoResponse supported_types count from source (currently 38)
- Version string in ALL readme files
- Config option list matches current config.h fields

**Warning signs:**
- Numbers in docs don't match across files
- Grep for old version strings finds hits in "updated" docs

**Phase to address:**
Documentation phase. Build the checklist FIRST, verify all values, THEN write. Do not write prose first and verify later.

---

### Pitfall 6: PROTOCOL.md Wire Format Descriptions Diverge from Implementation

**What goes wrong:**
PROTOCOL.md describes byte-level wire formats for all message types. If any format description has a wrong byte offset, wrong field order, wrong endianness, or wrong length, a third-party SDK implementer writes code that produces invalid messages. The node silently drops them (bad crypto or validation failure). The documentation becomes actively harmful.

**Why it happens:**
Wire format documentation is written by reading source code and transcribing it into prose. Human transcription introduces errors. The custom binary formats (not FlatBuffers payloads) are especially risky: NodeInfoResponse, WriteAck, DeleteAck, ExistsResponse, BatchReadResponse, PeerInfoResponse, and other v1.4.0 query responses all use hand-crafted binary layouts.

**How to avoid:**
For each message type documented:
1. Write the prose description
2. Cross-reference against the FlatBuffers schema for field order and types
3. Cross-reference against the handler code for payload encoding (especially hand-crafted binary formats)
4. Verify byte offset arithmetic adds up (field sizes must sum to documented total size)

**Warning signs:**
- Byte offset arithmetic in docs doesn't add up
- Copy-paste errors between similar message types
- SDK implementers report "connection drops" after sending messages that match docs

**Phase to address:**
Documentation phase. Every binary format description must include a verification step against the encoder source code.

---

### Pitfall 7: Missing systemd Security Hardening

**What goes wrong:**
A service file with only `User=chromatindb` provides minimal isolation. A vulnerability in the daemon gives the attacker read access to the entire filesystem, write access to any file the user can write, and the ability to call any syscall. Without hardening, `systemd-analyze security chromatindb.service` scores 9.0+ (UNSAFE).

**Why it happens:**
Developers ship a minimal service file that "works" and skip hardening because it requires understanding many directives. The daemon appears to function identically with or without hardening, so the omission is invisible during testing.

**How to avoid:**
Include these directives in the service file:
```ini
ProtectSystem=strict
ProtectHome=yes
NoNewPrivileges=yes
PrivateTmp=yes
PrivateDevices=yes
ReadWritePaths=/var/lib/chromatindb /var/log/chromatindb /run/chromatindb
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
RestrictSUIDSGID=yes
```
These are all safe for chromatindb because it only needs: network access (TCP listen/connect), filesystem access (data dir, log dir, UDS socket), and standard crypto syscalls.

Verify with: `systemd-analyze security chromatindb.service` -- target score below 5.0.

**Warning signs:**
- `systemd-analyze security` scores above 7.0
- Service file has no Protect*/Private*/Restrict* directives

**Phase to address:**
systemd unit file creation phase. Add hardening directives from the start -- retrofitting is harder because each directive needs testing.

---

### Pitfall 8: sysusers.d / tmpfiles.d Redundant with systemd Unit Directives

**What goes wrong:**
Developers ship sysusers.d (creates chromatindb user), tmpfiles.d (creates directories), AND the systemd unit has `StateDirectory=`, `LogsDirectory=`, `RuntimeDirectory=`. Two mechanisms manage the same directories, leading to ownership conflicts or confusion about which is authoritative.

**Why it happens:**
Multiple guides recommend different approaches. All are correct in isolation, but combining them creates redundancy.

**How to avoid:**
Use this split:
- **sysusers.d:** User/group creation only (this runs early at boot, before services). This is the only mechanism for user creation.
- **systemd unit directives** (`StateDirectory=chromatindb`, `LogsDirectory=chromatindb`, `RuntimeDirectory=chromatindb`): Directory creation. These auto-create directories owned by the configured `User=` when the service starts.
- **tmpfiles.d:** Only if directories must exist when the service is NOT running (e.g., for manual inspection). For chromatindb, this is unnecessary -- directories only need to exist while the service runs.

Never use both tmpfiles.d and StateDirectory= for the same path.

**Warning signs:**
- `ls -la /var/lib/chromatindb` shows wrong owner
- tmpfiles.d config references paths also in StateDirectory=
- Install script also `mkdir -p` the same directories

**Phase to address:**
Distribution packaging design phase. Decide the mechanism once before writing any files.

---

### Pitfall 9: Default Config With Relative Paths Fails Under systemd

**What goes wrong:**
The production config ships with `"data_dir": "./data"`. Under systemd, WorkingDirectory defaults to root (`/`), so `./data` resolves to `/data`, creating data in an unexpected location. Operators start the service, it appears to work, but data is not in `/var/lib/chromatindb` where they expect it.

**Why it happens:**
Config defaults are designed for development (relative paths work when running from the project directory). Nobody tests the config under systemd's execution environment.

**How to avoid:**
Ship a production config with absolute paths matching the systemd unit layout:
```json
{
  "data_dir": "/var/lib/chromatindb",
  "log_file": "/var/log/chromatindb/node.log",
  "uds_path": "/run/chromatindb/node.sock"
}
```
The install script installs this to `/etc/chromatindb/chromatindb.conf` (only if file doesn't already exist). The systemd unit references it explicitly: `ExecStart=/usr/local/bin/chromatindb run --config /etc/chromatindb/chromatindb.conf`.

**Warning signs:**
- Data directory created in unexpected location after service start
- Config file not found errors in journal
- `--config` flag missing from ExecStart in service file

**Phase to address:**
Config template and systemd unit creation phase. Design them together so paths are consistent.

---

### Pitfall 10: Install Script Overwrites Operator-Modified Configs

**What goes wrong:**
Operator customizes `/etc/chromatindb/chromatindb.conf` with their bootstrap_peers, allowed_keys, and quota settings. Running install.sh to update binaries also overwrites their config with the default.

**Why it happens:**
Install scripts written as sequential "copy everything" without checking what already exists.

**How to avoid:**
Config install must be conditional:
```bash
if [ ! -f /etc/chromatindb/chromatindb.conf ]; then
    install -m 0640 -o root -g chromatindb dist/chromatindb.conf /etc/chromatindb/chromatindb.conf
else
    echo "Config already exists, skipping. See dist/chromatindb.conf for new defaults."
fi
```
Binaries and service files: always overwrite (vendor-managed). Config files: never overwrite. The install script must call `systemctl daemon-reload` after updating service files.

**Warning signs:**
- Script exits on second run with errors
- Operator's customized config gets overwritten on upgrade
- `systemctl daemon-reload` not called after service file changes

**Phase to address:**
Install script phase. Test by running the script twice on the same system.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Skipping install script testing on a fresh VM | Save 30 min of VM setup | Script breaks on real deployments, bad first impression | Never -- always test on a clean system |
| Hardcoding paths instead of using systemd directives | Simpler service file | Breaks on non-standard installations, ownership bugs | Never -- StateDirectory is zero extra effort |
| Copying config unconditionally in install script | Simpler script logic | Overwrites operator customizations on upgrade | Never -- always check before overwriting |
| Documenting message types from memory | Faster writing | Wrong wire formats cause SDK bugs | Never -- always verify against source |
| Shipping logrotate for spdlog-rotated logs | "Looks professional" | Dual rotation causes log loss | Never -- document spdlog's built-in rotation instead |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| systemd + spdlog logging | Shipping logrotate config for spdlog-managed files | Document that spdlog handles rotation internally; skip logrotate entirely for these paths |
| systemd + UDS socket | Not setting `RuntimeDirectory=` for UDS path | Use `RuntimeDirectory=chromatindb` and configure UDS at `/run/chromatindb/node.sock` |
| systemd + SIGTERM | Setting `KillMode=control-group` which SIGTERMs child processes too | Use `KillMode=mixed` so only main process gets SIGTERM, thread pool children get SIGKILL after timeout |
| systemd + config reload | Not supporting `ExecReload=` for SIGHUP | Add `ExecReload=/bin/kill -HUP $MAINPID` to leverage existing SIGHUP handler |
| systemd + metrics | No way to trigger SIGUSR1 from systemd | Document `systemctl kill -s SIGUSR1 chromatindb` for operators |
| sysusers.d + Dockerfile | Dockerfile creates user with `groupadd/useradd`, sysusers.d uses different UID/GID | Ensure sysusers.d does not hardcode UID/GID (use `-` for system-allocated IDs); Docker and host users are independent |
| Install script + SELinux | Binary installed to non-standard path lacks correct SELinux context | Use `restorecon` after install, or install to standard `/usr/local/bin/` which inherits correct context |
| Relay + Node ordering | Relay starts before node UDS socket exists | Relay unit must have `After=chromatindb.service` + `Wants=chromatindb.service` |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| systemd journal + JSON log format double-encoding | Journal stores structured data; JSON-in-journal makes parsing harder | Use `log_format=text` with journal (systemd captures metadata automatically) or `log_format=json` with file only | When operators try to parse journal entries with `jq` and get escaped JSON |
| Config at `/etc/` with `data_dir` as relative path | Data goes to root filesystem, fills up root partition | Always use absolute paths in production config | First deployment on a real server |
| tmpfiles.d age-based cleanup on data directory | tmpfiles `q` or `Q` rules with age spec can delete blob data | Never add age-based cleanup rules for the data directory; only use `d` (create) without age | When tmpfiles timer fires and data is older than the age threshold |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Running service as root | Compromised daemon = root shell | `User=chromatindb` + `Group=chromatindb` in service file |
| No systemd hardening directives | Compromised daemon can read/write entire filesystem | Add ProtectSystem=strict, ProtectHome=yes, NoNewPrivileges=yes, PrivateTmp=yes, PrivateDevices=yes |
| master.key file readable by other users | Encryption at rest is meaningless if any user can read the key | StateDirectory sets 0750; master.key is already 0600. Verify ownership in install script. |
| Config file with allowed_keys readable by world | Reveals which pubkeys are authorized | Install config with 0640 root:chromatindb permissions |
| UDS socket accessible to all users | Any local process can send commands to the node | RuntimeDirectory with 0750; socket inherits directory permissions |
| Install script with TOCTOU on file permissions | Files briefly world-readable during install | Use `install -o chromatindb -g chromatindb -m 0750` to set owner+permissions atomically |

## UX Pitfalls

| Pitfall | User Impact | Better Approach |
|---------|-------------|-----------------|
| Install script with no `--uninstall` option | Operators cannot cleanly remove the software | Ship an uninstall command or document manual removal steps |
| README assumes reader built from source | Production operators installing from dist/ cannot follow "mkdir build" instructions | Separate "Building from Source" and "Installing from Package" sections |
| PROTOCOL.md has no table of contents | 58 message types in one document is unnavigable | Add a TOC at the top with anchors to each message type section |
| Default config example missing comments | Operators don't know what each option does | Ship a commented config or a separate config-reference section in README |
| Relay is not clearly documented as optional | Operators install both services when they only need the node | Document when relay is needed (TCP clients with PQ handshake) vs when it is not (node-to-node only) |

## "Looks Done But Isn't" Checklist

- [ ] **systemd unit:** Has `ExecReload=/bin/kill -HUP $MAINPID` -- verify SIGHUP config reload works via `systemctl reload chromatindb`
- [ ] **systemd unit:** Has security hardening directives -- verify `systemd-analyze security chromatindb.service` scores below 5.0
- [ ] **systemd unit:** Has `After=network-online.target` + `Wants=network-online.target` -- chromatindb needs network for peer connections
- [ ] **systemd unit:** Has `TimeoutStopSec=120` -- graceful shutdown with in-flight sync needs time
- [ ] **systemd unit:** Has `KillMode=mixed` -- only main process gets SIGTERM, thread pool children cleaned up by SIGKILL after timeout
- [ ] **Relay unit:** Has `After=chromatindb.service` + `Wants=chromatindb.service` -- relay must not start before node
- [ ] **Default config:** All paths are absolute (`data_dir`, `log_file`, `uds_path`) -- relative paths break under systemd
- [ ] **Install script:** Idempotent -- running twice produces no errors and does not overwrite customized config
- [ ] **Install script:** Calls `systemctl daemon-reload` after installing/updating service files
- [ ] **README.md:** Version string matches current release -- verify against PROJECT.md or git tags
- [ ] **README.md:** Test count matches `ctest -N` output
- [ ] **README.md:** Message type count matches FlatBuffers schema enum
- [ ] **PROTOCOL.md:** All 58 message types documented (not just the 40 from v1.3.0)
- [ ] **PROTOCOL.md:** Hand-crafted binary format byte offsets verified against encoder source code
- [ ] **sysusers.d:** Uses `-` for UID/GID (system-allocated, not hardcoded)
- [ ] **No logrotate:** Confirm no logrotate config shipped for spdlog-managed log paths
- [ ] **dist/ package:** Both `chromatindb` and `chromatindb_relay` binaries included

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Wrong systemd Type= | LOW | Fix service file, `systemctl daemon-reload`, restart |
| logrotate fighting spdlog | LOW | Remove logrotate config, restart daemon (spdlog reopens cleanly) |
| TimeoutStopSec too short (data loss) | MEDIUM | libmdbx is crash-safe, but peer list/cursors may need recovery. Increase timeout, restart. Check integrity scan output. |
| Stale docs shipped | MEDIUM | Audit and correct. Damage is to credibility with SDK implementers who may have built against wrong wire format. |
| Install script overwrote custom config | HIGH | Operator must restore from backup. If no backup, recreate config from memory. |
| Wrong wire format in PROTOCOL.md | HIGH | SDK implementers already wrote code against wrong docs. Requires coordinated doc fix + SDK patch. |
| Hardcoded paths don't match system | LOW | Fix service file + config, daemon-reload, restart |
| Missing service dependency (relay before node) | LOW | Add After=/Wants= to relay unit, daemon-reload |
| Missing security hardening | MEDIUM | Add directives to unit file, daemon-reload, restart. No data loss but requires testing that hardening doesn't break functionality. |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| spdlog vs logrotate conflict | dist/ packaging | No logrotate config for spdlog paths; README documents spdlog rotation |
| Wrong service Type= | systemd unit creation | `systemctl start && systemctl status` shows "active (running)" |
| TimeoutStopSec too short | systemd unit creation | `systemctl stop` completes without SIGKILL in journal |
| Path inconsistency | dist/ packaging (single session) | Install on fresh system, both services start and communicate |
| Stale docs (numbers/versions) | Documentation refresh | Checklist verified against source code before merge |
| Wire format doc errors | Documentation refresh | Every binary format cross-referenced against encoder source |
| Missing security hardening | systemd unit creation | `systemd-analyze security` score below 5.0 |
| tmpfiles.d/StateDirectory overlap | dist/ packaging design | Only one mechanism per directory; no tmpfiles.d if StateDirectory used |
| Relative paths in default config | Config template creation | Service starts correctly with shipped config on fresh install |
| Config overwrite on upgrade | Install script | Run script twice, config preserved |

## Sources

- [systemd.service man page](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html) -- Type=, TimeoutStopSec=, ExecReload= semantics
- [systemd.exec man page](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html) -- StateDirectory=, LogsDirectory=, RuntimeDirectory=, security hardening directives
- [systemd.kill man page](https://www.freedesktop.org/software/systemd/man/latest/systemd.kill.html) -- KillMode=, SendSIGKILL= behavior
- [tmpfiles.d man page](https://www.freedesktop.org/software/systemd/man/latest/tmpfiles.d.html) -- directory creation rules, ownership semantics
- [daemon(7) man page](https://www.freedesktop.org/software/systemd/man/latest/daemon.html) -- new-style daemon recommendations (Type=simple, sd_notify)
- [spdlog issue #3464](https://github.com/gabime/spdlog/issues/3464) -- spdlog + external logrotate interaction; append mode; no external rotation support; SIGHUP handling recommendation from maintainer
- [logrotate man page](https://man7.org/linux/man-pages/man8/logrotate.8.html) -- copytruncate window vulnerability
- [systemd service hardening gist](https://gist.github.com/ageis/f5595e59b1cddb1513d1b425a323db04) -- comprehensive hardening directive reference
- [Ctrl blog: systemd hardening 101](https://www.ctrl.blog/entry/systemd-service-hardening.html) -- practical hardening walkthrough
- [systemd-tmpfiles guide](https://blogs.reliablepenguin.com/2025/12/22/systemd-tmpfiles-the-unsung-janitor-of-run-and-tmp) -- tmpfiles.d best practices, redundancy with StateDirectory
- Source analysis: `db/logging/logging.cpp` -- confirms `rotating_file_sink_mt` handles its own rotation internally
- Source analysis: `db/main.cpp` -- confirms foreground daemon (ioc.run() blocks, no fork, no sd_notify)
- Source analysis: `db/net/server.cpp` line 15 -- confirms SIGTERM/SIGINT handling via `asio::signal_set`
- Source analysis: `db/peer/peer_manager.h` lines 292-293 -- confirms SIGHUP/SIGUSR1 handling via `asio::signal_set`
- Source analysis: `CMakeLists.txt` -- confirms two binaries: `chromatindb` (node) and `chromatindb_relay` (relay)

---
*Pitfalls research for: chromatindb v1.5.0 Documentation & Distribution*
*Researched: 2026-03-28*
