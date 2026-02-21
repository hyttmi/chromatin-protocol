# Group Messaging Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement shared-inbox group messaging with server-enforced ACL, 3-level roles (Owner/Admin/Member), and dedicated storage tables.

**Architecture:** Dedicated GROUP_INDEX and GROUP_BLOBS tables store group messages at DHT key `SHA3-256("group:" || group_id)`. GROUP_META stores membership + encrypted GEK. Server validates all read/write/delete against the GROUP_META member list. WsServer gets 8 new command handlers + push notifications.

**Tech Stack:** C++20, libmdbx, uWebSockets, jsoncpp, liboqs (ML-DSA-87, ML-KEM-1024), GoogleTest

**Design doc:** `docs/plans/2026-02-21-group-messaging-design.md`

---

### Task 1: Add GROUP_INDEX and GROUP_BLOBS storage tables

**Files:**
- Modify: `src/storage/storage.h:26` — add two new table constants
- Modify: `src/storage/storage.cpp:25-38` — add tables to creation list
- Test: `tests/test_storage.cpp` (if exists) or verify via later tasks

**Step 1: Add table constants to storage.h**

After the existing `TABLE_GROUP_META` line, add:

```cpp
inline constexpr const char* TABLE_GROUP_INDEX    = "group_index";
inline constexpr const char* TABLE_GROUP_BLOBS    = "group_blobs";
```

**Step 2: Register tables in storage.cpp**

Add `TABLE_GROUP_INDEX` and `TABLE_GROUP_BLOBS` to the `tables` array in the Storage constructor where all tables are created in the write transaction.

**Step 3: Build and verify**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds with no errors.

**Step 4: Commit**

```bash
git add src/storage/storage.h src/storage/storage.cpp
git commit -m "feat: add TABLE_GROUP_INDEX and TABLE_GROUP_BLOBS storage tables"
```

---

### Task 2: Implement GROUP_META and GROUP_MESSAGE validators

**Files:**
- Modify: `src/kademlia/kademlia.h:197-202` — add validator declarations
- Modify: `src/kademlia/kademlia.cpp` — implement validators + wire into store_locally
- Test: `tests/test_kademlia.cpp` — add validation tests

**Step 1: Declare validators in kademlia.h**

After `validate_allowlist_entry`, add:

```cpp
bool validate_group_meta(std::span<const uint8_t> value, const crypto::Hash& key);
bool validate_group_message(std::span<const uint8_t> value);
```

**Step 2: Wire validators into store_locally switch**

In `store_locally()` (kademlia.cpp ~line 924-930), add cases to the validation switch:

```cpp
case 0x05: valid = validate_group_message(value);       break;
case 0x06: valid = validate_group_meta(value, key);     break;
```

**Step 3: Implement validate_group_message**

GROUP_MESSAGE wire format: `group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob`

Minimum size: 112 bytes (no blob). Validate:
- `value.size() >= 112`
- Parse blob_len from bytes [104..108] (big-endian)
- `value.size() == 112 + blob_len`
- `value.size() <= cfg_.max_message_size`

**Step 4: Implement validate_group_meta**

GROUP_META wire format: `group_id(32) || owner_fp(32) || version(4 BE) || member_count(2 BE) || per-member[fp(32) + role(1) + kem_ciphertext(1568)] × member_count || sig_len(2 BE) || signature`

Validate:
- `value.size() >= 70` (minimum: 32+32+4+2 header = 70 without members)
- Parse member_count (big-endian u16), must be 1..512
- Expected data size before signature: `70 + member_count * 1601`
- Parse sig_len at that offset, verify remaining bytes match
- At least one member must have role=0x02 (owner)
- `group_id` in value must match `key` derivation: verify `key == SHA3-256("group:" || group_id)`
- Verify ML-DSA-87 signature: signer must be an owner in the member list

**Step 5: Write tests for validate_group_message**

In `tests/test_kademlia.cpp`, add test `GroupMessageValidation`:
- Build a valid GROUP_MESSAGE binary, STORE via Kademlia, expect success
- Build truncated GROUP_MESSAGE (< 112 bytes), expect rejection
- Build GROUP_MESSAGE with wrong blob_len, expect rejection

**Step 6: Write tests for validate_group_meta**

