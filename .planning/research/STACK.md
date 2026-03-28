# Stack Research: v1.5.0 Documentation & Distribution

**Domain:** Production distribution packaging and documentation for C++20 database daemon
**Researched:** 2026-03-28
**Confidence:** HIGH

## Scope

This research covers ONLY what is needed for v1.5.0: production dist/ packaging (systemd units, logrotate, tmpfiles.d, sysusers.d, install script) and documentation updates. The existing build system and C++ dependencies are validated and locked -- not re-researched.

## Recommended Stack

### Core Technologies (dist/ packaging)

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| systemd unit files | systemd 252+ (Debian Bookworm baseline) | Service management for chromatindb and chromatindb_relay | Standard on all modern Linux. Both binaries are long-running foreground daemons that need auto-restart, graceful shutdown (SIGTERM already handled), and boot ordering. Type=simple because neither binary forks. |
| sysusers.d | systemd 252+ | Declarative system user/group creation | Replaces manual useradd in install scripts. Idempotent -- runs at boot and package install. Already used in Dockerfile (groupadd/useradd) -- sysusers.d is the systemd-native equivalent for bare-metal. |
| tmpfiles.d | systemd 252+ | Declarative directory creation for /var/lib/chromatindb, /run/chromatindb | Ensures data dir and UDS runtime dir exist with correct ownership. /run is tmpfs and cleared on reboot -- tmpfiles.d recreates it. Standard pattern for daemons needing persistent state dirs. |
| logrotate | 3.21+ (Debian Bookworm) | External log rotation as operator option | chromatindb already has built-in spdlog rotation (log_max_size_mb, log_max_files). logrotate config is a convenience for operators who want system-level log management. Use copytruncate because spdlog holds the file descriptor open. |
| bash install script | bash 5.x | Installs binaries + config files to correct system paths | Simple, auditable, no build-system coupling. Operators can read exactly what it does. No RPM/DEB packaging needed pre-release. YAGNI. |

### Supporting Files (no new dependencies)

| File | Purpose | When to Use |
|------|---------|-------------|
| dist/chromatindb.service | systemd unit for database node | Always -- primary service |
| dist/chromatindb-relay.service | systemd unit for relay | When relay is deployed alongside node |
| dist/chromatindb.sysusers | User/group creation via sysusers.d | Always -- runs via systemd-sysusers |
| dist/chromatindb.tmpfiles | Directory creation/permissions via tmpfiles.d | Always -- runs via systemd-tmpfiles |
| dist/chromatindb.logrotate | Optional external log rotation | When operator prefers logrotate over built-in spdlog rotation |
| dist/chromatindb.conf | Default node config (JSON) | Reference config, copied to /etc/chromatindb/ |
| dist/chromatindb-relay.conf | Default relay config (JSON) | Reference config, copied to /etc/chromatindb/ |
| dist/install.sh | Install script | Copies all of the above to system paths |

### Development/Validation Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| systemd-analyze verify | Validate unit file syntax offline | Run during development: `systemd-analyze verify dist/chromatindb.service`. Catches typos and unknown directives. |
| systemd-analyze security | Audit service hardening score | Run after writing unit files. Produces numeric score (0=fully locked, 10=no hardening). Target score below 3. |

## File Specifications

### systemd Unit: chromatindb.service

```ini
[Unit]
Description=chromatindb - Post-quantum secure database node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=chromatindb
Group=chromatindb
ExecStart=/usr/local/bin/chromatindb run --config /etc/chromatindb/chromatindb.conf --data-dir /var/lib/chromatindb
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=5

# Directories (systemd auto-creates with correct ownership)
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
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
ReadWritePaths=/var/lib/chromatindb /run/chromatindb

[Install]
WantedBy=multi-user.target
```

Key decisions:
- **Type=simple** because chromatindb does not fork (main.cpp calls ioc.run() in foreground).
- **ExecReload=/bin/kill -HUP** because SIGHUP reload is already implemented (ACL, expiry interval, compaction interval).
- **StateDirectory/RuntimeDirectory/LogsDirectory** auto-create dirs with correct ownership. This overlaps with tmpfiles.d, but both are provided: systemd dirs work when the service is enabled, tmpfiles.d works on systems where the service is started manually.
- **ProtectSystem=strict + ReadWritePaths** locks the filesystem to read-only except where the daemon writes. Safe because chromatindb only writes to data_dir (libmdbx files, identity keys, logs) and uds_path (runtime socket).
- **MemoryDenyWriteExecute=yes** is safe because neither chromatindb nor its libraries (liboqs, libsodium) use JIT or runtime code generation. liboqs uses static compiled assembly for PQ algorithms.
- **RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6** covers TCP peer connections (AF_INET/AF_INET6) and UDS local access (AF_UNIX). No AF_NETLINK needed.
- **No CapabilityBoundingSet restriction** because binding to port 4200 (>1024) does not require CAP_NET_BIND_SERVICE. The default empty bounding set via User= is sufficient.

