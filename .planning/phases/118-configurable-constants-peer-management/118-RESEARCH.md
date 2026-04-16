# Phase 118: Configurable Constants + Peer Management - Research

**Researched:** 2026-04-16
**Domain:** C++20 node configuration, SIGHUP reload, CLI subcommand dispatch
**Confidence:** HIGH

## Summary

Phase 118 makes 5 hardcoded sync/peer constants configurable via config.json and adds peer management subcommands (`add-peer`, `remove-peer`, `list-peers`) to the `chromatindb` node binary. The codebase already has a mature config system (28+ fields, JSON parsing, validation, SIGHUP reload) and the peer info protocol (types 55/56) is fully implemented with UDS trust-gating. The work is additive -- extending existing, well-established patterns.

The primary technical risk is the PID discovery mechanism for SIGHUP signaling from the peer management subcommands. The node currently has no pidfile; the subcommands need a way to find the running node's PID. The cleanest approach is writing a pidfile to `data_dir/chromatindb.pid` on startup.

**Primary recommendation:** Extend Config struct with 5 new fields, add validation ranges, extend SIGHUP reload for 3 reloadable constants, add pidfile on startup, implement 3 subcommands in main.cpp.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Only 5 of 12 hardcoded constants become configurable. The rest stay hardcoded.
- **D-02:** The 5 configurable constants: `blob_transfer_timeout` (default 600s), `sync_timeout` (default 30s), `pex_interval` (default 300s), `strike_threshold` (default 10), `strike_cooldown` (default 300s).
- **D-03:** Constants that stay hardcoded: KEEPALIVE_INTERVAL (30s), KEEPALIVE_TIMEOUT (60s), MAX_HASHES_PER_REQUEST (64), MAX_PEERS_PER_EXCHANGE (8), MAX_DISCOVERED_PER_ROUND (3), MAX_PERSISTED_PEERS (100), MAX_PERSIST_FAILURES (3).
- **D-04:** SIGHUP-reloadable: `blob_transfer_timeout`, `sync_timeout`, `pex_interval`. Only affect new operations, not in-flight.
- **D-05:** Restart required: `strike_threshold`, `strike_cooldown`. Loaded once at startup.
- **D-06:** Invalid config values rejected with clear error messages and range check details.
- **D-07:** Peer commands are subcommands on the `chromatindb` node binary (not `cdb`).
- **D-08:** `add-peer` and `remove-peer` edit `bootstrap_peers` array in config.json, then send SIGHUP to running node (if running). Works offline too.
- **D-09:** `remove-peer` only removes from `bootstrap_peers` in config.json. PEX-discovered peers in `peers.json` managed automatically.
- **D-10:** `add-peer` and `remove-peer` trigger SIGHUP automatically after editing config.
- **D-11:** `chromatindb list-peers` queries running node via UDS (PeerInfoRequest type 55) for connected peer state, reads config.json for bootstrap peers list.
- **D-12:** Merge display: connected peers from PeerInfoResponse + disconnected bootstrap peers from config.json.
- **D-13:** Uses existing PeerInfoResponse fields only -- no wire format changes.
- **D-14:** If node is not running, fall back to showing config.json bootstrap peers only (all marked disconnected).

### Claude's Discretion
- Config field naming convention (snake_case in JSON, matching existing config.json style)
- Exact validation ranges for each constant
- How SIGHUP sender finds the node PID (pidfile or process lookup)
- list-peers output formatting (table, plain text, etc.)

