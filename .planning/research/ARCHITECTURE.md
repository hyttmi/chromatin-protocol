# Architecture: v1.5.0 Documentation & Distribution Integration

**Domain:** Production dist/ packaging and documentation refresh for C++ database daemon
**Researched:** 2026-03-28
**Confidence:** HIGH (based on direct source code analysis + systemd/packaging official documentation)

## Executive Summary

v1.5.0 adds two non-code deliverables: (1) comprehensive documentation updates across README.md, db/README.md, and db/PROTOCOL.md, and (2) a production distribution kit in a new `dist/` directory containing systemd service units, default config files, logrotate configuration, tmpfiles.d, sysusers.d, and an install script. Neither deliverable modifies any C++ source or the CMake build system. The dist/ directory is a pure packaging layer -- shell scripts and configuration files -- that consumes the binaries CMake already produces. Documentation updates are text-only edits to existing files. Zero compilation changes. Zero new dependencies.

## Recommended Architecture

### System Overview -- Where dist/ Fits

```
Repository Root
├── db/                          # EXISTING: Node library + main.cpp
│   ├── README.md                # MODIFY: Comprehensive docs refresh
│   └── PROTOCOL.md              # MODIFY: All 58 message types documented
├── relay/                       # EXISTING: Relay binary (no changes)
├── README.md                    # MODIFY: Root README refresh
├── CMakeLists.txt               # EXISTING: No changes
├── Dockerfile                   # EXISTING: No changes
├── deploy/                      # EXISTING: Benchmark/Docker Compose
│   ├── docker-compose.yml
│   └── configs/
├── dist/                        # NEW: Production distribution kit
│   ├── systemd/
│   │   ├── chromatindb.service
│   │   └── chromatindb-relay.service
│   ├── config/
│   │   ├── chromatindb.json
│   │   └── chromatindb-relay.json
│   ├── logrotate/
│   │   └── chromatindb
│   ├── tmpfiles.d/
│   │   └── chromatindb.conf
│   ├── sysusers.d/
│   │   └── chromatindb.conf
│   └── install.sh
└── tests/                       # EXISTING: Integration tests (no changes)
```

### Component Responsibilities

| Component | Responsibility | Status |
|-----------|----------------|--------|
| `dist/systemd/` | systemd unit files for node + relay daemons | NEW |
| `dist/config/` | Production-ready default JSON configs | NEW |
| `dist/logrotate/` | External log rotation for non-spdlog consumers | NEW |
| `dist/tmpfiles.d/` | Runtime directory creation at boot | NEW |
| `dist/sysusers.d/` | System user/group creation at install | NEW |
| `dist/install.sh` | Copies artifacts to FHS-standard paths | NEW |
| `README.md` (root) | Project introduction, points to db/README.md | MODIFY |
| `db/README.md` | Full user documentation: build, config, usage, relay | MODIFY |
| `db/PROTOCOL.md` | Complete wire protocol reference (58 types) | MODIFY |

### What Does NOT Change

No changes to: CMakeLists.txt (root or db/), any `.cpp` or `.h` file, Dockerfile, deploy/ directory, tests/ directory, relay/ source code, loadgen/, tools/, sanitizers/. The build produces the same binaries. dist/ is a consumer of those binaries, not an input to the build.

## New Component: dist/

### dist/systemd/ -- Service Unit Files

Two service files, one per binary. Both follow the same pattern.

#### chromatindb.service

```ini
[Unit]
Description=chromatindb - Post-quantum secure database node
Documentation=https://github.com/user/chromatin-protocol
After=network-online.target
Wants=network-online.target

[Service]
Type=exec
User=chromatindb
Group=chromatindb
ExecStart=/usr/local/bin/chromatindb run --config /etc/chromatindb/chromatindb.json
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=5

# Directories
StateDirectory=chromatindb
RuntimeDirectory=chromatindb
LogsDirectory=chromatindb
ConfigurationDirectory=chromatindb

# Security hardening
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
ProtectClock=yes
ProtectHostname=yes
ProtectKernelLogs=yes
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
MemoryDenyWriteExecute=yes
LockPersonality=yes

# Allow write access to data and log directories
ReadWritePaths=/var/lib/chromatindb /var/log/chromatindb /run/chromatindb

[Install]
WantedBy=multi-user.target
```