### systemd Unit: chromatindb-relay.service

Same hardening profile as chromatindb.service, with these differences:
- **After=chromatindb.service** because the relay connects to the node via UDS.
- **BindsTo=chromatindb.service** so the relay stops when the node stops (the UDS socket disappears when the node shuts down).
- **ExecStart** points to /usr/local/bin/chromatindb_relay run --config /etc/chromatindb/chromatindb-relay.conf
- **No ExecReload** because the relay does not implement SIGHUP (relay_main.cpp only handles SIGINT/SIGTERM).
- **ReadWritePaths** needs /run/chromatindb (for UDS connect) and /var/lib/chromatindb (for relay identity key).

### sysusers.d: chromatindb.conf

```
u chromatindb - "chromatindb daemon" /var/lib/chromatindb /usr/sbin/nologin
```

One line. The `u` type creates both user AND primary group with the same name. The `-` for ID means auto-allocated UID/GID (recommended by freedesktop.org -- no reason to claim a specific numeric ID). Home dir set to state directory. Shell set to /usr/sbin/nologin for security (this user never needs interactive login).

### tmpfiles.d: chromatindb.conf

```
d /var/lib/chromatindb 0750 chromatindb chromatindb - -
d /run/chromatindb 0750 chromatindb chromatindb - -
d /var/log/chromatindb 0750 chromatindb chromatindb - -
```

Mode 0750: owner rwx, group rx, other none. No age-based cleanup (`-` for age field) because these are permanent state directories. The /run/chromatindb entry is critical because /run is tmpfs and cleared on every reboot -- tmpfiles.d recreates it.

Note: when the systemd service is enabled, StateDirectory/RuntimeDirectory/LogsDirectory achieve the same result. The tmpfiles.d config exists as a fallback for systems where the service is started manually or via alternative init.

### logrotate: chromatindb

```
/var/log/chromatindb/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
    create 0640 chromatindb chromatindb
}
```

**copytruncate is critical** because spdlog holds file descriptors open via rotating_file_sink_mt. Without copytruncate, logrotate would rename the file but spdlog would continue writing to the (now unlinked) old inode. copytruncate copies the file then truncates in-place, so spdlog's fd remains valid. There is a tiny window where log lines can be lost during the copy+truncate, but this is acceptable for a belt-and-suspenders backup -- chromatindb's built-in rotation is the primary mechanism.

**missingok** because the operator may choose console-only logging (empty log_file in config), in which case no log files exist and logrotate should not error.

### Default Configs

**chromatindb.conf** (JSON, matching existing Config struct in config.h):
```json
{
    "bind_address": "0.0.0.0:4200",
    "data_dir": "/var/lib/chromatindb",
    "log_level": "info",
    "log_file": "/var/log/chromatindb/chromatindb.log",
    "log_format": "json",
    "uds_path": "/run/chromatindb/chromatindb.sock",
    "sync_interval_seconds": 60
}
```

Uses production-appropriate defaults: JSON log format for structured log ingestion, UDS enabled for relay connectivity, data under /var/lib. The operator adds bootstrap_peers, allowed_keys, and quota settings as needed.

**chromatindb-relay.conf** (JSON, matching RelayConfig struct):
```json
{
    "bind_address": "0.0.0.0",
    "bind_port": 4201,
    "uds_path": "/run/chromatindb/chromatindb.sock",
    "identity_key_path": "/var/lib/chromatindb/relay.key",
    "log_level": "info",
    "log_file": "/var/log/chromatindb/relay.log"
}
```

The relay's uds_path matches the node's uds_path so they connect out of the box. The relay identity key lives alongside the node data because both run as the same system user.

### install.sh Behavior