### Deferred Ideas (OUT OF SCOPE)
- Extend PeerInfoResponse with strikes + direction -- not needed now.
- Blob size in ls output -- carried forward from Phase 117.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| CONF-01 | 10 hardcoded sync/peer constants moved to config.json with sensible defaults | Scoped to 5 per D-01/D-02. Config struct extension pattern verified in config.h (28+ existing fields). load_config parsing pattern in config.cpp. |
| CONF-02 | All new config fields SIGHUP-reloadable where safe | 3 of 5 reloadable per D-04. SIGHUP reload handler in peer_manager.cpp:488-671 already reloads 15+ fields -- proven extension point. |
| CONF-03 | Validation with range checks (reject bad values) | validate_config() in config.cpp:239-368 has 15+ existing range checks -- pattern is "accumulate errors, throw all at once". |
| PEER-01 | `chromatindb add-peer <addr>` adds peer to config and triggers SIGHUP | Requires: config.json editing (nlohmann/json read-modify-write), PID discovery (pidfile), SIGHUP send (kill(pid, SIGHUP)). |
| PEER-02 | `chromatindb remove-peer <addr>` removes peer from config and triggers SIGHUP | Same mechanism as PEER-01, just remove from bootstrap_peers array instead of add. |
| PEER-03 | `chromatindb list-peers` shows configured and connected peers | Requires: UDS connection to running node, PeerInfoRequest/Response parsing (type 55/56), merge with config.json bootstrap_peers, fallback when node not running. |
</phase_requirements>

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Config field addition | Config (db/config/) | -- | Config struct owns all field definitions and parsing |
| Config validation | Config (db/config/) | -- | validate_config() is the single validation entry point |
| SIGHUP reload | PeerManager (db/peer/) | SyncOrchestrator, PexManager | PeerManager::reload_config() dispatches to component setters |
| Peer subcommands | Main (db/main.cpp) | Config (for JSON editing) | main.cpp owns subcommand dispatch; peer commands read/write config.json directly |
| PID discovery | Main (db/main.cpp) | -- | Pidfile written at startup, read by subcommands |
| list-peers UDS query | Main (db/main.cpp) | MessageDispatcher (handler) | Subcommand connects via UDS; existing PeerInfoRequest handler serves the response |

## Standard Stack

### Core (already in project)
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| nlohmann/json | FetchContent | Config JSON parsing and editing | Already used for all config operations |
| Catch2 | FetchContent | Unit testing | Already used for 699 tests |
| spdlog | FetchContent | Logging | Already used throughout |
| Standalone Asio | FetchContent | UDS connection (for list-peers) | Already used for all networking |

No new libraries needed. [VERIFIED: codebase inspection]

## Architecture Patterns

### System Flow Diagram

```
Configurable Constants Flow:
  config.json -> load_config() -> Config struct -> PeerManager ctor
                                                     |
                                              [startup: all 5 loaded]
                                                     |
  config.json -> SIGHUP -> reload_config() -> update 3 reloadable fields
                                              (blob_transfer_timeout,
                                               sync_timeout, pex_interval)

Peer Management Flow:
  chromatindb add-peer <addr>
    -> find config.json (--config or default)
    -> JSON read -> modify bootstrap_peers -> JSON write
    -> read pidfile -> kill(pid, SIGHUP) if running
    -> exit 0

  chromatindb list-peers
    -> read config.json for bootstrap_peers
    -> connect UDS -> send PeerInfoRequest -> recv PeerInfoResponse
    -> merge connected peers + disconnected bootstrap peers
    -> print table -> exit 0
    (fallback: if UDS fails, show config bootstrap_peers only)
```

### Recommended Project Structure (changes only)

```
db/
  config/
    config.h           # +5 new fields in Config struct
    config.cpp         # +5 field parsing, +5 known_keys, +5 validations
  peer/
    peer_manager.h     # Remove BLOB_TRANSFER_TIMEOUT, STRIKE_THRESHOLD,
                       #   STRIKE_COOLDOWN_SEC, PEX_INTERVAL_SEC constexprs
                       # (keep as defaults only for backward compat in tests)
    peer_manager.cpp   # Extend reload_config() with 3 reloadable constants
    sync_orchestrator.h # Remove BLOB_TRANSFER_TIMEOUT constexpr, add member
    sync_orchestrator.cpp # Replace local SYNC_TIMEOUT constexpr with member
    connection_manager.h  # Remove STRIKE_THRESHOLD constexpr, add member
    connection_manager.cpp # Use member strike_threshold_ instead of constexpr
    pex_manager.h       # Remove PEX_INTERVAL_SEC constexpr, add member
    pex_manager.cpp     # Use member pex_interval_sec_ instead of constexpr
  main.cpp             # +3 subcommands: add-peer, remove-peer, list-peers
                       # +pidfile write on startup
  tests/
    config/test_config.cpp    # +tests for 5 new fields, validation ranges
    peer/test_peer_manager.cpp # Update constant-value tests
```

