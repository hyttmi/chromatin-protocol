# Phase 76: Directory & User Discovery - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-01
**Phase:** 76-directory-user-discovery
**Areas discussed:** UserEntry blob format, Directory model, User lookup strategy, Cache invalidation, Registration flow
**Mode:** --auto (all decisions auto-selected)

---

## UserEntry Blob Format

| Option | Description | Selected |
|--------|-------------|----------|
| Raw binary with length-prefixed strings | Consistent with _codec.py patterns, compact | x |
| JSON | Human-readable but large, inconsistent with SDK patterns | |
| FlatBuffers | Consistent with wire protocol but overkill for SDK-internal format | |

**User's choice:** Raw binary with length-prefixed strings (auto-selected: recommended default)
**Notes:** Node stores opaque blobs -- format is SDK-internal only. Raw binary follows existing _codec.py struct.pack patterns.

---

## Directory Model

| Option | Description | Selected |
|--------|-------------|----------|
| Configuration object wrapping a namespace | Directory = admin's namespace, no marker blob | x |
| Marker blob + namespace | Write a special "directory init" blob | |
| Implicit (just write UserEntry blobs) | No Directory class, just functions | |

**User's choice:** Configuration object wrapping a namespace (auto-selected: recommended default)
**Notes:** Admin's namespace IS the directory. Directory class wraps ChromatinClient + namespace for ergonomic API.

---

## User Lookup Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| In-memory index from cached entries | Build dict by name and pubkey hash on cache populate | x |
| Index blob (secondary index in namespace) | Admin maintains an index blob listing all users | |
| Per-lookup scan | List and decode on every lookup call | |

**User's choice:** In-memory index from cached entries (auto-selected: recommended default)
**Notes:** O(1) lookups after initial cache populate. Directory sizes are small (hundreds, not millions).

---

## Cache Invalidation

| Option | Description | Selected |
|--------|-------------|----------|
| Subscribe + clear on notification | Clear entire cache on any directory change | x |
| Subscribe + incremental update | Fetch only changed blob on notification | |
| TTL-based refresh | Periodic refresh regardless of changes | |

**User's choice:** Subscribe + clear on notification (auto-selected: recommended default)
**Notes:** Simple, correct. Directory changes infrequently. Incremental updates add complexity for negligible benefit.

---

## Registration Flow

| Option | Description | Selected |
|--------|-------------|----------|
| Two-step: admin delegates, user registers | Matches DIR-02 requirement exactly | x |
| One-step: register() creates delegation + entry | Simpler API but requires admin identity at user call site | |
| Pre-delegation: admin creates entries for users | Doesn't support self-registration | |

**User's choice:** Two-step (auto-selected: recommended default)
**Notes:** Admin calls delegate(), user calls register(). Clean separation of admin and user roles.

---

## Claude's Discretion

- Internal cache data structures and index rebuild strategy
- Whether Directory uses async context manager or regular class
- Exact test case breakdown and assertion patterns
- Notification callback mechanism

## Deferred Ideas

None -- discussion stayed within phase scope