**Design decisions:**

- **Type=exec** (not Type=notify): chromatindb does not call `sd_notify()` -- it runs in foreground and systemd tracks the main process directly. `exec` is more accurate than `simple` because it waits for the exec to succeed before considering the service started.
- **ExecReload=/bin/kill -HUP**: Leverages the existing SIGHUP handler for config reload (allowed_keys, trusted_peers, rate limits, quotas).
- **StateDirectory=chromatindb**: Creates /var/lib/chromatindb owned by the service user. This is where mdbx databases and identity keys live.
- **RuntimeDirectory=chromatindb**: Creates /run/chromatindb for the UDS socket path.
- **ProtectSystem=strict + ReadWritePaths**: Mounts the entire filesystem read-only, then explicitly allows writes only to the three directories the daemon needs. This is the strongest usable protection for a database daemon.
- **MemoryDenyWriteExecute=yes**: Safe because chromatindb does not JIT, dlopen, or generate executable code at runtime. liboqs/libsodium use constant-time code from pre-compiled object code.

#### chromatindb-relay.service

Same pattern but simpler -- relay has no storage directory, only needs the UDS socket path and its own config.

```ini
[Unit]
Description=chromatindb-relay - PQ-authenticated client relay
After=chromatindb.service
Wants=chromatindb.service

[Service]
Type=exec
User=chromatindb
Group=chromatindb
ExecStart=/usr/local/bin/chromatindb_relay run --config /etc/chromatindb/chromatindb-relay.json
Restart=on-failure
RestartSec=5

# Same security hardening as node
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
ReadWritePaths=/var/log/chromatindb /run/chromatindb

[Install]
WantedBy=multi-user.target
```

**Design decisions:**

- **After=chromatindb.service**: The relay connects to the node via UDS, so it must start after the node. `Wants=` is soft -- the relay can still start if the node is not enabled, but ordering is correct if both are present.
- **No ExecReload**: The relay does not have a SIGHUP handler. Config changes require restart.
- **No StateDirectory**: The relay has no persistent state. It creates ephemeral UDS connections to the node.
- **Same user/group**: Both run as `chromatindb`. The UDS socket permissions rely on shared group membership.

### dist/config/ -- Default Configuration Files

#### chromatindb.json (production defaults)

```json
{
  "bind_address": "0.0.0.0:4200",
  "data_dir": "/var/lib/chromatindb",
  "log_level": "info",
  "log_file": "/var/log/chromatindb/node.log",
  "log_max_size_mb": 50,
  "log_max_files": 5,
  "log_format": "json",
  "uds_path": "/run/chromatindb/node.sock",
  "max_peers": 64,
  "sync_interval_seconds": 60,
  "worker_threads": 0,
  "inactivity_timeout_seconds": 120,
  "expiry_scan_interval_seconds": 60,
  "compaction_interval_hours": 6
}
```

**Rationale:**

- **data_dir = /var/lib/chromatindb**: FHS-compliant state directory. Matches `StateDirectory=` in the service unit.
- **log_file with JSON format**: Production nodes should produce machine-parseable logs. spdlog's rotating_file_sink handles rotation internally (50 MiB x 5 files = 250 MiB max disk for logs).
- **uds_path = /run/chromatindb/node.sock**: RuntimeDirectory from systemd ensures /run/chromatindb exists at boot.
- **worker_threads = 0**: Auto-detect. Correct for any hardware.
- Fields intentionally omitted from defaults (use their built-in zero-means-disabled): `max_storage_bytes`, `rate_limit_bytes_per_sec`, `rate_limit_burst`, `allowed_keys`, `trusted_peers`, `bootstrap_peers`, `sync_namespaces`, `namespace_quota_bytes`, `namespace_quota_count`, `namespace_quotas`. The operator adds these as needed.

#### chromatindb-relay.json (production defaults)