### Pattern 1: Config Field Extension
**What:** Add a new configurable field to the node.
**When to use:** Every time a new config option is added.
**Example:**
```cpp
// config.h -- add to Config struct with default matching prior hardcoded value
uint32_t blob_transfer_timeout = 600;  // seconds (raised from 120s)

// config.cpp -- parse in load_config() (after existing fields)
cfg.blob_transfer_timeout = j.value("blob_transfer_timeout", cfg.blob_transfer_timeout);

// config.cpp -- add to known_keys set
"blob_transfer_timeout",

// config.cpp -- add validation in validate_config()
if (cfg.blob_transfer_timeout < 10 || cfg.blob_transfer_timeout > 86400) {
    errors.push_back("blob_transfer_timeout must be 10-86400 seconds (got " +
                      std::to_string(cfg.blob_transfer_timeout) + ")");
}
```
Source: existing pattern in config.h/config.cpp [VERIFIED: codebase inspection]

### Pattern 2: SIGHUP Reload Extension
**What:** Make a config field hot-reloadable without restart.
**When to use:** For constants that only affect new operations (not in-flight state).
**Example:**
```cpp
// In reload_config() -- after loading new_cfg:
sync_.set_blob_transfer_timeout(new_cfg.blob_transfer_timeout);
sync_.set_sync_timeout(new_cfg.sync_timeout);
pex_.set_pex_interval(new_cfg.pex_interval);
spdlog::info("config reload: blob_transfer_timeout={}s sync_timeout={}s pex_interval={}s",
             new_cfg.blob_transfer_timeout, new_cfg.sync_timeout, new_cfg.pex_interval);
```
Source: existing reload pattern in peer_manager.cpp:488-671 [VERIFIED: codebase inspection]

### Pattern 3: Subcommand Dispatch
**What:** Add a new subcommand to the chromatindb binary.
**When to use:** For admin operations that don't require the full daemon.
**Example:**
```cpp
// main.cpp -- in main(), add dispatch:
if (cmd == "add-peer") return cmd_add_peer(argc - 1, argv + 1);
if (cmd == "remove-peer") return cmd_remove_peer(argc - 1, argv + 1);
if (cmd == "list-peers") return cmd_list_peers(argc - 1, argv + 1);

// Subcommand implementation:
int cmd_add_peer(int argc, char* argv[]) {
    // Parse --config and --data-dir
    // Load config.json with nlohmann/json
    // Check for duplicates in bootstrap_peers
    // Add address to array
    // Write back (preserving formatting)
    // Find PID from pidfile, send SIGHUP if running
    return 0;
}
```
Source: existing subcommand pattern (keygen, backup, show-key) in main.cpp [VERIFIED: codebase inspection]

### Pattern 4: Pidfile for SIGHUP Discovery
**What:** Write PID to a file so subcommands can signal the running daemon.
**When to use:** When external processes need to signal the daemon.
**Example:**
```cpp
// In cmd_run(), after pid is known (it's the current process):
auto pidfile = std::filesystem::path(config.data_dir) / "chromatindb.pid";
{
    std::ofstream f(pidfile);
    f << getpid();
}
// Clean up on exit:
std::filesystem::remove(pidfile);

// In cmd_add_peer(), to signal:
auto pidfile = std::filesystem::path(data_dir) / "chromatindb.pid";
if (std::filesystem::exists(pidfile)) {
    std::ifstream f(pidfile);
    pid_t pid;
    if (f >> pid && kill(pid, 0) == 0) {  // Process exists?
        kill(pid, SIGHUP);
        std::cout << "Sent SIGHUP to node (PID " << pid << ")" << std::endl;
    }
}
```
[ASSUMED] -- pidfile is the standard Unix daemon pattern. Alternative is scanning /proc but pidfile is simpler and more portable.

