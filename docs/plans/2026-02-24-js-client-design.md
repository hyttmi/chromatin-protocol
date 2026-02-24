# JavaScript Client Library Design

> Date: 2026-02-24

## Problem

The Chromatin protocol has a C++ node implementation and a Python test client, but no
JavaScript client. A JS client is required for browser-based applications (e.g. a web
messenger UI) and Node.js tooling. Post-quantum crypto must work in both environments
without native binaries or WASM compilation.

## Approach

Use `@noble/post-quantum` — a pure-JS, MIT-licensed, audited implementation of FIPS 203/204/205
(ML-KEM, ML-DSA, SLH-DSA). This eliminates the need to compile liboqs to WASM while
providing correct, spec-compliant ML-DSA-87 signing and ML-KEM-1024 key encapsulation.

Build a TypeScript client library (`chromatin-client`) that wraps the WebSocket protocol
defined in PROTOCOL-SPEC.md section 5.

---

## 1. Package

| Property       | Value                                              |
|----------------|----------------------------------------------------|
| Name           | `chromatin-client`                                 |
| Language       | TypeScript                                         |
| Targets        | Browser (ESM) + Node.js (CJS + ESM)                |
| Build tool     | `tsup`                                             |
| Test runner    | Vitest                                             |
| Crypto         | `@noble/post-quantum`, `@noble/hashes`             |
| Dependencies   | Zero runtime deps beyond the noble crypto packages |

The package lives in a separate repository. Key storage is the caller's
responsibility — the library provides `Crypto.generateKeyPair()` and raw byte
types so callers can persist keys in localStorage, IndexedDB, or the filesystem.

---

## 2. Source Layout

```
src/
  crypto.ts      — generateKeyPair(), fingerprint(), sign(), verify()
  client.ts      — ChromatinClient class
  protocol.ts    — typed request/response builders, message ID counter
  chunked.ts     — chunked upload/download state machines
  types.ts       — all protocol TypeScript types and enums
index.ts         — public re-exports
```

---

## 3. Connection & Auth

`client.connect(url)` runs the full auth flow and resolves only after `OK`:

1. Open WebSocket to `url`.
2. Send `HELLO` with the client's fingerprint.
3. If the node responds with `REDIRECT`, close and reconnect to the given address, then repeat from step 2.
4. On `CHALLENGE`, sign the 32-byte nonce with ML-DSA-87 and send `AUTH` with the
   signature and full public key.
5. On `OK`, resolve the promise. On `ERROR`, reject it.

Reconnection uses exponential backoff and re-runs the full auth flow on each attempt.

```ts
const client = new ChromatinClient(keyPair)
await client.connect('wss://2.chromatin.cpunk.io:4001')
client.on('disconnect', () => { /* handle */ })
```

---

## 4. Full API

### Messaging

```ts
send(to: string, blob: Uint8Array): Promise<void>
list(cursor?: string): Promise<MessageRef[]>
get(msgId: string): Promise<Uint8Array>
```

`send` handles chunking transparently: blobs ≤ 64 KB go inline; larger blobs use
the `SEND_READY` / binary-frame protocol. `get` reassembles chunks transparently.

### Contact Requests

```ts
contactRequest(to: string, blob: Uint8Array): Promise<void>
listContactRequests(cursor?: string): Promise<MessageRef[]>
getContactRequest(msgId: string): Promise<Uint8Array>
```

### Allowlist

```ts
allow(fingerprint: string): Promise<void>
revoke(fingerprint: string): Promise<void>
```

### Names

```ts
registerName(name: string): Promise<void>
lookupName(name: string): Promise<NameRecord | null>
```

### Profile

```ts
setProfile(blob: Uint8Array): Promise<void>
getProfile(fingerprint: string): Promise<Uint8Array | null>
```

### Groups

```ts
createGroup(ownerName: string, members: string[]): Promise<string>
updateGroup(groupId: string, update: GroupUpdate): Promise<void>
deleteGroup(groupId: string): Promise<void>
destroyGroup(groupId: string): Promise<void>
groupInfo(groupId: string): Promise<GroupInfo>
listGroup(groupId: string, cursor?: string): Promise<MessageRef[]>
getGroupMessage(groupId: string, msgId: string): Promise<Uint8Array>
sendGroup(groupId: string, blob: Uint8Array): Promise<void>
```

### Push Events

```ts
client.on('message',         (ref: MessageRef)      => void)
client.on('contact_request', (ref: MessageRef)       => void)
client.on('group_message',   (ref: GroupMessageRef)  => void)
client.on('disconnect',      ()                      => void)
client.on('reconnecting',    ()                      => void)
```

---

## 5. Internals

**Request correlation:** Every outgoing JSON frame carries a monotonically increasing
`id`. The client keeps a `Map<number, { resolve, reject }>` of pending requests.
Incoming frames match on `id` and settle the corresponding promise.

**Chunked upload state machine (`chunked.ts`):**
1. Send `SEND` (or `GROUP_SEND`) with `size` field.
2. Wait for `SEND_READY` with `request_id`.
3. Split blob into 1 MiB frames. Send each as a binary WebSocket frame with the
   4-byte `request_id` prefix and frame type `0x01` (UPLOAD_CHUNK).
4. Send final frame with type `0x02` (UPLOAD_DONE).
5. Await `OK`.

**Chunked download:** On `GET` response with `chunks` > 0, collect binary frames
until all chunks arrive, then concatenate and resolve.

Only one concurrent chunked upload per connection (protocol limit). A second
chunked send queues behind the first.

---

## 6. Testing

| Layer         | Approach                                                        |
|---------------|-----------------------------------------------------------------|
| Unit          | Vitest — crypto helpers, protocol builders, chunked state machine |
| Integration   | Vitest — live nodes, off by default (`CHROMATIN_INTEGRATION=1`) |

Live test nodes: `195.181.202.122:62010`, `195.181.202.122:62011`, `2.chromatin.cpunk.io:4001` (WSS).

---

## 7. Non-Goals (MVP)

- Key storage / wallet — caller's responsibility
- Message encryption / decryption — caller's responsibility (the library transfers raw blobs)
- CLI tool — separate project
- React/Vue hooks — separate package built on top of this one