```json
{
  "bind_address": "0.0.0.0",
  "bind_port": 4201,
  "uds_path": "/run/chromatindb/node.sock",
  "identity_key_path": "/var/lib/chromatindb/relay.key",
  "log_level": "info",
  "log_file": "/var/log/chromatindb/relay.log"
}
```

### dist/logrotate/ -- External Log Rotation

Although spdlog handles its own rotation internally, a logrotate config is included as defense-in-depth for operators who prefer system-managed rotation or who change `log_file` to a non-rotating path.

```
/var/log/chromatindb/*.log {
    daily
    missingok
    rotate 7
    compress
    delaycompress
    notifempty
    copytruncate
    create 0640 chromatindb chromatindb
}
```

**Design decisions:**

- **copytruncate**: spdlog holds the file descriptor open. `copytruncate` copies then truncates in place, which is the only safe option without a signal-based reopen handler. spdlog's own rotation makes this a belt-and-suspenders measure.
- **daily + rotate 7**: Complements spdlog's size-based rotation with time-based cleanup.

### dist/tmpfiles.d/ -- Runtime Directory Bootstrap

```
# /usr/lib/tmpfiles.d/chromatindb.conf
# Create runtime directory for chromatindb UDS socket
d /run/chromatindb 0750 chromatindb chromatindb -
```

**Why needed:** The `RuntimeDirectory=` directive in the service unit creates this when the service starts. But tmpfiles.d provides a fallback for systems where the directory is needed before the service starts (e.g., if an operator manually runs the binary outside systemd). In practice, the service unit's `RuntimeDirectory=` handles the common case. This is a packaging convention, not strictly required.

### dist/sysusers.d/ -- System User Provisioning

```
# /usr/lib/sysusers.d/chromatindb.conf
u chromatindb - "chromatindb daemon" /var/lib/chromatindb /usr/sbin/nologin
```

**What it does:** Creates the `chromatindb` system user and group at package install time (or at boot via `systemd-sysusers`). The user has no login shell and its home directory is the data directory.

**Why sysusers.d over useradd in install.sh:** `sysusers.d` is declarative, idempotent, and runs automatically during package manager operations (dpkg, rpm) via the appropriate triggers. The install script calls `systemd-sysusers` explicitly as a fallback for non-package installs.

### dist/install.sh -- Installation Script

The script handles non-package-manager installation (build from source). It:

1. Checks for root privileges
2. Runs `systemd-sysusers` to create the user/group
3. Copies binaries to `/usr/local/bin/`
4. Copies config files to `/etc/chromatindb/` (skips if files already exist -- never overwrites operator config)
5. Copies systemd units to `/etc/systemd/system/`
6. Copies tmpfiles.d to `/usr/lib/tmpfiles.d/`
7. Copies sysusers.d to `/usr/lib/sysusers.d/`
8. Copies logrotate to `/etc/logrotate.d/`
9. Creates `/var/lib/chromatindb` and `/var/log/chromatindb` with correct ownership
10. Runs `systemd-tmpfiles --create`
11. Runs `systemctl daemon-reload`
12. Prints next steps (keygen, enable, start)

**Design decisions:**

- **Never overwrite existing configs**: `install.sh` checks before copying config files. Protects operator customizations.
- **/usr/local/bin for binaries**: Standard for locally-compiled software. Distinguishes from package-managed /usr/bin.
- **No automatic enable/start**: The script tells the operator what to do but does not activate services. This is respectful of production environments where service activation is a deliberate decision.
- **Idempotent**: Running install.sh twice is safe. It skips existing configs and overwrites binaries + systemd units (which are source-controlled, not operator-edited).

## Documentation Update Architecture

### Root README.md

Current: 14 lines, a stub pointing to db/README.md.
Target: Expand to a meaningful landing page with:

- Project description (the one-paragraph pitch)
- Architecture summary (three-layer diagram)
- Quick start (3 commands to run a node)
- Links to db/README.md (full docs), db/PROTOCOL.md (wire protocol), dist/ (production deployment)
- Build instructions (or link to db/README.md)
- License

Keep it to approximately 100-150 lines. This is the GitHub landing page. Details live in db/README.md.

### db/README.md

Current: 370 lines, covers build, test, config, signals, wire protocol overview, and deployment scenarios.
Target: Comprehensive refresh to cover all v1.4.0 capabilities:

- Update "Current release" to v1.4.0
- Update test count (560+ unit tests)
- Update message type count (58 types)
- Add relay section (build, config, usage) -- currently not documented in README
- Add dist/ deployment section (systemd install, production config)
- Verify all config fields are documented (cross-check with config.h)
- Update scenarios for relay usage
- Add troubleshooting section (common issues)

Estimated size: ~500-600 lines.

### db/PROTOCOL.md

Current: 828 lines, covers transport layer, handshake, all message types through v1.4.0.
Status: Already comprehensive. v1.4.0 Phase 67 documented BatchRead, PeerInfo, and TimeRange types.

Verification needed: Walk through all 58 type IDs and confirm each has a documented section. The current PROTOCOL.md should already be complete after Phase 67 updates, but this verification pass is part of v1.5.0 scope.

## Data Flow

### Install Flow

```
Operator builds from source
    |
    v
cmake --build build/  -->  build/chromatindb, build/chromatindb_relay
    |
    v
cd dist/ && sudo ./install.sh --binaries-dir ../build/
    |
    +-- systemd-sysusers  -->  creates chromatindb user/group
    +-- cp binaries       -->  /usr/local/bin/
    +-- cp configs        -->  /etc/chromatindb/ (skip if exists)
    +-- cp units          -->  /etc/systemd/system/
    +-- cp tmpfiles       -->  /usr/lib/tmpfiles.d/
    +-- cp sysusers       -->  /usr/lib/sysusers.d/
    +-- cp logrotate      -->  /etc/logrotate.d/
    +-- mkdir + chown     -->  /var/lib/chromatindb, /var/log/chromatindb
    +-- daemon-reload
    |
    v
Operator runs:
    chromatindb keygen --data-dir /var/lib/chromatindb
    systemctl enable --now chromatindb
    systemctl enable --now chromatindb-relay  (optional)
```

### Service Runtime Flow

```
systemd starts chromatindb.service
    |
    +-- Creates /run/chromatindb (RuntimeDirectory)
    +-- Runs as chromatindb user
    |
    v
chromatindb run --config /etc/chromatindb/chromatindb.json
    |
    +-- Reads config from /etc/chromatindb/
    +-- Opens mdbx database in /var/lib/chromatindb/
    +-- Writes logs to /var/log/chromatindb/node.log
    +-- Creates UDS at /run/chromatindb/node.sock
    +-- Binds TCP on 0.0.0.0:4200
    |
    v
systemd starts chromatindb-relay.service (After=chromatindb)
    |
    +-- Connects to /run/chromatindb/node.sock
    +-- Binds TCP on 0.0.0.0:4201
    +-- Accepts PQ-authenticated client connections
```

### FHS Path Mapping

| Path | Contents | Owner | Permissions |
|------|----------|-------|-------------|
| `/usr/local/bin/chromatindb` | Node binary | root:root | 0755 |
| `/usr/local/bin/chromatindb_relay` | Relay binary | root:root | 0755 |
| `/etc/chromatindb/chromatindb.json` | Node config | root:chromatindb | 0640 |
| `/etc/chromatindb/chromatindb-relay.json` | Relay config | root:chromatindb | 0640 |
| `/var/lib/chromatindb/` | mdbx databases, identity keys | chromatindb:chromatindb | 0750 |
| `/var/log/chromatindb/` | Log files (node + relay) | chromatindb:chromatindb | 0750 |
| `/run/chromatindb/` | UDS socket | chromatindb:chromatindb | 0750 |

**Config file permissions:** 0640 (root-owned, group-readable by chromatindb). The daemon reads config but should not be able to modify it. This prevents a compromised daemon from altering its own configuration.

## Architectural Patterns

### Pattern 1: Declarative System Integration (sysusers.d + tmpfiles.d)

**What:** Instead of imperative `useradd` and `mkdir` commands scattered in install scripts, use systemd's declarative configuration files that integrate with package managers.

**When to use:** Any daemon that creates system users or runtime directories.