### Pattern 5: Config JSON Read-Modify-Write
**What:** Edit a specific field in config.json without losing other fields.
**When to use:** add-peer and remove-peer need to modify bootstrap_peers.
**Example:**
```cpp
// Read existing config as raw JSON (not through Config struct -- preserves unknowns)
nlohmann::json j;
{
    std::ifstream f(config_path);
    if (f.is_open()) j = nlohmann::json::parse(f);
}

// Modify bootstrap_peers
auto& peers = j["bootstrap_peers"];
if (!peers.is_array()) peers = nlohmann::json::array();
peers.push_back(address);

// Write back with indentation
{
    std::ofstream f(config_path);
    f << j.dump(4) << std::endl;
}
```
[VERIFIED: nlohmann/json supports parse -> modify -> dump round-trip]

### Pattern 6: UDS PeerInfoRequest from Subcommand
**What:** Connect to running node via UDS and query peer info.
**When to use:** list-peers subcommand.
**Example:**
```cpp
// list-peers connects via UDS (no handshake needed -- UDS is pre-trusted)
// The existing PeerInfoRequest handler in message_dispatcher.cpp already
// returns full per-peer entries when connection is UDS (trusted).
// Response format (from code inspection):
//   [4B peer_count_be][4B bootstrap_count_be]
//   Per peer: [2B addr_len_be][addr_bytes][1B is_bootstrap][1B syncing][1B peer_is_full][8B duration_ms_be]
```
Source: message_dispatcher.cpp:1154-1228 [VERIFIED: codebase inspection]

### Anti-Patterns to Avoid
- **Modifying Config struct with mutable references in PeerManager:** The current code holds `const config::Config& config_`. New configurable constants should be stored as member fields in the components that use them (SyncOrchestrator, ConnectionManager, PexManager), loaded from Config at construction, and updated via setter methods during SIGHUP reload. Do NOT make Config mutable.
- **Reloading restart-required constants on SIGHUP:** D-05 says strike_threshold and strike_cooldown require restart. The reload_config() must NOT update these even if the new config has different values. Load once in constructor.
- **Using Config struct for JSON editing:** add-peer/remove-peer must use raw nlohmann::json parse-modify-write, NOT load_config() -> modify -> save. The Config struct drops unknown keys and doesn't preserve formatting.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| JSON read-modify-write | Custom file parser | nlohmann/json parse/dump | Preserves all existing fields, handles edge cases |
| PID discovery | /proc scanning | Pidfile in data_dir | Standard Unix pattern, race-free, cross-platform |
| UDS connection for list-peers | Raw socket code | asio::local::stream_protocol | Already used by UDS acceptor and CLI connection |
| Signal sending | Complex IPC | kill(pid, SIGHUP) | POSIX standard, already the mechanism the node listens on |
| Config validation | Ad-hoc per-field checks | Extend existing validate_config() | Accumulates all errors, single throw, established pattern |

**Key insight:** Every mechanism needed already exists in the codebase. This phase is pure extension of existing patterns -- config parsing, SIGHUP reload, subcommand dispatch, UDS protocol. No new libraries or concepts needed.

## Common Pitfalls

### Pitfall 1: SYNC_TIMEOUT is a local constexpr, not a class member
**What goes wrong:** `SYNC_TIMEOUT` is defined as `constexpr auto SYNC_TIMEOUT = std::chrono::seconds(30)` locally inside both `run_sync_with_peer()` (line 143) and `handle_sync_as_responder()` (line 616) in sync_orchestrator.cpp. It is NOT a class member like `BLOB_TRANSFER_TIMEOUT`.
**Why it happens:** It was added as a simple local constant, never needed to be configurable before.
**How to avoid:** Replace both local constexpr declarations with a member field `sync_timeout_` in SyncOrchestrator, initialized from config, with a setter for SIGHUP reload.
**Warning signs:** Grep for "constexpr auto SYNC_TIMEOUT" will find exactly 2 occurrences that must both be replaced.

