# Message Storage Revision — LIST + GET with Chunked Transfer

> Design doc for revising the message storage/retrieval protocol.
> Replaces the current FETCH model with reference-based LIST + GET.

**Goal:** Replace the monolithic FETCH command with a reference-based LIST + GET model that supports large file transfers (up to 50 MiB) with chunked binary frames, while keeping small messages efficient.

**Architecture:** Two-table storage (inbox index + message blobs), hybrid wire format (JSON text frames for control, binary WebSocket frames for blob data), 1 MiB chunked transfer for large payloads, client-side deduplication.

---

## 1. Motivation

The current FETCH model returns all messages inline as base64-encoded JSON. Problems:

1. **No deduplication** — client re-downloads all messages on every FETCH
2. **Base64 overhead** — 33% size increase for binary blobs
3. **No large file support** — 256 KiB blob limit is too restrictive
4. **Memory pressure** — server must hold entire response in memory

## 2. LIST + GET Model

### LIST — Retrieve message index

Client requests message metadata. Server returns lightweight index entries with small messages inlined.

```json
// Client -> Node
{"type": "LIST", "id": 10}

// Node -> Client
{"type": "LIST_RESULT", "id": 10, "messages": [
  {
    "msg_id": "<hex>",
    "from": "<fingerprint hex>",
    "timestamp": 1708000100,
    "size": 1200,
    "blob": "<base64>"        // inlined: size <= 64 KB
  },
  {
    "msg_id": "<hex>",
    "from": "<fingerprint hex>",
    "timestamp": 1708000200,
    "size": 5242880,
    "blob": null               // too large: client must GET separately
  }
]}
```

**Inline threshold:** 64 KB. Messages at or below this size have their blob inlined in the LIST response. Larger messages have `blob: null` and must be fetched with GET.

**Client-side dedup:** LIST always returns all msg_ids (up to 7-day TTL window). The client tracks known msg_ids locally and skips GET for messages it already has. No server-side cursor or `since` parameter needed.

### GET — Fetch a specific message blob

For large messages (> 64 KB) not inlined in LIST:

```json
// Client -> Node
{"type": "GET", "id": 11, "msg_id": "<hex>"}
```

**Small blob response (<=64 KB, edge case):**
```json
{"type": "GET_RESULT", "id": 11, "msg_id": "<hex>", "blob": "<base64>"}
```

**Large blob response (>64 KB):** Server sends a JSON header followed by binary chunks:

```json
{"type": "GET_RESULT", "id": 11, "msg_id": "<hex>", "size": 5242880, "chunks": 5}
```

Then the server sends `chunks` binary WebSocket frames (see Section 5).

## 3. Storage Model — Two Tables

Replace the current single inboxes table with two tables:

### TABLE_INBOX_INDEX

Lightweight metadata for LIST responses. Composite key for prefix scan.

```
Key:   recipient_fp(32) || msg_id(32)
Value: sender_fp(32) || timestamp(8 BE) || size(4 BE)
```

- 44 bytes per entry (value only)
- Prefix scan by `recipient_fp` returns all messages
- Ordered by msg_id (deterministic, not time-ordered — client sorts by timestamp)

### TABLE_MESSAGE_BLOBS

Raw encrypted blob data, keyed by msg_id alone.

```
Key:   msg_id(32)
Value: raw encrypted blob (up to 50 MiB)
```

- Blob stored once, referenced from index
- DELETE removes both index entry and blob
- 7-day TTL applies to both tables

### Storage on recipient's responsible nodes

Blobs are stored on the R nodes responsible for the recipient's inbox key (`SHA3-256("inbox:" || recipient_fp)`). Same nodes that hold the inbox index. No separate blob storage infrastructure.

## 4. SEND Changes

### Small messages (<=64 KB)

Same as before — inline JSON:

```json
{"type": "SEND", "id": 20, "to": "<fingerprint hex>", "blob": "<base64>"}
```

Response:
```json
{"type": "SEND_ACK", "id": 20, "msg_id": "<hex>"}
```

Server writes to both TABLE_INBOX_INDEX and TABLE_MESSAGE_BLOBS, then replicates via Kademlia STORE.

### Large messages (>64 KB, up to 50 MiB)

Two-step: JSON metadata first, then chunked binary upload.

**Step 1 — Announce:**
```json
{"type": "SEND", "id": 21, "to": "<fingerprint hex>", "size": 5242880}
```