**Trade-offs:**
- Pro: Idempotent, integrates with dpkg/rpm triggers, boot-time verification
- Pro: Self-documenting -- the conf files describe what the system state should be
- Con: Requires systemd (not portable to non-systemd systems)

chromatindb targets systemd-based Linux distributions exclusively. Non-systemd systems can still run the binaries manually -- the dist/ kit is optional convenience, not a runtime dependency.

### Pattern 2: Config Files Never Overwritten

**What:** The install script copies default configs to `/etc/chromatindb/` only if no config file already exists at that path. Subsequent install.sh runs skip config files entirely.

**When to use:** Any install script for services with operator-customized configuration.

**Trade-offs:**
- Pro: Operator customizations are never lost
- Pro: Running install.sh to update binaries is safe
- Con: New config options in defaults do not automatically appear in existing installations

This is the standard behavior expected by system administrators. The operator reads the release notes and updates their config as needed.

### Pattern 3: Separation of Packaging from Build

**What:** dist/ contains zero CMake integration. It does not use `install()` targets, CPack, or configure_file. It is a standalone directory of shell scripts and config files that assume binaries exist in a build directory.

**When to use:** When the project already has a working build system and packaging is a separate concern. When CPack overhead is not justified by the project's distribution model.

**Trade-offs:**
- Pro: Zero CMake complexity added. The existing CMakeLists.txt is already complex (190 lines, 8 FetchContent dependencies)
- Pro: dist/ is understandable without CMake knowledge
- Pro: Works with any binary source (local build, CI artifact, Docker extract)
- Con: No automatic version stamping in service files (trivially addressed by reading `chromatindb version` output)
- Con: No DEB/RPM generation (YAGNI -- the project is pre-release)

**Rationale:** CMake's `install()` and CPack are designed for library distribution and package generation. chromatindb is a daemon binary, not a library. The dist/ script approach is simpler, more transparent, and matches how operators actually deploy custom-built daemons.

## Anti-Patterns

### Anti-Pattern 1: Modifying CMakeLists.txt for dist/

**What people do:** Add install() targets, configure_file() for service units, CPack configuration.

**Why it's wrong:** Adds complexity to a build system that is already 190 lines with 8 FetchContent dependencies. The CMakeLists.txt serves one purpose: building the software. Mixing packaging concerns creates coupling between build configuration and deployment layout.

**Do this instead:** Keep dist/ as a standalone packaging layer. The install script takes a `--binaries-dir` argument and copies from there.

### Anti-Pattern 2: Running Services as Root

**What people do:** Skip user creation, run daemon as root because "it's easier."

**Why it's wrong:** A compromised daemon with root access owns the entire system. chromatindb opens network sockets from untrusted peers and processes untrusted data.

**Do this instead:** Dedicated system user with minimal privileges. The systemd unit's security directives confine the process to only the directories it needs.

### Anti-Pattern 3: logrotate as Primary Rotation

**What people do:** Rely solely on logrotate for log rotation, using `postrotate` scripts to send SIGHUP.

**Why it's wrong for chromatindb:** SIGHUP already means "reload config" in chromatindb. Overloading it for log rotation would be a protocol violation. Also, spdlog already rotates internally.

**Do this instead:** Use spdlog's built-in rotating_file_sink as primary. Include logrotate as a safety net with `copytruncate` (no signal needed).

### Anti-Pattern 4: Documentation Duplication Between README and PROTOCOL

**What people do:** Repeat wire format details in README.md and PROTOCOL.md, leading to drift.

**Why it's wrong:** When one is updated and the other is not, users get confused. The v1.3.0 and v1.4.0 updates already showed this risk -- PROTOCOL.md was updated in the same phase as implementation, but README.md wire protocol sections can drift.

**Do this instead:** README.md provides a high-level overview ("58 message types, see PROTOCOL.md") and links to PROTOCOL.md for details. PROTOCOL.md is the single source of truth for wire format.

## Integration Points

### New Files (dist/)