### Pitfall 2: STRIKE_COOLDOWN_SEC is declared but never used
**What goes wrong:** `STRIKE_COOLDOWN_SEC` is declared in peer_manager.h (line 81) and tested in test_peer_manager.cpp (line 110), but never referenced in any implementation code. The strike system currently disconnects immediately at threshold -- there is no cooldown/ban timer.
**Why it happens:** The ban cooldown was planned but not implemented. The constant was placed as a placeholder.
**How to avoid:** Making `strike_cooldown` configurable means: (a) store it in Config, (b) store it in ConnectionManager as a member, (c) but it has NO runtime effect unless a ban-cooldown mechanism is also implemented. The CONTEXT.md says "changing mid-strike accumulation could be surprising" -- this suggests the user expects it to exist even if it currently doesn't. Implementing a simple reconnect-rejection-for-N-seconds mechanism on disconnect-by-strikes would be the minimal way to make this useful. Alternatively, just store the value for future use and document that it's not yet enforced. Given D-05 says "restart required" -- this implies the value is loaded once and used. The planner should clarify whether to implement the actual cooldown mechanism or just store the config value.
**Warning signs:** Test assertions `REQUIRE(PeerManager::STRIKE_COOLDOWN_SEC == 300)` will need updating.

### Pitfall 3: Constants duplicated across multiple classes
**What goes wrong:** `BLOB_TRANSFER_TIMEOUT` is defined as `static constexpr` in BOTH `PeerManager` (peer_manager.h:77) and `SyncOrchestrator` (sync_orchestrator.h:96). `STRIKE_THRESHOLD` is in BOTH `PeerManager` (peer_manager.h:80) and `ConnectionManager` (connection_manager.h:80). `PEX_INTERVAL_SEC` is in BOTH `PeerManager` (peer_manager.h:84) and `PexManager` (pex_manager.h:23).
**Why it happens:** Phase 96 extracted components from PeerManager but kept the constexprs in PeerManager as public test API.
**How to avoid:** When making these configurable: (a) the runtime value goes into a member field of the component that USES it (SyncOrchestrator for blob_transfer_timeout/sync_timeout, ConnectionManager for strike_threshold, PexManager for pex_interval), (b) the PeerManager constexprs become just default values or are removed, (c) tests access through the component or config, not PeerManager static constants.

### Pitfall 4: Config JSON write must preserve existing content
**What goes wrong:** add-peer/remove-peer must not lose existing config fields when writing back. If you load through Config struct and serialize back, unknown keys and formatting are lost.
**Why it happens:** Config struct is a deserialization target, not a round-trip container.
**How to avoid:** Use raw nlohmann::json parse -> modify `bootstrap_peers` -> dump(4). Read the JSON object, modify only the bootstrap_peers array, write back.

### Pitfall 5: list-peers UDS connection needs AEAD handshake handling
**What goes wrong:** UDS connections to the node still go through the connection pipeline. The node's UDS acceptor creates a `Connection` with `is_uds() == true`, which skips PQ handshake but still uses AEAD encryption. The list-peers subcommand in main.cpp runs in the same process as the node binary but not as the daemon -- it needs to establish a proper UDS client connection.
**Why it happens:** The node's UDS path uses the lightweight TrustedHello handshake (nonce + pubkey exchange, HKDF key derivation) for UDS connections.
**How to avoid:** The CLI already has a working UDS connection implementation in `cli/src/connection.cpp` that handles TrustedHello + AEAD. The list-peers subcommand should reuse this pattern (or extract common UDS client code). Since the `chromatindb` binary doesn't currently link the CLI code, the subcommand needs its own minimal UDS client -- or the common handshake code could be factored out. The simplest approach: implement a minimal synchronous UDS client in main.cpp that does TrustedHello + AEAD, sends PeerInfoRequest, reads PeerInfoResponse. This is ~100 lines.
**Warning signs:** Testing will need a running node with UDS enabled, or a mock.