The install script should:
1. Check for root privileges (exit 1 if not root/sudo)
2. Accept binary paths as arguments: `install.sh <chromatindb_binary> <chromatindb_relay_binary>`
3. Copy binaries to /usr/local/bin/ and strip if not already stripped
4. Copy configs to /etc/chromatindb/ (skip existing files to preserve operator edits, using cp --no-clobber)
5. Copy systemd units to /usr/lib/systemd/system/
6. Copy sysusers.d to /usr/lib/sysusers.d/
7. Copy tmpfiles.d to /usr/lib/tmpfiles.d/
8. Copy logrotate to /etc/logrotate.d/
9. Run systemd-sysusers (creates user/group if not exists)
10. Run systemd-tmpfiles --create (creates directories)
11. Run systemctl daemon-reload
12. Print next-steps instructions (keygen, enable, start)

Key: install to /usr/lib/ (not /etc/) for systemd units, sysusers.d, and tmpfiles.d because /usr/lib/ is the vendor path. Operators customize via /etc/ overrides (systemd drop-in directories like /etc/systemd/system/chromatindb.service.d/override.conf).

## dist/ Directory Layout

```
dist/
  chromatindb.service           # systemd unit for node
  chromatindb-relay.service     # systemd unit for relay
  chromatindb.sysusers          # sysusers.d config (1 line)
  chromatindb.tmpfiles          # tmpfiles.d config (3 lines)
  chromatindb.logrotate         # logrotate config
  chromatindb.conf              # default node config (JSON)
  chromatindb-relay.conf        # default relay config (JSON)
  install.sh                    # install script
```

8 files, flat directory. No subdirectories -- simple and scannable.

## Documentation Stack

No new tools or dependencies for documentation. Both README.md and PROTOCOL.md are hand-written Markdown. This is correct because:

1. The protocol is custom binary -- no auto-generation from FlatBuffers schemas would produce useful human-readable docs
2. PROTOCOL.md already has 828 lines covering transport, handshake, and message types through v1.4.0
3. The v1.5.0 documentation work is content updates (adding dist/ usage, ensuring all 58 message types are documented), not tooling changes
4. Markdown renders natively on GitHub/GitLab with no build step

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| bash install script | CMake install() + GNUInstallDirs | Couples packaging to build system. Requires CMake at install time. Operators deploying prebuilt binaries or Docker-extracted binaries should not need CMake. |
| bash install script | CPack (DEB/RPM generation) | Pre-release product. DEB/RPM adds packaging complexity (control files, spec files, maintainer scripts, package signing). Not worth it until there are actual users requesting distro packages. YAGNI. |
| sysusers.d | useradd in install.sh | sysusers.d is idempotent and declarative. useradd requires checking if user exists, conditional creation, error handling. sysusers.d does all of that in one line. |
| tmpfiles.d | mkdir -p in install.sh | mkdir only works at install time. /run is tmpfs -- cleared on every reboot. tmpfiles.d automatically recreates /run/chromatindb on boot. mkdir cannot solve this without also adding a systemd-tmpfiles integration anyway. |
| copytruncate logrotate | postrotate SIGUSR1 | spdlog does not implement file reopen on SIGUSR1/SIGUSR2. Adding that feature would require code changes. copytruncate works without any daemon modifications. |
| Type=simple | Type=notify (sd_notify) | sd_notify() requires linking libsystemd as a build dependency. The daemon starts in <100ms so startup readiness signaling adds no value. Not worth a new dependency. |
| /usr/local/bin for binaries | /usr/bin | /usr/local/bin is for locally-installed software not managed by the distro package manager. Correct for a shell-script installer. If we ship DEB/RPM packages later, those would use /usr/bin per FHS. |
| /usr/lib/systemd/system/ for units | /etc/systemd/system/ | /etc/systemd/system/ is for operator overrides and locally-created units. Vendor-shipped units belong in /usr/lib/systemd/system/ so operators can override them via drop-in files without modifying the original. |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| CMake install() for dist files | Couples packaging to build. Requires CMake at install time. | Shell script that copies prebuilt binaries. |
| Type=forking in service file | chromatindb does not fork (main.cpp runs ioc.run() in foreground). Incorrect Type causes systemd to mistrack the process, breaking restart and status reporting. | Type=simple. |
| Type=notify | Requires linking libsystemd. Adds a build dependency for negligible benefit (daemon starts in <100ms). | Type=simple with Restart=on-failure and RestartSec=5. |
| logrotate without copytruncate | spdlog holds file descriptors open. Standard rotate (rename+create) leaves spdlog writing to the unlinked old inode. Logs silently disappear. | Always use copytruncate for spdlog-managed logs. |
| ProtectSystem=full | "full" only protects /usr and /boot but leaves /etc and /var writable. Weaker than needed. | ProtectSystem=strict + explicit ReadWritePaths for data/run dirs. |
| Makefile-based install | Another build tool to maintain alongside CMake. | bash script is self-contained, no toolchain dependency. |
| Docker-only deployment | Docker Compose already exists in deploy/. dist/ serves bare-metal/VM operators. | Both coexist: deploy/ for Docker, dist/ for bare-metal. |
| Ansible/Puppet/Chef playbooks | Configuration management is operator's choice. Shipping opinionated CM code locks into one ecosystem. | Provide raw files (service, sysusers, tmpfiles, logrotate) that any CM tool can deploy. |

