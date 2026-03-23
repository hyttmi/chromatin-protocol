# Phase 58: Relay Scaffolding & Identity - Context

**Gathered:** 2026-03-23
**Status:** Ready for planning

<domain>
## Phase Boundary

Create relay/ directory, build system, JSON config, ML-DSA-87 identity keypair. The relay binary compiles, loads config, and has its own identity -- ready for protocol logic in Phase 59. No networking, no message handling, no UDS connections.

</domain>

<decisions>
## Implementation Decisions

### CLI commands
- **D-01:** Mirror the node's subcommand structure: `run`, `keygen`, `version`, `help`
- **D-02:** `keygen` takes `--output <path>` (writes `<path>` + sibling `.pub`) and `--force` (overwrite existing)
- **D-03:** `run` takes `--config <path>` (required)

### Config file
- **D-04:** Config file is required -- relay errors with clear usage message if `--config` not provided or file missing
- **D-05:** JSON config with fields: `bind_address`, `bind_port`, `uds_path`, `identity_key_path`, `log_level`, `log_file`
- **D-06:** Error-accumulating validation (same pattern as node's `validate_config`) -- collect all errors, report once

### Key file path
- **D-07:** `identity_key_path` points to the secret key file directly (e.g., `/etc/chromatindb/relay.key`)
- **D-08:** Public key derived as sibling by replacing `.key` with `.pub` (SSH-style convention)
- **D-09:** On first `run` with no existing key file, generate ML-DSA-87 keypair and save both files; on subsequent runs load and log public key hash
- **D-10:** `keygen` subcommand enables pre-generating keys for deployment automation, separate from `run`

### Claude's Discretion
- Relay config struct location (relay/ vs extending db/config)
- Whether to reuse NodeIdentity or create relay-specific identity handling
- Signal handling wiring (SIGHUP/SIGUSR1) -- can be deferred to Phase 59 or wired now
- Startup log content and ordering
- Config field validation ranges/defaults

</decisions>

<specifics>
## Specific Ideas

- CLI surface should feel identical to the node binary -- operators familiar with `chromatindb run --config` should immediately know how to use `chromatindb_relay run --config`
- Key path follows SSH convention: path to secret key, public key is the `.pub` sibling

</specifics>

<canonical_refs>
## Canonical References

### Build system
- `CMakeLists.txt` -- Root build file; existing pattern for adding binaries (lines 155-179: chromatindb, chromatindb_loadgen, chromatindb_verify all link chromatindb_lib)
- `db/CMakeLists.txt` -- Library target `chromatindb_lib` with public include dirs and linked deps

### Config pattern
- `db/config/config.h` -- Config struct definition (30+ fields with defaults)
- `db/config/config.cpp` -- JSON parsing, key validation, error-accumulating `validate_config()`

### Identity pattern
- `db/identity/identity.h` -- `NodeIdentity` class: `generate()`, `load_from()`, `load_or_generate()`, `save_to()`
- `db/identity/identity.cpp` -- Keypair file I/O: `node.pub` (2592B) + `node.key` (4896B), size validation on load

### Main entry point pattern
- `db/main.cpp` -- Subcommand dispatch (`run`/`keygen`/`version`/`help`), startup sequence, signal setup

### Logging pattern
- `db/logging/logging.h` -- `init()` with level, file, rotation, format; `get_logger()` for named loggers
- `db/logging/logging.cpp` -- Shared sinks (console + optional rotating file), text/JSON format

### Requirements
- `.planning/REQUIREMENTS.md` -- RELAY-05 (identity), RELAY-06 (config), RELAY-07 (directory), RELAY-08 (binary)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `chromatindb_lib` static library: all crypto, identity, config, logging, wire format available to relay
- `db/identity/identity.h`: NodeIdentity handles ML-DSA-87 keygen/load/save -- relay can reuse or adapt
- `db/logging/logging.h`: `init()` + shared sinks pattern directly reusable
- `db/crypto/signing.h`: Raw `Signer` class with `generate_keypair()`, `export_public_key()`, `export_secret_key()`, `import_keypair()`

### Established Patterns
- Binary addition: `add_executable()` + `target_link_libraries(PRIVATE chromatindb_lib)` in root CMakeLists.txt
- Include paths: `#include "db/..."` from project root (db CMake sets `$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>`)
- Config validation: accumulate errors in vector, throw single exception with all messages
- Subcommands: `argv[1]` dispatch in main(), shift argc/argv for subcommand handlers

### Integration Points
- Root CMakeLists.txt: add `chromatindb_relay` target
- Links `chromatindb_lib` for crypto, identity, logging, config utilities

</code_context>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 58-relay-scaffolding*
*Context gathered: 2026-03-23*