### Pitfall 6: Pidfile race conditions and stale PIDs
**What goes wrong:** If the node crashes without cleaning up the pidfile, add-peer could send SIGHUP to a wrong process with the recycled PID.
**Why it happens:** PIDs get recycled by the OS.
**How to avoid:** Before sending SIGHUP: (a) read PID from file, (b) verify the process exists with `kill(pid, 0)`, (c) optionally verify it's a chromatindb process by checking `/proc/<pid>/comm` or `/proc/<pid>/cmdline`. At minimum, check `kill(pid, 0) == 0` before sending. Also write the pidfile with fsync, and delete on clean shutdown.

## Code Examples

### Config struct additions
```cpp
// config.h -- add to Config struct
uint32_t blob_transfer_timeout = 600;  // seconds, default raised from 120s per D-02
uint32_t sync_timeout = 30;            // seconds
uint32_t pex_interval = 300;           // seconds
uint32_t strike_threshold = 10;        // strikes before disconnect
uint32_t strike_cooldown = 300;        // seconds banned after strike disconnect
```
Source: existing field pattern in config.h [VERIFIED: codebase inspection]

### Validation ranges (recommended)
```cpp
// config.cpp -- in validate_config()
if (cfg.blob_transfer_timeout < 10 || cfg.blob_transfer_timeout > 86400) {
    errors.push_back("blob_transfer_timeout must be 10-86400 (got " +
                      std::to_string(cfg.blob_transfer_timeout) + ")");
}
if (cfg.sync_timeout < 5 || cfg.sync_timeout > 3600) {
    errors.push_back("sync_timeout must be 5-3600 (got " +
                      std::to_string(cfg.sync_timeout) + ")");
}
if (cfg.pex_interval < 10 || cfg.pex_interval > 86400) {
    errors.push_back("pex_interval must be 10-86400 (got " +
                      std::to_string(cfg.pex_interval) + ")");
}
if (cfg.strike_threshold < 1 || cfg.strike_threshold > 1000) {
    errors.push_back("strike_threshold must be 1-1000 (got " +
                      std::to_string(cfg.strike_threshold) + ")");
}
if (cfg.strike_cooldown > 86400) {
    errors.push_back("strike_cooldown must be 0-86400 (got " +
                      std::to_string(cfg.strike_cooldown) + ")");
}
```
[ASSUMED] -- ranges chosen for operational sanity. Minimum 10s for blob_transfer_timeout to prevent accidental near-zero. Minimum 5s for sync_timeout because the sync protocol has multi-step exchanges. Minimum 10s for pex_interval to prevent PEX flooding.

### SyncOrchestrator member fields for configurable constants
```cpp
// sync_orchestrator.h -- replace static constexprs with member fields
// Add to private:
std::chrono::seconds blob_transfer_timeout_;
std::chrono::seconds sync_timeout_;

// Constructor: initialize from config
blob_transfer_timeout_(std::chrono::seconds(config.blob_transfer_timeout)),
sync_timeout_(std::chrono::seconds(config.sync_timeout)),

// Public setter for SIGHUP reload:
void set_blob_transfer_timeout(uint32_t seconds) {
    blob_transfer_timeout_ = std::chrono::seconds(seconds);
}
void set_sync_timeout(uint32_t seconds) {
    sync_timeout_ = std::chrono::seconds(seconds);
}
```
Source: existing setter pattern (set_sync_config, set_rate_limit) in sync_orchestrator.h [VERIFIED: codebase inspection]