## Integration with Existing Build System

The dist/ directory is **fully decoupled from CMake**. No changes to CMakeLists.txt required. The workflow is:

1. Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
2. Or use Docker: existing Dockerfile already produces stripped Release binaries
3. Install: `sudo dist/install.sh build/chromatindb build/chromatindb_relay`

The install script takes binary paths as arguments, making it work with both local builds and Docker-extracted binaries (`docker cp`). No CMake install() targets, no GNUInstallDirs, no FetchContent interaction.

The existing Dockerfile creates a chromatindb user/group and /data volume for container deployments. The dist/ packaging provides the equivalent for bare-metal: sysusers.d creates the user, tmpfiles.d creates the dirs, systemd manages the lifecycle. These are parallel deployment paths, not conflicting ones.

## Version Compatibility

| Component | Minimum Version | Ships With | Notes |
|-----------|----------------|------------|-------|
| systemd | 252 | Debian Bookworm | StateDirectory, RuntimeDirectory, LogsDirectory, all hardening directives available since systemd 235+. 252 is our floor because the Dockerfile uses debian:bookworm. |
| logrotate | 3.21 | Debian Bookworm | copytruncate supported since logrotate 3.7 (ancient). No version concerns. |
| bash | 5.0 | Debian Bookworm (5.2) | install.sh uses only POSIX-compatible features plus `set -euo pipefail`. Works on bash 4+ and most POSIX shells. |
| Linux kernel | 5.10+ | Debian Bookworm (6.1) | All seccomp features for systemd sandboxing. MemoryDenyWriteExecute requires kernel seccomp support (present since 3.17). |

## Sources

- [freedesktop.org sysusers.d(5)](https://www.freedesktop.org/software/systemd/man/latest/sysusers.d.html) -- file format specification, type codes u/g/m/r, field definitions (HIGH confidence)
- [freedesktop.org tmpfiles.d(5)](https://www.freedesktop.org/software/systemd/man/latest/tmpfiles.d.html) -- file format specification, type code d/D, column format (HIGH confidence)
- [freedesktop.org systemd.unit(5)](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html) -- unit file reference, section format (HIGH confidence)
- [systemd hardening options gist (ageis)](https://gist.github.com/ageis/f5595e59b1cddb1513d1b425a323db04) -- comprehensive hardening directive reference (MEDIUM confidence, cross-verified with Red Hat docs)
- [Red Hat systemd unit files guide](https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/8/html-single/using_systemd_unit_files_to_customize_and_optimize_your_system/index) -- unit file authoring best practices (HIGH confidence)
- [Red Hat logrotate guide](https://www.redhat.com/en/blog/setting-logrotate) -- logrotate configuration best practices (HIGH confidence)
- [Arch Wiki logrotate](https://wiki.archlinux.org/title/Logrotate) -- copytruncate behavior documentation (HIGH confidence)
- [openSUSE systemd packaging guidelines](https://en.opensuse.org/openSUSE:Systemd_packaging_guidelines) -- vendor (/usr/lib) vs operator (/etc) file paths (MEDIUM confidence)
- [Baeldung tmpfiles configuration](https://www.baeldung.com/linux/systemd-tmpfiles-configure-temporary-files) -- practical tmpfiles.d examples (MEDIUM confidence)
- Existing project files: CMakeLists.txt, Dockerfile, config.h, relay_config.h, main.cpp, relay_main.cpp -- verified current state (HIGH confidence)

---
*Stack research for: chromatindb v1.5.0 distribution packaging and documentation*
*Researched: 2026-03-28*
