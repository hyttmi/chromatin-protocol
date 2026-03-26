# Backlog

Ideas and future work captured during development. Numbered 999.x to avoid collision with active phases.

## 999.1 — Mobile SDK

Native mobile SDK (iOS/Android) for connecting to relay via raw TCP + PQ handshake. Same protocol as Python SDK, different language bindings. No WebSockets needed — native apps can do raw TCP.

**Depends on:** v1.2.0 (relay), v1.3.0 (Python SDK as reference)

## 999.2 — Key Discovery via Namespace

Users publish their public key as a well-known blob in their own namespace. Other users fetch the key blob to encrypt data for that recipient. Self-authenticating — the key blob is signed by the namespace owner (SHA3-256(pubkey) = namespace).

Needs convention: a standard blob format/name for "this is my public key" so SDKs know where to look.

**Depends on:** v1.3.0 (SDK)

## 999.3 — Organizational Key Management

Company-level key directory so organizations can manage access at scale. Concerns:
- New employee onboarding: auto-grant access to relevant namespaces without per-person key exchange
- Offboarding: revoke access (re-encrypt or rotate keys)
- Department/team-level group keys
- Admin namespace that holds org-wide key directory
- Possibly: an org root key that delegates to employee keys, with a company namespace holding the directory

This is above individual publish-to-namespace (999.2) — it's the organizational layer on top.

**Depends on:** 999.2 (key discovery)

## 999.4 — Multi-Reader Envelope Encryption

Client-side envelope encryption for shared file access:
1. Writer generates random symmetric key, encrypts blob payload
2. Writer wraps that symmetric key per-recipient using their ML-KEM public key
3. Stores: encrypted blob + wrapped key entries (one per authorized reader)
4. Any authorized reader unwraps their key copy, decrypts the blob

All client-side — database stays dumb, stores encrypted bytes. SDK provides the API.

**Depends on:** 999.2 (key discovery), v1.3.0 (SDK)

## 999.5 — Browser Client (WebSocket Gateway)

If browser-based clients are ever needed, a WebSocket layer on top of the relay. Browsers can't do raw TCP, so this would be a separate gateway that bridges WebSocket to the PQ-encrypted relay protocol. Low priority — native apps and SDKs cover most use cases.

**Depends on:** v1.2.0 (relay)

## 999.6 — Docker-Based Test Infrastructure

Migrate flaky in-process E2E peer tests to Docker containers with isolated networking. Currently, tests that spin up multiple nodes in-process share the host network with hardcoded ports, causing race conditions and SIGSEGV on teardown (port conflicts, timing-sensitive sync assertions). Move these to per-test Docker containers so each test gets isolated port space and deterministic teardown. Goal: all tests always run in Docker, zero flaky failures.

**Current flaky tests:** `[peer][acl]`, `[peer][metrics]`, `[peer][tombstone]`, `[daemon]` (PEX), `[sync][quota]`
**Root cause:** hardcoded ports + shared io_context + timing-dependent sync assertions

**Depends on:** existing Docker infrastructure (v0.6.0 Compose topology)

## 999.7 — Managed Replication / Backup-as-a-Service Mode

Sell storage space so companies can automatically back up their node data to your instances. Company runs relay + primary node + 2-3 backup nodes, and additionally replicates to your hosted nodes as off-site backup.

Bootstrap connection works today, but a paid backup service needs:

1. **Replication role config** — `replication_mode: receiver` that accepts/stores incoming syncs but doesn't initiate outbound sync or become a full mesh participant in the customer's network
2. **PEX scoping** — don't leak peer lists across customer boundaries. Currently PEX shares all known peers, so Customer A would learn Customer B's peer addresses
3. **Connection-level tenant tagging** — associate connections with a customer identity for billing/quota enforcement. StorageStatus + NamespaceStats (Phase 65) provide the usage data, but need a way to attribute it per-customer

What already works:
- `allowed_keys` restricts who can write to your nodes
- Namespace isolation means multi-tenant is cryptographically safe
- DARE means you can't read their data
- StorageStatus/NamespaceStats provide billing-grade usage metrics

**Depends on:** v1.4.0 (query suite for usage metrics), possibly v1.5.0+ as its own milestone
