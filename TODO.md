# Chromatin Protocol — TODO

## Open Design Questions

- **Reputation specifics:** Metrics, thresholds, slashing criteria. How to
  prevent bootstraps from becoming censorship points?

- **Sync frequency:** How often do responsible nodes sync replication logs?
  Event-driven (on write) vs periodic (every N seconds)?

- **Responsibility transfer protocol:** Detailed handoff when nodes
  join/leave. How to avoid data loss during transitions?

- **DNS bootstrap:** Use DNS records (e.g. `bootstrap.cpunk.io`) instead
  of hardcoded IPs to allow transparent bootstrap node rotation.

## Implementation TODO

- TTL enforcement: 7-day message expiry in tick() — prune expired entries
  from TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS
- REDIRECT seq ordering: query responsible nodes for actual repl_log seq
  (currently returns placeholder seq=0)
- Replication log compaction
- Group messaging (sender fan-out with GEK, max 512 members)