### PeerInfoResponse parsing for list-peers
```cpp
// Parsing the trusted (detailed) PeerInfoResponse:
// Format: [4B peer_count][4B bootstrap_count] then per-peer:
//   [2B addr_len][addr_bytes][1B is_bootstrap][1B syncing][1B peer_is_full][8B duration_ms]
auto peer_count = load_u32_be(data);
auto bootstrap_count = load_u32_be(data + 4);
size_t off = 8;
for (uint32_t i = 0; i < peer_count && off < data_len; ++i) {
    uint16_t addr_len = (data[off] << 8) | data[off + 1];
    off += 2;
    std::string address(reinterpret_cast<const char*>(data + off), addr_len);
    off += addr_len;
    bool is_bootstrap = data[off++] == 0x01;
    bool syncing = data[off++] == 0x01;
    bool peer_is_full = data[off++] == 0x01;
    uint64_t duration_ms = load_u64_be(data + off);
    off += 8;
    // Use these fields for display
}
```
Source: message_dispatcher.cpp:1189-1214 [VERIFIED: codebase inspection]

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Hardcoded BLOB_TRANSFER_TIMEOUT=120s | Configurable, default=600s | Phase 118 | Operators can tune for large blobs over slow links |
| Hardcoded constants as static constexpr | Config-loaded member fields | Phase 118 | Runtime tunability via SIGHUP |
| Manual config.json editing for peers | `chromatindb add-peer/remove-peer` | Phase 118 | Operator ergonomics |

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | Pidfile in data_dir is the right PID discovery mechanism | Patterns, Pitfalls | Low -- if user prefers /proc scanning or different location, easy to change |
| A2 | Validation ranges (10-86400 for timeouts, 1-1000 for strikes, etc.) | Code Examples | Low -- ranges are operational guardrails, easily adjusted |
| A3 | STRIKE_COOLDOWN_SEC should be stored as config even though no runtime enforcement exists | Pitfalls | Medium -- planner needs to decide: store only vs. implement basic ban timer |
| A4 | list-peers subcommand needs its own minimal UDS client (not linking CLI code) | Pitfalls | Low -- the alternative (factor out common code) is more work but cleaner long-term |

## Open Questions

1. **STRIKE_COOLDOWN_SEC has no runtime implementation**
   - What we know: The constant is declared (300) but the strike system just disconnects. There is no "banned for N seconds" mechanism.
   - What's unclear: Should Phase 118 implement a basic reconnection-rejection cooldown, or just store the config value as a placeholder?
   - Recommendation: Store the config value and add a TODO comment. The reconnect backoff in Server already provides some natural cooldown. Implementing a proper ban timer is a separate feature.

2. **list-peers UDS client complexity**
   - What we know: The `chromatindb` binary currently doesn't have UDS client code. The CLI (`cdb`) has a full UDS client with TrustedHello + AEAD.
   - What's unclear: How much handshake code needs to be duplicated.
   - Recommendation: Implement a minimal synchronous UDS client in main.cpp (~100 lines). The TrustedHello exchange is simple: send [nonce:32][pubkey:2592], recv [nonce:32][pubkey:2592], HKDF derive keys, then send/recv AEAD-encrypted messages. Identity keys are loaded from data_dir.

## Environment Availability

