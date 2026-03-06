---
phase: 10-access-control
plan: 01
subsystem: acl
tags: [config, access-control, sha3-256]

requires:
  - phase: 09-source-restructure
    provides: chromatindb:: namespace and /db layout
provides:
  - Config.allowed_keys field (vector of 64-char hex strings)
  - Config.config_path field (for SIGHUP reload)
  - validate_allowed_keys() function
  - AccessControl class with open/closed mode, is_allowed(), reload()
---

# Plan 10-01 Summary: Config + AccessControl Foundation

## What was built

1. **Config extensions** (`db/config/config.h`, `db/config/config.cpp`):
   - `allowed_keys`: vector of hex namespace hashes (64 chars each)
   - `config_path`: stored when `--config` is provided (for SIGHUP reload)
   - `validate_allowed_keys()`: strict hex validation (length + charset)
   - JSON parsing with validation in `load_config()`

2. **AccessControl class** (`db/acl/access_control.h`, `db/acl/access_control.cpp`):
   - Open mode (empty keys): all peers allowed
   - Closed mode (non-empty keys): only listed namespace hashes allowed
   - Implicit self-allow (own namespace always in set)
   - `is_allowed()`: O(log n) lookup via std::set
   - `reload()`: atomic swap with diff tracking (added/removed counts)

## Tests added

- 8 config tests: parsing, validation, config_path storage
- 12 AccessControl tests: open/closed mode, allow/reject, self-allow, reload, SHA3-256 round-trip

## Requirements covered

- ACL-01 (partial): Config field defined and parsed