| File | Depends On | Depended On By |
|------|-----------|----------------|
| `dist/install.sh` | Build output (binaries), all dist/ configs | Operator |
| `dist/systemd/chromatindb.service` | Binary path, config path, user/group | systemd |
| `dist/systemd/chromatindb-relay.service` | Binary path, config path, user/group, node service | systemd |
| `dist/config/chromatindb.json` | Config schema from db/config/config.h | chromatindb binary |
| `dist/config/chromatindb-relay.json` | Config schema from relay/config/relay_config.h | chromatindb_relay binary |
| `dist/sysusers.d/chromatindb.conf` | None | install.sh, systemd-sysusers |
| `dist/tmpfiles.d/chromatindb.conf` | sysusers.d (user must exist) | install.sh, systemd-tmpfiles |
| `dist/logrotate/chromatindb` | Log file paths from config | logrotate daemon |

### Modified Files (Documentation)

| File | What Changes | Integration Risk |
|------|-------------|-----------------|
| `README.md` (root) | Expanded from 14 lines to ~100-150 | None (text only) |
| `db/README.md` | Updated from 370 lines to ~500-600 | None (text only) |
| `db/PROTOCOL.md` | Verification pass, minor corrections | None (text only) |

### Internal Boundaries

| Boundary | Communication | Notes |
|----------|---------------|-------|
| dist/install.sh -> build output | Filesystem copy | install.sh takes `--binaries-dir` arg |
| dist/config/*.json -> binary config parsers | JSON schema compatibility | Config files must match what `config::load_config()` and `relay_config::load_relay_config()` expect |
| systemd units -> binary CLI | ExecStart command line | Must match the binary's CLI: `chromatindb run --config <path>` |
| logrotate -> spdlog | `copytruncate` (no signal) | No coordination needed; spdlog handles its own FD |

## Suggested Build Order

### Phase 1: dist/ Production Kit

Build dist/ first because documentation will reference it. Order within dist/:

1. **sysusers.d/chromatindb.conf** -- No dependencies, defines the user/group everything else relies on
2. **tmpfiles.d/chromatindb.conf** -- Depends on user existing
3. **config/chromatindb.json** -- Requires cross-checking with db/config/config.h for field names
4. **config/chromatindb-relay.json** -- Requires cross-checking with relay/config/relay_config.h
5. **systemd/chromatindb.service** -- References config path, binary path, user/group
6. **systemd/chromatindb-relay.service** -- References config path, binary path, plus After= dependency
7. **logrotate/chromatindb** -- References log file paths from config
8. **install.sh** -- Orchestrates all of the above; write last so all paths are finalized

### Phase 2: Documentation Refresh

Documentation references dist/ paths and describes current functionality. Order:

1. **db/PROTOCOL.md verification** -- Walk all 58 type IDs, confirm documentation completeness. This is the foundation other docs reference.
2. **db/README.md update** -- Full refresh including relay docs, dist/ deployment, updated counts. Largest writing effort.
3. **README.md (root) update** -- Last, because it links to and summarizes db/README.md content.

### Dependency Graph

```
sysusers.d ─┐
             ├─> tmpfiles.d ─┐
config/*.json ───────────────├─> systemd/*.service ─┐
                              │                      ├─> install.sh
logrotate ────────────────────┘                      │
                                                      v
                                              PROTOCOL.md verification
                                                      │
                                              db/README.md update
                                                      │
                                              README.md (root) update
```

## Sources

- [systemd.service(5) -- Official service unit documentation](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)
- [systemd.exec(5) -- Execution environment options including security hardening](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)
- [tmpfiles.d(5) -- Temporary files and directories configuration](https://www.freedesktop.org/software/systemd/man/latest/tmpfiles.d.html)
- [systemd hardening options (community reference)](https://gist.github.com/ageis/f5595e59b1cddb1513d1b425a323db04)
- [systemd/Sandboxing -- ArchWiki](https://wiki.archlinux.org/title/Systemd/Sandboxing)
- [spdlog and external logrotate (Issue #3464)](https://github.com/gabime/spdlog/issues/3464)
- Direct analysis of: CMakeLists.txt, db/config/config.h, relay/config/relay_config.h, db/main.cpp, relay/relay_main.cpp, Dockerfile, existing deploy/ structure

---
*Architecture research for: v1.5.0 Documentation & Distribution*
*Researched: 2026-03-28*
