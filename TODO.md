# Chromatin Protocol — TODO

## Open Design Questions

- **Reputation specifics:** Metrics, thresholds, slashing criteria. How to
  prevent bootstraps from becoming censorship points?

- **DNS bootstrap:** Use DNS records (e.g. `bootstrap.cpunk.io`) instead
  of hardcoded IPs to allow transparent bootstrap node rotation.

## Implementation TODO

- Group messaging (sender fan-out with GEK, max 512 members)
- Push notification gap detection (client reconnection replay)
- Node failover (client reconnect to next responsible node on disconnect)
