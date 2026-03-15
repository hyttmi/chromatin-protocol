# Phase 26: Documentation & Release - Context

**Gathered:** 2026-03-15
**Status:** Ready for planning

<domain>
## Phase Boundary

Update db/README.md to document all v0.5.0 features (DARE, trusted peers, TTL flexibility) and bump version.h to 0.5.0. Operators should be able to understand and configure every v0.5.0 feature from the README alone.

</domain>

<decisions>
## Implementation Decisions

### Claude's Discretion

All documentation decisions are at Claude's discretion. The user trusts Claude to make the right placement, depth, and structural choices based on the existing README patterns and operator ergonomics. Specific areas:

- **DARE documentation placement** — Inline in existing sections vs. dedicated subsection. Choose based on README flow and discoverability.
- **Trusted peers documentation** — Integrated across sections vs. dedicated scenario. Follow existing patterns (like "Closed Mode with ACLs" scenario).
- **Crypto table updates** — Whether to update HKDF row to mention at-rest key derivation alongside session keys.
- **Master key detail level** — Operator essentials (auto-generated, file permissions, back it up) vs. full explanation (HKDF derivation, rotation, loss consequences).
- **Config example** — Whether to add `trusted_peers` to the main JSON example or only in a scenario.
- **TTL rewording** — How to fix stale "7-day protocol constant" language. Audit all TTL mentions across Architecture, Features, and Deletion descriptions.
- **Tombstone TTL** — How to document that tombstones can be permanent (TTL=0) or time-limited (TTL>0, garbage collected).
- **TTL section structure** — Whether to add a dedicated "Blob Lifetime" section or update existing prose inline.
- **Performance table** — Whether to add a "Lightweight handshake (trusted)" benchmark row.
- **Changelog** — Whether to create a CHANGELOG.md for v0.5.0.
- **SIGHUP section** — Must be updated to include `trusted_peers` in the list of reloadable config options.

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. User deferred all documentation decisions to Claude.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/README.md` (284 lines): Well-structured with sections: Crypto Stack, Architecture, Building, Usage, Configuration, Signals, Wire Protocol, Scenarios, Features, Performance
- Config reference pattern: JSON example block + bulleted descriptions per option
- Scenario pattern: Title + prose explanation + JSON config + bash commands

### Established Patterns
- Features section: Bold title + em-dash + 1-2 sentence description per feature
- Config options: `**option_name**` + em-dash + description with default in parens
- SIGHUP section: Lists all reloadable options with behavior notes

### Integration Points
- `db/version.h`: VERSION_MAJOR/MINOR/PATCH defines — currently `0.4.0`, needs `0.5.0`
- `db/config/config.h`: `trusted_peers` field exists, no max_ttl or tombstone_ttl
- `db/crypto/master_key.h`: `load_or_generate_master_key()`, `derive_blob_key()` — key file is `master.key` in data_dir
- Architecture paragraph references "TTL (7-day protocol constant)" — stale
- Crypto table HKDF row: "Key derivation (session keys from KEM output)" — incomplete for DARE
- SIGHUP lists: `allowed_keys`, `rate_limit_bytes_per_sec`, `rate_limit_burst`, `sync_namespaces` — missing `trusted_peers`

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 26-documentation-release*
*Context gathered: 2026-03-15*
