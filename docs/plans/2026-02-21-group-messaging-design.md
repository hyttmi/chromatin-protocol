# Group Messaging Design

> 2026-02-21

## Overview

Group messaging for Chromatin using a **shared inbox model** with server-enforced
access control. One copy of each message is stored at the group's DHT key.
Members read from the shared inbox after the node verifies their membership
against the GROUP_META record.

## Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Storage model | Shared inbox | Bandwidth-efficient: sender uploads once regardless of group size |
| Access control | Server-enforced via GROUP_META | Node checks member list on every read/write/delete |
| Role model | Owner/Admin/Member (3 levels) | Owner: full control. Admin: manage members. Member: send/read. |
| Multiple owners | Yes | Any owner can manage the group. Last owner leaving destroys the group. |
| Owner departure | Group destroyed if last owner | No ownerless groups. Simple, no auto-succession complexity. |
| Write access | Any member | All members in GROUP_META can send messages |
| Delete access | Sender + Admin/Owner | Standard moderation model |
| Push notifications | Push to connected members | Real-time via NEW_GROUP_MESSAGE to WS sessions |
| Tables | Dedicated GROUP_INDEX + GROUP_BLOBS | Clean separation from 1:1 inbox tables |
| Hash | SHA3-256 (32 bytes) | Consistent with rest of protocol |
| Max members | 512 | Per existing spec |

## Data Model

### GROUP_META Wire Format (data_type 0x06)

```
[32 bytes: group_id]                    // SHA3-256(owner_fp || timestamp || random)
[32 bytes: owner_fingerprint]           // Original creator (informational after multi-owner)
[4 bytes BE: version]                   // Monotonic, incremented on any change
[2 bytes BE: member_count]              // 1-512
For each member:
  [32 bytes: member_fingerprint]
  [1 byte: role]                        // 0x00=member, 0x01=admin, 0x02=owner
  [1568 bytes: kem_ciphertext]          // ML-KEM-1024 encrypted GEK
[2 bytes BE: signature_length]
[signature_length bytes: ML-DSA-87 signature]
```

Signature covers everything from `group_id` through the last member's
`kem_ciphertext`. Signed by an owner (any member with role=0x02).

### GROUP_MESSAGE Wire Format (data_type 0x05)

```
[32 bytes: group_id]
[32 bytes: sender_fingerprint]          // Message author
[32 bytes: msg_id]                      // Random 32-byte identifier
[8 bytes BE: timestamp]                 // Milliseconds since Unix epoch
[4 bytes BE: gek_version]              // GEK version for decryption
[4 bytes BE: blob_length]
[blob_length bytes: encrypted blob]     // AES-256-GCM encrypted with GEK
```

No `recipient_fingerprint` — shared inbox, not fan-out.

### Storage Tables

| Table | Key | Value |
|---|---|---|
| `TABLE_GROUP_META` | `group_id(32)` | Full GROUP_META binary (signed) |
| `TABLE_GROUP_INDEX` | `group_id(32) \|\| msg_id(32)` | `sender_fp(32) \|\| ts(8) \|\| size(4)` |
| `TABLE_GROUP_BLOBS` | `group_id(32) \|\| msg_id(32)` | Encrypted blob |

### DHT Routing

- All group data (meta + messages) at: `SHA3-256("group:" || group_id)`
- Stored on R closest nodes (same replication factor as 1:1 messages)
- Single REDIRECT handles both meta and message access

## Roles & Access Control

### Role Definitions

| Role | Value | Permissions |
|---|---|---|
| Member | 0x00 | Send messages, read messages, leave group |
| Admin | 0x01 | + Add/remove regular members, sign GROUP_META for member changes |
| Owner | 0x02 | + Add/remove anyone (including admins), change roles, GROUP_DESTROY |

Multiple members can have role=0x02 (owner). At least one owner must exist at
all times (server-enforced).

### Per-Operation Access

| Operation | Who | Validation |
|---|---|---|
| GROUP_CREATE | Anyone authenticated | Signer = authenticated client, valid GROUP_META, version=1 |
| GROUP_UPDATE (owner signs) | Owner | Can change any roles, add/remove any member |
| GROUP_UPDATE (admin signs) | Admin | Can only add/remove members with role=0x00 |
| GROUP_SEND | Any member | Sender fingerprint in GROUP_META member list |
| GROUP_LIST | Any member | Requester fingerprint in GROUP_META member list |
| GROUP_GET | Any member | Requester fingerprint in GROUP_META member list |
| GROUP_DELETE | Sender, Admin, or Owner | msg sender = requester, OR requester role >= 0x01 |
| GROUP_INFO | Any member | Requester fingerprint in GROUP_META member list |
| GROUP_DESTROY | Owner | Requester has role=0x02 in GROUP_META |

### GROUP_UPDATE Validation Rules