Step 2.6: SKIPPED (no external dependencies identified -- pure code/config changes to existing C++20 project).

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 (FetchContent) |
| Config file | db/CMakeLists.txt (catch_discover_tests) |
| Quick run command | `./build/db/chromatindb_tests "[config]" -c "test name"` |
| Full suite command | `./build/db/chromatindb_tests` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CONF-01 | 5 new config fields parsed with defaults | unit | `./build/db/chromatindb_tests "[config]" -c "blob_transfer_timeout"` | Extend existing: db/tests/config/test_config.cpp |
| CONF-02 | 3 fields reload on SIGHUP | unit | `./build/db/chromatindb_tests "[peer_manager]" -c "reload"` | Extend existing: db/tests/peer/test_peer_manager.cpp |
| CONF-03 | Validation rejects bad values | unit | `./build/db/chromatindb_tests "[config]" -c "validation"` | Extend existing: db/tests/config/test_config.cpp |
| PEER-01 | add-peer modifies config.json | unit | `./build/db/chromatindb_tests "[peer_cmd]"` | New tests needed |
| PEER-02 | remove-peer modifies config.json | unit | `./build/db/chromatindb_tests "[peer_cmd]"` | New tests needed |
| PEER-03 | list-peers shows peers | manual-only | Manual: run against live node | Justification: requires running daemon with UDS |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . -j$(nproc) && ./db/chromatindb_tests "[config]" "[peer_manager]" "[peer_cmd]"`
- **Per wave merge:** `cd build && cmake --build . -j$(nproc) && ./db/chromatindb_tests`
- **Phase gate:** Full suite green (699+ tests) before /gsd-verify-work

### Wave 0 Gaps
- [ ] New test tag `[peer_cmd]` for add-peer/remove-peer unit tests (JSON read-modify-write)
- [ ] Update existing constant-value assertions in test_peer_manager.cpp (STRIKE_THRESHOLD, STRIKE_COOLDOWN_SEC, BLOB_TRANSFER_TIMEOUT values will change from static constexpr to config-loaded)

## Security Domain

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | no | -- |
| V3 Session Management | no | -- |
| V4 Access Control | yes | UDS trust-gating already enforced for PeerInfoRequest; list-peers uses UDS which requires local access |
| V5 Input Validation | yes | validate_config() range checks for all 5 new fields; add-peer validates address format |
| V6 Cryptography | no | No crypto changes |

### Known Threat Patterns

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| Malicious config values (e.g., pex_interval=0) | Denial of Service | Range validation in validate_config() -- reject values outside safe bounds |
| Stale pidfile causing SIGHUP to wrong process | Tampering | Verify PID with kill(pid, 0) before sending signal; check /proc/<pid>/comm |
| Race condition in config JSON write | Tampering | Atomic write (write to temp file, rename) or accept that admin tools run serially |
| Unauthorized list-peers access | Information Disclosure | UDS requires local filesystem access; existing UDS trust-gating enforces this |

## Sources

### Primary (HIGH confidence)
- **db/config/config.h** -- Config struct with 28+ existing fields, all field defaults
- **db/config/config.cpp** -- JSON parsing (load_config), validation (validate_config), argument parsing
- **db/peer/peer_manager.h** -- Static constexpr constants: BLOB_TRANSFER_TIMEOUT, STRIKE_THRESHOLD, STRIKE_COOLDOWN_SEC, PEX_INTERVAL_SEC
- **db/peer/peer_manager.cpp:488-671** -- Full SIGHUP reload handler, 15+ field reload pattern
- **db/peer/sync_orchestrator.h** -- BLOB_TRANSFER_TIMEOUT static constexpr, member fields for runtime config
- **db/peer/sync_orchestrator.cpp:143,616** -- Local constexpr SYNC_TIMEOUT at two call sites
- **db/peer/connection_manager.h** -- STRIKE_THRESHOLD static constexpr (line 80)
- **db/peer/connection_manager.cpp:334-348** -- Strike system implementation
- **db/peer/pex_manager.h** -- PEX_INTERVAL_SEC static constexpr (line 23)
- **db/peer/pex_manager.cpp:155** -- PEX timer using PEX_INTERVAL_SEC
- **db/peer/message_dispatcher.cpp:1154-1228** -- PeerInfoRequest handler with UDS trust-gating
- **db/main.cpp** -- Subcommand dispatch (lines 240-257), cmd_run setup
- **db/tests/config/test_config.cpp** -- 121 existing config tests
- **db/tests/peer/test_peer_manager.cpp** -- 80 existing peer manager tests, constant-value assertions

### Secondary (MEDIUM confidence)
- **cli/src/connection.h/cpp** -- CLI's UDS client implementation (reference for list-peers subcommand)

### Tertiary (LOW confidence)
- None -- all research based on codebase inspection

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new libraries, pure extension of existing code
- Architecture: HIGH -- all patterns directly observed in codebase, no novel mechanisms
- Pitfalls: HIGH -- all pitfalls identified from concrete code inspection (duplicate constexprs, unused STRIKE_COOLDOWN_SEC, local SYNC_TIMEOUT, etc.)

**Research date:** 2026-04-16
**Valid until:** 2026-05-16 (stable -- internal project, no external dependency changes)