Add test `GroupMetaValidation`:
- Build a valid GROUP_META with 1 owner member, sign with owner's key, STORE, expect success
- Build GROUP_META with 0 members, expect rejection
- Build GROUP_META with > 512 members, expect rejection
- Build GROUP_META with no owner role, expect rejection
- Build GROUP_META with wrong signature, expect rejection

**Step 7: Run tests**

Run: `cd build && ctest --output-on-failure -R test_kademlia 2>&1 | tail -20`
Expected: All tests pass.

**Step 8: Commit**

```bash
git add src/kademlia/kademlia.h src/kademlia/kademlia.cpp tests/test_kademlia.cpp
git commit -m "feat: implement GROUP_META and GROUP_MESSAGE validation"
```

---

### Task 3: Wire GROUP_MESSAGE and GROUP_META into store_locally dispatch

**Files:**
- Modify: `src/kademlia/kademlia.cpp:938-1025` — add storage dispatch for 0x05 and 0x06

**Step 1: Add GROUP_MESSAGE (0x05) storage dispatch**

In `store_locally()`, after the inbox (0x02) block and before the allowlist (0x04) block, add:

```cpp
} else if (data_type == 0x05) {
    // GROUP_MESSAGE: two-table write (GROUP_INDEX + GROUP_BLOBS)
    // Value: group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob
    std::span<const uint8_t> group_id(value.data(), 32);
    std::span<const uint8_t> sender_fp(value.data() + 32, 32);
    std::span<const uint8_t> msg_id(value.data() + 64, 32);

    uint32_t blob_len = (static_cast<uint32_t>(value[104]) << 24)
                      | (static_cast<uint32_t>(value[105]) << 16)
                      | (static_cast<uint32_t>(value[106]) << 8)
                      | static_cast<uint32_t>(value[107]);

    // INDEX key: group_id(32) || msg_id(32)
    std::vector<uint8_t> idx_key;
    idx_key.reserve(64);
    idx_key.insert(idx_key.end(), group_id.begin(), group_id.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

    // Dedup: reject if group msg_id already exists
    if (storage_.get(storage::TABLE_GROUP_BLOBS, idx_key)) {
        spdlog::debug("store_locally: duplicate group message rejected");
        return false;
    }

    // INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE)
    std::vector<uint8_t> idx_value;
    idx_value.reserve(44);
    idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
    idx_value.insert(idx_value.end(), value.data() + 96, value.data() + 104);  // timestamp
    idx_value.insert(idx_value.end(), value.data() + 104, value.data() + 108); // blob_len

    // BLOB key: group_id(32) || msg_id(32), value: raw blob
    std::vector<uint8_t> blob_value(value.data() + 108, value.data() + 108 + blob_len);

    storage_.batch_put({
        {storage::TABLE_GROUP_INDEX, idx_key, idx_value},
        {storage::TABLE_GROUP_BLOBS, idx_key, blob_value}
    });
} else if (data_type == 0x06) {
    // GROUP_META: single table write
    std::vector<uint8_t> group_id_key(value.data(), value.data() + 32);
    std::vector<uint8_t> value_vec(value.begin(), value.end());
    storage_.put(storage::TABLE_GROUP_META, group_id_key, value_vec);
```

**Step 2: Write a test that STOREs a GROUP_MESSAGE via Kademlia and reads it back**

In `tests/test_kademlia.cpp`:
- Build a valid GROUP_MESSAGE binary
- Call `kademlia.store(group_key, 0x05, group_msg_bytes)`
- Read from `TABLE_GROUP_INDEX` with prefix `group_id` to verify index entry exists
- Read from `TABLE_GROUP_BLOBS` with key `group_id || msg_id` to verify blob exists

**Step 3: Run tests**

Run: `cd build && ctest --output-on-failure -R test_kademlia 2>&1 | tail -20`
Expected: All tests pass.

**Step 4: Commit**

```bash
git add src/kademlia/kademlia.cpp tests/test_kademlia.cpp
git commit -m "feat: wire GROUP_MESSAGE and GROUP_META into store_locally"
```

---

### Task 4: Add GROUP_META cache and helper to WsServer

**Files:**
- Modify: `src/ws/ws_server.h` — add GroupMeta struct, cache map, helper methods
- Modify: `src/ws/ws_server.cpp` — implement cache helpers

