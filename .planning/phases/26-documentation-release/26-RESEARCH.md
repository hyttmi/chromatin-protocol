# Phase 26: Documentation & Release - Research

**Researched:** 2026-03-15
**Domain:** README documentation updates and version bump
**Confidence:** HIGH

## Summary

Phase 26 is a documentation-only phase: update `db/README.md` to document all v0.5.0 features (DARE, trusted peers, TTL flexibility) and bump `db/version.h` to 0.5.0. No new code, no new libraries, no architecture changes.

The existing README (284 lines) has well-established patterns for config options (JSON example + bulleted descriptions), features (bold title + em-dash + description), scenarios (title + prose + JSON + bash), and signals (bulleted list). All documentation changes follow these patterns.

**Primary recommendation:** Single plan with two tasks: (1) README updates covering all v0.5.0 features, (2) version bump + test verification.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
None -- all documentation decisions are at Claude's discretion.

### Claude's Discretion
- DARE documentation placement -- inline vs. dedicated subsection
- Trusted peers documentation -- integrated vs. dedicated scenario
- Crypto table updates -- whether to update HKDF row
- Master key detail level -- operator essentials vs. full explanation
- Config example -- whether to add trusted_peers to main JSON example
- TTL rewording -- how to fix stale "7-day protocol constant" language
- Tombstone TTL -- how to document TTL=0 permanent vs. TTL>0 expiry
- TTL section structure -- dedicated section vs. inline updates
- Performance table -- whether to add trusted handshake benchmark
- Changelog -- whether to create CHANGELOG.md
- SIGHUP section -- must add trusted_peers to reloadable options

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| DOC-05 | README updated to document DARE, trusted peers, configurable TTL, and tombstone expiry | All v0.5.0 features are implemented; README needs sections for each. See documentation audit below. |
</phase_requirements>

## Documentation Audit

### What Needs Updating (existing content)

1. **Architecture paragraph (line 23):** References "TTL (7-day protocol constant)" -- stale. TTL is now writer-controlled per blob. Must reword.

2. **Crypto table HKDF row (line 17):** Says "Key derivation (session keys from KEM output)". Should also mention at-rest key derivation (blob encryption key from master key).

3. **SIGHUP section (line 118):** Lists `allowed_keys`, `rate_limit_bytes_per_sec`, `rate_limit_burst`, `sync_namespaces` -- missing `trusted_peers`.

4. **Deletion paragraph (line 27):** Says "Tombstones are permanent (TTL=0)". This is now incomplete -- tombstones CAN have TTL>0 and expire naturally.

5. **Config JSON example (lines 84-98):** Missing `trusted_peers` option.

6. **Config options list (lines 100-110):** Missing `trusted_peers` description.

### What Needs Adding (new content)

1. **Encryption at Rest feature:** New entry in Features section. Documents: master key auto-generation, file permissions (0600), HKDF-SHA256 derivation, ChaCha20-Poly1305 encryption, transparent decryption.

2. **Trusted Peers feature/scenario:** Either a Features entry, a Scenario, or both. Documents: `trusted_peers` config option, localhost implicit trust, lightweight handshake (no ML-KEM-1024), SIGHUP reload.

3. **TTL Flexibility updates:** Rewording existing prose to reflect writer-controlled TTL. Documents: per-blob TTL in signed data, TTL=0 permanent, TTL>0 expires, tombstone expiry + GC.

### What version.h Needs

- `VERSION_MINOR` from `"4"` to `"5"` in `db/version.h` (line 2)

## Current Code State

### Config (`db/config/config.h`)
```cpp
std::vector<std::string> trusted_peers;  // IP addresses for lightweight handshake
```
No `max_ttl` or `tombstone_ttl` config options exist. TTL is entirely writer-controlled (set in signed blob data). The node accepts any TTL value from the writer.

### Master Key (`db/crypto/master_key.h`)
```cpp
SecureBytes load_or_generate_master_key(const std::filesystem::path& data_dir);
SecureBytes derive_blob_key(const SecureBytes& master_key);
```
Key file: `data_dir/master.key`, 32 random bytes, 0600 permissions. HKDF context: `"chromatindb-dare-v1"`.

### README Patterns (for consistency)

**Feature entry pattern:**
```
**Feature Name** -- One or two sentence description. Details about behavior.
```

**Config option pattern:**
```
- **option_name** -- description with default in parens (default: `value`)
```

**Scenario pattern:**
```
### Scenario Title

Prose explanation of when/why.

\`\`\`json
{ "config": "example" }
\`\`\`

\`\`\`bash
chromatindb run --config config.json
\`\`\`
```

**SIGHUP pattern:**
```
- **SIGHUP** -- Configuration reload. Re-reads the config file and updates `option1`, `option2`, ... without restarting.
```

## Documentation Recommendations

### DARE Documentation
Add "Encryption at Rest" to Features section. Keep it operator-focused: master key is auto-generated on first run, stored at `data_dir/master.key` with 0600 permissions, back it up. No need to explain HKDF derivation details -- operators don't interact with derived keys.

Update the HKDF row in the Crypto Stack table to mention both session keys and at-rest key derivation.

### Trusted Peers Documentation
Add `trusted_peers` to the main config JSON example and options list. Add a new scenario "Trusted Local Peers" showing a multi-node setup on localhost or a trusted LAN. Add "Lightweight Handshake" to Features section.

### TTL Documentation
Reword Architecture paragraph to remove "7-day protocol constant" and say TTL is writer-controlled. Update the Deletion paragraph to note tombstones can have TTL>0 and expire. No dedicated TTL section needed -- the existing prose covers it when corrected.

### SIGHUP Update
Add `trusted_peers` to the list of reloadable options in the SIGHUP signal description.

### Changelog
Skip CHANGELOG.md -- YAGNI for this project. The README covers what operators need.

### Performance Table
Skip trusted handshake benchmark row -- benchmarks focus on crypto operations and data path, not handshake variants.

## Sources

### Primary (HIGH confidence)
- `db/README.md` -- current README content (284 lines)
- `db/version.h` -- current version (0.4.0)
- `db/config/config.h` -- Config struct with all fields
- `db/crypto/master_key.h` -- master key API
- `.planning/phases/23-ttl-flexibility/23-01-SUMMARY.md` -- Phase 23 implementation details
- `.planning/phases/26-documentation-release/26-CONTEXT.md` -- user decisions

## Metadata

**Confidence breakdown:**
- Documentation audit: HIGH -- direct codebase inspection
- Patterns: HIGH -- existing README patterns clearly established
- Recommendations: HIGH -- straightforward documentation updates

**Research date:** 2026-03-15
**Valid until:** 2026-04-15
