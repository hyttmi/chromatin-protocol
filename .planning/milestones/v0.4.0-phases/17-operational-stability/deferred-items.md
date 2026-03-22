# Deferred Items - Phase 17

## Pre-existing Test Failures

### PeerManager storage full signaling (SEGFAULT)
- **Test:** #253 "PeerManager storage full signaling"
- **Status:** SEGFAULT (pre-existing, fails without any Phase 17 changes)
- **Origin:** Phase 16-03 storage capacity signaling
- **Impact:** Does not affect correctness of Phase 17 features
- **Action needed:** Investigate and fix in a separate plan