**Step 1: Add GroupMeta struct and cache to ws_server.h**

Add a `GroupMeta` struct (parsed GROUP_META for ACL checks):

```cpp
struct GroupMember {
    crypto::Hash fingerprint{};
    uint8_t role = 0x00;  // 0x00=member, 0x01=admin, 0x02=owner
};

struct GroupMeta {
    crypto::Hash group_id{};
    crypto::Hash owner_fingerprint{};
    uint32_t version = 0;
    std::vector<GroupMember> members;
};
```

Add to WsServer private members:

```cpp
// Group metadata cache for ACL checks (uWS thread only)
std::unordered_map<crypto::Hash, GroupMeta, crypto::HashHash> group_meta_cache_;

// Parse raw GROUP_META binary into GroupMeta struct.
// Returns nullopt if the binary is malformed.
std::optional<GroupMeta> parse_group_meta(std::span<const uint8_t> data);

// Get cached GROUP_META, loading from storage if not cached.
// Returns nullptr if group doesn't exist.
const GroupMeta* get_group_meta(const crypto::Hash& group_id);

// Invalidate cached GROUP_META (call after GROUP_UPDATE/CREATE/DESTROY).
void invalidate_group_meta(const crypto::Hash& group_id);

// Check if a fingerprint has a specific minimum role in a group.
// Returns false if group doesn't exist or member not found.
bool check_group_role(const crypto::Hash& group_id, const crypto::Hash& fingerprint, uint8_t min_role = 0x00);
```

Add GROUP command handler declarations:

```cpp
void handle_group_create(ws_t* ws, const Json::Value& msg);
void handle_group_info(ws_t* ws, const Json::Value& msg);
void handle_group_update(ws_t* ws, const Json::Value& msg);
void handle_group_send(ws_t* ws, const Json::Value& msg);
void handle_group_list(ws_t* ws, const Json::Value& msg);
void handle_group_get(ws_t* ws, const Json::Value& msg);
void handle_group_delete(ws_t* ws, const Json::Value& msg);
void handle_group_destroy(ws_t* ws, const Json::Value& msg);
```

**Step 2: Implement parse_group_meta and cache helpers**

`parse_group_meta`: Parse the binary GROUP_META format into the GroupMeta struct. Skip the kem_ciphertext (1568 bytes per member) — only extract fingerprint and role.

`get_group_meta`: Check cache first, if miss, load from `TABLE_GROUP_META` via `storage_.get()`, parse, cache, return pointer.

`invalidate_group_meta`: Erase from `group_meta_cache_`.

`check_group_role`: Call `get_group_meta()`, iterate members, check if fingerprint exists with `role >= min_role`.

**Step 3: Build and verify**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds (handlers not yet registered in dispatch table, so no link errors).

