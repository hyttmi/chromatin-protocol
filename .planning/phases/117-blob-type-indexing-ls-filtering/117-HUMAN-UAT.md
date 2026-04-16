---
status: partial
phase: 117-blob-type-indexing-ls-filtering
source: [117-VERIFICATION.md]
started: 2026-04-16T12:00:00Z
updated: 2026-04-16T12:00:00Z
---

## Current Test

[awaiting human testing]

## Tests

### 1. Rebuild CLI binary and verify new flags
expected: `cdb ls --help` shows --raw and --type flags in usage output
result: [pending]

### 2. cdb ls hides infrastructure blobs against live node
expected: Default `cdb ls` against 192.168.1.73 omits PUBK/CDAT/DLGT blobs while showing CENV/DATA blobs
result: [pending]

### 3. cdb ls --raw shows all types with labels
expected: --raw sends include_all flag (0x01) and live node returns 44-byte entries with type fields decoded correctly as labels
result: [pending]

### 4. cdb ls --type PUBK bypasses hide list
expected: Server-side type_filter bytes sent in ListRequest, only PUBK blobs returned
result: [pending]

## Summary

total: 4
passed: 0
issues: 0
pending: 4
skipped: 0
blocked: 0

## Gaps