Response (ready for chunks):
```json
{"type": "SEND_READY", "id": 21, "request_id": 42}
```

If size > 50 MiB:
```json
{"type": "ERROR", "id": 21, "code": 413, "reason": "attachment too large"}
```

**Step 2 — Chunked upload:** Client sends binary WebSocket frames (see Section 5).

**Step 3 — Completion:** After all chunks received:
```json
{"type": "SEND_ACK", "id": 21, "msg_id": "<hex>"}
```

### Timeout

If a chunked upload is not completed within **30 seconds**, the server discards the temp file and closes the upload. This prevents orphaned partial uploads from connection drops.

## 5. Binary Frame Format — Chunked Transfer

Binary WebSocket frames are used for transferring large blob data in both directions (upload and download). All binary frames share the same header format:

```
[1 byte: frame_type]
[4 bytes BE: request_id]
[2 bytes BE: chunk_index]    // 0-based
[payload: up to 1 MiB]
```

**7 bytes overhead per chunk.**

### Frame types

| Value | Type           | Direction        | Description                   |
|-------|----------------|------------------|-------------------------------|
| 0x01  | UPLOAD_CHUNK   | Client -> Node   | Chunk of a SEND blob          |
| 0x02  | DOWNLOAD_CHUNK | Node -> Client   | Chunk of a GET response       |

### Chunk size

Fixed **1 MiB** (1,048,576 bytes) per chunk. The last chunk may be smaller.

Number of chunks = `ceil(blob_size / 1_048_576)`.

### Server memory model

- **Upload:** Server writes each chunk directly to a temp file as it arrives. Maximum 1 MiB memory per active upload per connection. On completion, the temp file is moved to TABLE_MESSAGE_BLOBS.
- **Download:** Server reads 1 MiB at a time from TABLE_MESSAGE_BLOBS and sends as binary frames.

### Maximum concurrent uploads

Implementation-defined. Recommended: 1 active chunked upload per connection. Additional SEND requests with `size` > 64KB while an upload is in progress receive:

```json
{"type": "ERROR", "id": 22, "code": 429, "reason": "upload already in progress"}
```

## 6. Push Notifications

### Small messages (<=64 KB)

Inlined in push, same as current NEW_MESSAGE:

```json
{"type": "NEW_MESSAGE", "msg_id": "<hex>", "from": "<fingerprint hex>", "size": 1200, "blob": "<base64>", "timestamp": 1708000100}
```

### Large messages (>64 KB)

Metadata-only push — blob is null, client fetches with GET when ready:

```json
{"type": "NEW_MESSAGE", "msg_id": "<hex>", "from": "<fingerprint hex>", "size": 5242880, "blob": null, "timestamp": 1708000200}
```

Client decides when to download (e.g., on Wi-Fi, user tap, etc.).

## 7. DELETE

Unchanged in semantics, now deletes from both tables:

```json
{"type": "DELETE", "id": 30, "msg_ids": ["<hex>", "<hex>"]}
```

Server removes entries from TABLE_INBOX_INDEX and TABLE_MESSAGE_BLOBS for each msg_id.

## 8. Summary of Protocol Changes

| What changed           | Before                          | After                                |
|------------------------|---------------------------------|--------------------------------------|
| Message retrieval      | FETCH (all inline, base64)      | LIST (metadata + small inline) + GET |
| Wire format            | JSON only                       | JSON + binary WebSocket frames       |
| Max blob size          | 256 KiB                         | 50 MiB                              |
| Inline threshold       | N/A (all inline)                | 64 KB                               |
| Storage tables         | Single inboxes table            | TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS |
| Large file transfer    | Not supported                   | 1 MiB chunked transfer              |
| Push (large messages)  | Full blob inline                | Metadata only, client GETs          |
| Dedup model            | None (re-fetch everything)      | Client-side (track known msg_ids)   |
| Upload timeout         | N/A                             | 30 seconds for incomplete uploads   |

## 9. Error Codes (additions)

| Code | Reason                  | Context                              |
|------|-------------------------|--------------------------------------|
| 413  | attachment too large    | SEND with size > 50 MiB             |
| 429  | upload already in progress | Second chunked upload on same conn |

## 10. Backward Compatibility

This is a breaking change to the WS protocol. FETCH is removed and replaced by LIST + GET. Since we're pre-release (no deployed clients), this is acceptable. The TCP node-to-node protocol is unchanged.