**Step 4: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp
git commit -m "feat: add GROUP_META cache and ACL helpers to WsServer"
```

---

### Task 5: Implement GROUP_CREATE and GROUP_INFO commands

**Files:**
- Modify: `src/ws/ws_server.cpp` — implement handlers, register in dispatch table
- Test: `tests/test_ws_server.cpp` — add GROUP_CREATE and GROUP_INFO tests

**Step 1: Register commands in dispatch table**

Add to the `commands` map in `on_message()`:

```cpp
{"GROUP_CREATE",  {&WsServer::handle_group_create,  true, 3.0}},
{"GROUP_INFO",    {&WsServer::handle_group_info,    true, 1.0}},
```

**Step 2: Implement handle_group_create**

Required fields: `group_meta` (hex-encoded GROUP_META binary).

Flow:
1. Parse `group_meta` hex string to bytes
2. Call `parse_group_meta()` to validate format
3. Verify `version == 1` (must be initial creation)
4. Verify the authenticated client's fingerprint is an owner (role=0x02) in the meta
5. Verify group doesn't already exist in `TABLE_GROUP_META`
6. Compute `group_key = SHA3-256("group:" || group_id)`
7. Post to worker: `kademlia_.store(group_key, 0x06, raw_meta)` and defer response
8. Invalidate cache, respond with `{id, ok:true, group_id: hex}`

**Step 3: Implement handle_group_info**

Required fields: `group_id` (64 hex chars).

Flow:
1. Parse group_id from hex
2. Check membership: `check_group_role(group_id, session->fingerprint)` — must be a member
3. Load raw GROUP_META from `TABLE_GROUP_META` using group_id as key
4. Return `{id, ok:true, group_meta: hex-encoded raw binary}`

**Step 4: Write test GroupCreateAndInfo**

```
1. Generate owner keypair, authenticate
2. Build GROUP_META binary: group_id=random, owner_fp=owner, version=1, 1 member (owner, role=0x02), sign with owner key
3. Send GROUP_CREATE with hex-encoded meta
4. Expect {ok:true, group_id: hex}
5. Send GROUP_INFO with group_id
6. Expect {ok:true, group_meta: hex} — verify returned meta matches sent meta
```

**Step 5: Write test GroupInfoNotMember**

```
1. Create group as owner (same as above)
2. Generate separate non-member keypair, authenticate on new connection
3. Send GROUP_INFO with the group_id
4. Expect error 403 "not a member"
```

**Step 6: Run tests**

Run: `cd build && ctest --output-on-failure -R test_ws_server 2>&1 | tail -20`
Expected: All tests pass.

**Step 7: Commit**

```bash
git add src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: implement GROUP_CREATE and GROUP_INFO WS commands"
```

---

### Task 6: Implement GROUP_SEND and GROUP_LIST commands

**Files:**
- Modify: `src/ws/ws_server.cpp` — implement handlers, register in dispatch
- Test: `tests/test_ws_server.cpp` — add tests

**Step 1: Register commands**

```cpp
{"GROUP_SEND",    {&WsServer::handle_group_send,    true, 2.0}},
{"GROUP_LIST",    {&WsServer::handle_group_list,    true, 1.0}},
```

**Step 2: Implement handle_group_send**

Required fields: `group_id` (hex), `msg_id` (hex), `gek_version` (int), `blob` (hex).

Flow:
1. Parse fields, validate lengths (group_id=64 hex, msg_id=64 hex)
2. Verify sender is a member: `check_group_role(group_id, session->fingerprint)`
3. Build GROUP_MESSAGE binary: `group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob`
4. Compute `group_key = SHA3-256("group:" || group_id)`
5. Post to worker: `kademlia_.store(group_key, 0x05, msg_bytes)` and defer response
6. Respond with `{id, ok:true}`

**Step 3: Implement handle_group_list**

Required fields: `group_id` (hex). Optional: `after` (hex msg_id), `limit` (int, default 50, max 200).

Flow:
1. Parse group_id, verify membership
2. Prefix-scan `TABLE_GROUP_INDEX` with `group_id` as prefix (32 bytes)
3. For each entry: key = `group_id(32) || msg_id(32)`, value = `sender_fp(32) || ts(8) || size(4)`
4. If `after` is set, skip entries until msg_id > after
5. For each message: if size <= 64KB, read blob from `TABLE_GROUP_BLOBS` and include inline (hex), else `blob:null`
6. Include `gek_version` in response — read from blob header or store in index? **Note:** gek_version is in the GROUP_MESSAGE binary but not in the index. For LIST, set `gek_version` by reading from the blob's first 4 bytes (the blob stored in GROUP_BLOBS is the raw encrypted blob, but the gek_version is at offset 100 in the original GROUP_MESSAGE). **Decision:** Add gek_version to GROUP_INDEX value: `sender_fp(32) || ts(8) || size(4) || gek_version(4)` = 48 bytes.

**Important:** Update GROUP_INDEX value format to include gek_version:
- INDEX value: `sender_fp(32) || timestamp(8 BE) || size(4 BE) || gek_version(4 BE)` = 48 bytes
- Update `store_locally()` GROUP_MESSAGE dispatch (Task 3) accordingly.

7. Respond with `{id, ok:true, messages: [...]}`

**Step 4: Write test GroupSendAndList**

```
1. Create group with owner
2. Send GROUP_SEND with group_id, random msg_id, gek_version=1, small blob
3. Expect {ok:true}
4. Send GROUP_LIST with group_id
5. Expect messages array with 1 entry, verify msg_id, sender, size, blob matches
```

**Step 5: Write test GroupSendNotMember**

```
1. Create group as owner
2. Authenticate as non-member
3. Send GROUP_SEND
4. Expect error 403 "not a member"
```

**Step 6: Write test GroupListPagination**

```
1. Create group, send 5 messages with different msg_ids
2. GROUP_LIST with limit=2 → expect 2 messages
3. GROUP_LIST with after=last_msg_id from first page, limit=2 → expect next 2
```

**Step 7: Run tests and commit**

```bash
git add src/ws/ws_server.cpp src/kademlia/kademlia.cpp tests/test_ws_server.cpp
git commit -m "feat: implement GROUP_SEND and GROUP_LIST WS commands"
```

---

### Task 7: Implement GROUP_GET and GROUP_DELETE commands

**Files:**
- Modify: `src/ws/ws_server.cpp` — implement handlers, register in dispatch
- Test: `tests/test_ws_server.cpp` — add tests

**Step 1: Register commands**

```cpp
{"GROUP_GET",     {&WsServer::handle_group_get,     true, 1.0}},
{"GROUP_DELETE",  {&WsServer::handle_group_delete,  true, 1.0}},
```

**Step 2: Implement handle_group_get**

Required fields: `group_id` (hex), `msg_id` (hex).

Flow:
1. Parse fields, verify membership
2. Build key: `group_id(32) || msg_id(32)`
3. Read from `TABLE_GROUP_BLOBS`
4. If not found, return error 404
5. If blob <= 64KB, return `{id, ok:true, blob: hex}`
6. If blob > 64KB, use chunked binary download (same pattern as regular GET)

**Step 3: Implement handle_group_delete**

Required fields: `group_id` (hex), `msg_id` (hex).

Flow:
1. Parse fields, verify membership
2. Build key: `group_id(32) || msg_id(32)`
3. Read GROUP_INDEX entry to get sender_fp
4. Check permission: requester is the sender, OR requester has role >= 0x01 (admin/owner)
5. Delete from both `TABLE_GROUP_INDEX` and `TABLE_GROUP_BLOBS`
6. Respond with `{id, ok:true}`

**Step 4: Write test GroupGetMessage**

```
1. Create group, send message
2. GROUP_GET with group_id + msg_id → expect blob matches
```

**Step 5: Write test GroupDeleteOwnMessage**

```
1. Create group with owner + member (2 members)
2. Authenticate as member, send message
3. GROUP_DELETE own message → expect {ok:true}
4. GROUP_GET → expect 404
```

**Step 6: Write test GroupDeleteByAdmin**

```
1. Create group with owner + admin + member
2. Authenticate as member, send message
3. Authenticate as admin (new connection)
4. GROUP_DELETE member's message → expect {ok:true}
```

**Step 7: Write test GroupDeleteForbidden**

```
1. Create group with owner + member1 + member2
2. member1 sends message
3. member2 tries to GROUP_DELETE member1's message → expect 403
```

**Step 8: Run tests and commit**

```bash
git add src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: implement GROUP_GET and GROUP_DELETE WS commands"
```

---

### Task 8: Implement GROUP_UPDATE command

**Files:**
- Modify: `src/ws/ws_server.cpp` — implement handler, register in dispatch
- Test: `tests/test_ws_server.cpp` — add tests

**Step 1: Register command**

```cpp
{"GROUP_UPDATE",  {&WsServer::handle_group_update,  true, 2.0}},
```

**Step 2: Implement handle_group_update**

Required fields: `group_meta` (hex-encoded GROUP_META binary).

Flow:
1. Parse GROUP_META binary, extract group_id
2. Load current GROUP_META from cache/storage
3. Verify signer is owner or admin in **current** (stored) meta
4. Verify new version > current version
5. If signer is admin: verify changes are limited to adding/removing role=0x00 members only. Specifically, admin cannot: change any member's role, remove admins or owners.
6. Verify new meta has at least one owner (role=0x02)
7. If member_count == 0 or no owners: trigger group destruction (delete all group data)
8. Otherwise: store new GROUP_META, invalidate cache
9. Compute `group_key = SHA3-256("group:" || group_id)`
10. Post to worker: `kademlia_.store(group_key, 0x06, raw_meta)`, defer response

**Step 3: Write test GroupUpdateAddMember**

```
1. Create group with 1 owner
2. GROUP_UPDATE: add a member (role=0x00), version=2, sign with owner key
3. Expect {ok:true}
4. GROUP_INFO → verify 2 members
```

**Step 4: Write test GroupUpdateVersionMustIncrease**

```
1. Create group (version=1)
2. GROUP_UPDATE with version=1 → expect error (version not increasing)
```

**Step 5: Write test GroupUpdateNonMemberRejected**

```
1. Create group
2. Non-member signs a GROUP_UPDATE → expect error 403
```

**Step 6: Write test GroupUpdateAdminCanAddMember**

```
1. Create group with owner + admin
2. Admin signs GROUP_UPDATE adding a new member (role=0x00)
3. Expect {ok:true}
```

**Step 7: Write test GroupUpdateAdminCannotChangeRoles**

```
1. Create group with owner + admin + member
2. Admin signs GROUP_UPDATE promoting member to admin
3. Expect error 403 (admin cannot change roles)
```

**Step 8: Run tests and commit**

```bash
git add src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: implement GROUP_UPDATE WS command with role enforcement"
```

---

### Task 9: Implement GROUP_DESTROY command

**Files:**
- Modify: `src/ws/ws_server.cpp` — implement handler, register in dispatch
- Test: `tests/test_ws_server.cpp` — add tests

**Step 1: Register command**

```cpp
{"GROUP_DESTROY", {&WsServer::handle_group_destroy, true, 3.0}},
```

**Step 2: Implement handle_group_destroy**

Required fields: `group_id` (hex).

Flow:
1. Parse group_id, verify requester is owner (role=0x02)
2. Post to worker:
   a. Delete GROUP_META from `TABLE_GROUP_META`
   b. Scan `TABLE_GROUP_INDEX` with `group_id` prefix, delete all matching entries
   c. Scan `TABLE_GROUP_BLOBS` with `group_id` prefix, delete all matching entries
3. Defer: invalidate cache, push GROUP_DESTROYED to connected members, respond {ok:true}

**Step 3: Write test GroupDestroy**

```
1. Create group with owner + member, send a message
2. GROUP_DESTROY as owner → expect {ok:true}
3. GROUP_INFO → expect error 404 (group gone)
4. GROUP_LIST → expect error 404
```

**Step 4: Write test GroupDestroyNotOwner**

```
1. Create group with owner + admin + member
2. Admin sends GROUP_DESTROY → expect 403 (only owners)
3. Member sends GROUP_DESTROY → expect 403
```

**Step 5: Write test GroupDestroyAutoOnLastOwner**

```
1. Create group with 1 owner + 1 member
2. Owner sends GROUP_UPDATE removing themselves (member_count=1, no owners left)
3. Expect group auto-destruction
4. GROUP_INFO → expect 404
```

**Step 6: Run tests and commit**

```bash
git add src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: implement GROUP_DESTROY with auto-destruction on last owner leave"
```

---

### Task 10: Add push notifications for group messages

**Files:**
- Modify: `src/ws/ws_server.cpp:1525-1605` — extend `on_kademlia_store()`
- Modify: `src/ws/ws_server.h` — add group member tracking (optional optimization)
- Test: `tests/test_ws_server.cpp` — add push notification test

**Step 1: Extend on_kademlia_store for GROUP_MESSAGE (0x05)**

In `on_kademlia_store()`, after the existing `data_type != 0x02 && data_type != 0x03` check, add handling for 0x05:

```cpp
if (data_type == 0x05) {
    // GROUP_MESSAGE push: notify connected group members
    // Value: group_id(32) || sender_fp(32) || msg_id(32) || timestamp(8 BE) || gek_version(4 BE) || blob_len(4 BE) || blob
    if (value_copy.size() < 112) return;
    crypto::Hash group_id{};
    std::copy(value_copy.begin(), value_copy.begin() + 32, group_id.begin());
    auto sender = std::span<const uint8_t>(value_copy.data() + 32, 32);
    auto msg_id = std::span<const uint8_t>(value_copy.data() + 64, 32);
    uint32_t blob_len = /* parse from [104..108] */;

    // Load GROUP_META to find members
    auto* meta = get_group_meta(group_id);
    if (!meta) return;

    Json::Value push;
    push["type"] = "NEW_GROUP_MESSAGE";
    push["group_id"] = to_hex(group_id);
    push["msg_id"] = to_hex(msg_id);
    push["sender"] = to_hex(sender);
    push["size"] = blob_len;

    // Push to all connected group members
    for (const auto& member : meta->members) {
        auto it = authenticated_.find(member.fingerprint);
        if (it == authenticated_.end()) continue;
        for (auto* client_ws : it->second) {
            if (connections_.count(client_ws) > 0) {
                send_json(client_ws, push);
            }
        }
    }
}
```

**Step 2: Write test GroupMessagePush**

```
1. Create group with owner + member
2. Authenticate member on a second WS connection
3. Owner sends GROUP_SEND
4. Member receives NEW_GROUP_MESSAGE push with correct group_id, msg_id, sender, size
```

**Step 3: Run tests and commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: add NEW_GROUP_MESSAGE push notification for group messages"
```

