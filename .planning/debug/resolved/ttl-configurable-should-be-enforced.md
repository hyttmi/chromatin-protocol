---
status: resolved
trigger: "TTL is exposed as user-configurable in config but should be strict/enforced 7-day default"
created: 2026-03-03T00:00:00Z
updated: 2026-03-03T00:00:00Z
---

## Current Focus

hypothesis: TTL is a mutable field in Config struct, loaded from JSON, with no enforcement
test: Read config.h, config.cpp, test_config.cpp
expecting: Find default_ttl as configurable field
next_action: Return diagnosis

## Symptoms

expected: TTL should be a hardcoded/enforced protocol constant (7 days = 604800s), not user-overridable
actual: TTL is exposed as Config.default_ttl, loadable from JSON "default_ttl" key, overridable to any value
errors: N/A (design issue, not runtime error)
reproduction: Set "default_ttl": 86400 in config JSON, value is accepted without complaint
started: Always — this was the original design

## Eliminated

(none needed — root cause is immediately clear from code)

## Evidence

- timestamp: 2026-03-03T00:00:00Z
  checked: src/config/config.h line 16
  found: `uint32_t default_ttl = 604800;` — TTL is a public mutable struct field with a default
  implication: Any code can read and write this, and it's part of the Config struct alongside truly configurable values

- timestamp: 2026-03-03T00:00:00Z
  checked: src/config/config.cpp line 30
  found: `cfg.default_ttl = j.value("default_ttl", cfg.default_ttl);` — JSON config can override TTL
  implication: Users can set any TTL value in their config file, completely bypassing the 7-day policy

- timestamp: 2026-03-03T00:00:00Z
  checked: tests/config/test_config.cpp lines 39, 52, 94, 101
  found: Tests explicitly validate that TTL can be overridden to 86400 and 3600
  implication: Tests encode the expectation that TTL is configurable — they will need updating

- timestamp: 2026-03-03T00:00:00Z
  checked: src/wire/codec.h line 15
  found: `uint32_t ttl = 0;` in BlobData struct — blob-level TTL field
  implication: The per-blob TTL in BlobData is separate from config. The wire format supports TTL per blob, which is correct. The issue is only about the Config exposing default_ttl as user-settable.

## Resolution

root_cause: TTL is treated as a user-configurable setting in the config system (Config struct field + JSON loading + test coverage for overrides) when it should be an enforced protocol constant.
fix: (not applied — diagnosis only)
verification: (not applied)
files_changed: []