1. Signature must be valid (ML-DSA-87 verify)
2. Signer must be owner or admin in the **currently stored** GROUP_META
3. Version must be strictly greater than stored version
4. If signer is admin: changes are restricted to adding/removing role=0x00 members only
5. New GROUP_META must contain at least one owner (role=0x02)
6. If new GROUP_META has zero owners → group is destroyed (wipe meta + all messages)
7. If new GROUP_META has member_count=0 → group is destroyed

## WebSocket Commands

### GROUP_CREATE

```json
-> {"cmd":"GROUP_CREATE","id":1,"group_meta":"<hex-encoded GROUP_META binary>"}
<- {"id":1,"ok":true,"group_id":"<hex>"}
```

### GROUP_UPDATE

```json
-> {"cmd":"GROUP_UPDATE","id":2,"group_meta":"<hex-encoded GROUP_META binary>"}
<- {"id":2,"ok":true}
```

### GROUP_SEND

```json
-> {"cmd":"GROUP_SEND","id":3,"group_id":"<hex>","msg_id":"<hex>","gek_version":1,"blob":"<hex>"}
<- {"id":3,"ok":true}
```

Large blobs (>64KB) use the same SEND_READY + binary chunk protocol as regular messages.

### GROUP_LIST

```json
-> {"cmd":"GROUP_LIST","id":4,"group_id":"<hex>","after":"<msg_id hex, optional>","limit":50}
<- {"id":4,"ok":true,"messages":[
     {"msg_id":"...","sender":"...","ts":...,"size":...,"gek_version":1,"blob":"<hex or null>"}
   ]}
```

Inline blobs for messages <= 64KB, `blob:null` for larger.

### GROUP_GET

```json
-> {"cmd":"GROUP_GET","id":5,"group_id":"<hex>","msg_id":"<hex>"}
<- {"id":5,"ok":true,"blob":"<hex>"}
```

Large blobs use chunked binary download (same as regular GET).

### GROUP_DELETE

```json
-> {"cmd":"GROUP_DELETE","id":6,"group_id":"<hex>","msg_id":"<hex>"}
<- {"id":6,"ok":true}
```

### GROUP_INFO

```json
-> {"cmd":"GROUP_INFO","id":7,"group_id":"<hex>"}
<- {"id":7,"ok":true,"group_meta":"<hex-encoded GROUP_META binary>"}
```

### GROUP_DESTROY

```json
-> {"cmd":"GROUP_DESTROY","id":8,"group_id":"<hex>"}
<- {"id":8,"ok":true}
```

Wipes GROUP_META + all GROUP_INDEX + GROUP_BLOBS for the group.

### Push: NEW_GROUP_MESSAGE

```json
<- {"cmd":"NEW_GROUP_MESSAGE","group_id":"<hex>","msg_id":"<hex>","sender":"<hex>","size":123}
```

Pushed to all connected members of the group.

### Push: GROUP_DESTROYED

```json
<- {"cmd":"GROUP_DESTROYED","group_id":"<hex>"}
```

Pushed when GROUP_DESTROY is executed or last owner leaves.

## GEK (Group Encryption Key)

- 256-bit AES key for AES-256-GCM message encryption
- Owner generates GEK and encrypts it per-member with ML-KEM-1024
- Stored in GROUP_META as 1568-byte `kem_ciphertext` per member
- GEK version tracked in GROUP_META version and each GROUP_MESSAGE
- **Rotation required** when a member is removed (forward secrecy)
- **Rotation optional** when a member is added (new member gets current GEK)
- Messages use `gek_version` to identify which key decrypts them

## Replication & Sync

- GROUP_META and group messages share DHT key `SHA3-256("group:" || group_id)`
- Replication log entries use data_type 0x05 (GROUP_MESSAGE) and 0x06 (GROUP_META)
- Sync works identically to 1:1 messages via existing SYNC protocol
- Same replication factor (R=3 default)

## TTL & Cleanup

- **Group messages**: 7-day TTL (same as regular messages)
- **GROUP_META**: No TTL — persists as long as the group exists
- **Group destruction**: Wipes GROUP_META + all messages immediately
  - Triggered by: GROUP_DESTROY command, member_count → 0, or last owner leaves
- **Replication log**: Group entries follow same compaction rules

## GROUP_META Caching

Responsible nodes cache GROUP_META in memory for fast ACL lookups:
- `std::unordered_map<Hash, GroupMeta>` keyed by group_id
- Populated on first access (lazy load from mdbx)
- Invalidated and reloaded on GROUP_UPDATE or GROUP_CREATE
- Cleared on GROUP_DESTROY

## Edge Cases

- **Large blob group messages**: Same chunked binary frame protocol as regular messages
- **Client on non-responsible node**: REDIRECT to responsible node for the group DHT key
- **Concurrent GROUP_UPDATE**: Version must be strictly greater — lower version rejected,
  client retries with latest version
- **Group owner leaves**: If other owners exist, group continues. If last owner, group destroyed.
- **Member count hits 0**: Group auto-destroyed (wipe everything)