---

### Task 11: Implement chunked GROUP_SEND and GROUP_GET for large blobs

**Files:**
- Modify: `src/ws/ws_server.h` — extend PendingUpload for group context
- Modify: `src/ws/ws_server.cpp` — add chunked upload/download for groups
- Test: `tests/test_ws_server.cpp` — add large blob test

**Step 1: Extend PendingUpload for group context**

Add to the PendingUpload struct:

```cpp
std::optional<crypto::Hash> group_id;      // set for GROUP_SEND uploads
uint32_t gek_version = 0;                  // for GROUP_SEND
```

**Step 2: Update handle_group_send for large blobs**

If blob size > 64KB (or however SEND_READY threshold is defined):
1. Set `pending_upload` with group context (group_id, gek_version)
2. Respond with `{id, ok:true, type:"SEND_READY", request_id: N, expected_size: blob_len}`
3. Client sends binary chunks (same protocol as regular SEND)

**Step 3: Update on_binary to handle group uploads**

When `pending_upload->group_id` is set, build a GROUP_MESSAGE binary instead of a regular inbox message when all chunks are received.

**Step 4: Update handle_group_get for large blobs**

When the blob from `TABLE_GROUP_BLOBS` is > 64KB, use chunked binary download (same pattern as regular GET chunked download).

**Step 5: Write test GroupSendLargeBlob**

```
1. Create group, authenticate
2. GROUP_SEND with blob > 64KB (e.g. 100KB)
3. Receive SEND_READY response
4. Send binary chunks
5. Receive completion response
6. GROUP_GET → verify blob matches
```

**Step 6: Run tests and commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: chunked upload/download for large group messages"
```

---

### Task 12: Update protocol documentation

**Files:**
- Modify: `PROTOCOL-SPEC.md` — update GROUP_MESSAGE/GROUP_META wire formats, add GROUP WS commands
- Modify: `PROTOCOL.md` — update group messaging section for shared inbox model

**Step 1: Update PROTOCOL-SPEC.md**

- Update GROUP_MESSAGE wire format: remove `recipient_fingerprint` (no fan-out)
- Update GROUP_META wire format: add `role` byte per member
- Update GROUP_INDEX value format: add `gek_version`
- Add Section 5.x: Group WebSocket commands (GROUP_CREATE, GROUP_UPDATE, GROUP_SEND, GROUP_LIST, GROUP_GET, GROUP_DELETE, GROUP_INFO, GROUP_DESTROY)
- Add NEW_GROUP_MESSAGE and GROUP_DESTROYED push notification formats
- Update constants table: add GROUP_INDEX, GROUP_BLOBS tables

**Step 2: Update PROTOCOL.md**

- Update Section 10: replace fan-out model description with shared inbox model
- Add role descriptions (Owner, Admin, Member)
- Add GROUP_DESTROY and auto-destruction on empty/ownerless groups
- Update GEK section if needed

**Step 3: Commit**

```bash
git add PROTOCOL-SPEC.md PROTOCOL.md
git commit -m "docs: update protocol docs for shared-inbox group messaging"
```

---

### Task 13: Final review and cleanup

**Step 1: Run full test suite**

Run: `cd build && ctest --output-on-failure 2>&1 | tail -20`
Expected: All tests pass.

**Step 2: Fix any compiler warnings**

Run: `cmake --build build -j$(nproc) 2>&1 | grep -i warning`
Fix any warnings.

**Step 3: Review all changes**

Run: `git diff main --stat` to see all files changed.
Review for consistency, missing error handling, etc.

**Step 4: Final commit if needed**

```bash
git add -A
git commit -m "fix: address review feedback for group messaging"
```
