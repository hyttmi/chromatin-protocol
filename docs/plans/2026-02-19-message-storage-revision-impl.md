# Message Storage Revision Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the monolithic FETCH command with a reference-based LIST + GET model supporting chunked binary transfers up to 50 MiB.

**Architecture:** Split the single `inboxes` mdbx table into `inbox_index` (lightweight metadata) and `message_blobs` (raw blob data). Replace FETCH with LIST (returns metadata + small blobs inline) and GET (fetches large blobs by msg_id). Add binary WebSocket frame handling for chunked 1 MiB upload/download of large files. SEND is split into small-inline (<=64 KB) and large-chunked (>64 KB) paths.

**Tech Stack:** C++20, uWebSockets, libmdbx, jsoncpp, GoogleTest

**Key files to reference:**
- Design doc: `docs/plans/2026-02-19-message-storage-revision-design.md`
- Protocol spec: `PROTOCOL-SPEC.md` (Section 4)
- Architecture doc: `protocol.md` (Section 8)
- WS server: `src/ws/ws_server.h`, `src/ws/ws_server.cpp`
- Storage: `src/storage/storage.h`, `src/storage/storage.cpp`
- Tests: `tests/test_ws_server.cpp`, `tests/test_ws_client.h`

---

### Task 1: Add new storage tables (TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS)

Add the two new table constants and register them in the storage constructor. Keep the old `TABLE_INBOXES` for now (we'll migrate uses later and remove it).

**Files:**
- Modify: `src/storage/storage.h:17-24`
- Modify: `src/storage/storage.cpp:28-31`
- Test: `tests/test_storage.cpp`

**Step 1: Write a failing test**

Add to `tests/test_storage.cpp`:

```cpp
TEST_F(StorageTest, InboxIndexAndBlobTables) {
    // Verify we can put/get in both new tables
    std::vector<uint8_t> key = {0x01, 0x02, 0x03};
    std::vector<uint8_t> value = {0xAA, 0xBB};

    EXPECT_TRUE(storage_->put(storage::TABLE_INBOX_INDEX, key, value));
    auto got = storage_->get(storage::TABLE_INBOX_INDEX, key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, value);

    EXPECT_TRUE(storage_->put(storage::TABLE_MESSAGE_BLOBS, key, value));
    auto got2 = storage_->get(storage::TABLE_MESSAGE_BLOBS, key);
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(*got2, value);
}
```

**Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -20 && ./tests/chromatin-tests --gtest_filter=StorageTest.InboxIndexAndBlobTables`
Expected: Runtime failure — "Unknown table" exception.

**Step 3: Add table constants and register them**

In `src/storage/storage.h`, after line 22 (`TABLE_REQUESTS`):
```cpp
inline constexpr const char* TABLE_INBOX_INDEX   = "inbox_index";
inline constexpr const char* TABLE_MESSAGE_BLOBS = "message_blobs";
```

In `src/storage/storage.cpp`, add the new tables to the `tables[]` array:
```cpp
const char* tables[] = {
    TABLE_PROFILES, TABLE_NAMES, TABLE_INBOXES, TABLE_REQUESTS,
    TABLE_ALLOWLISTS, TABLE_REPL_LOG, TABLE_NODES, TABLE_REPUTATION,
    TABLE_INBOX_INDEX, TABLE_MESSAGE_BLOBS,
};
```

**Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests --gtest_filter=StorageTest.InboxIndexAndBlobTables`
Expected: PASS

**Step 5: Run all tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass (100 tests).

**Step 6: Commit**

```bash
git add src/storage/storage.h src/storage/storage.cpp tests/test_storage.cpp
git commit -m "feat: add TABLE_INBOX_INDEX and TABLE_MESSAGE_BLOBS storage tables"
```

---

### Task 2: Add binary frame support to TestWsClient

The test client needs to send and receive binary WebSocket frames for chunked uploads/downloads. Add `send_binary()` and `recv_frame()` (which returns opcode + data).

**Files:**
- Modify: `tests/test_ws_client.h`

**Step 1: Add send_binary and recv_frame to TestWsClient**

Add to `TestWsClient` (after `send_text`):

```cpp
bool send_binary(const std::vector<uint8_t>& data) {
    if (fd_ < 0) return false;

    std::vector<uint8_t> frame;
    frame.push_back(0x82);  // FIN + binary opcode

    if (data.size() < 126) {
        frame.push_back(0x80 | static_cast<uint8_t>(data.size()));
    } else if (data.size() <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((data.size() >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(data.size() & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        uint64_t len = data.size();
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }

    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.insert(frame.end(), mask, mask + 4);

    for (size_t i = 0; i < data.size(); ++i) {
        frame.push_back(data[i] ^ mask[i % 4]);
    }

    return write_all(reinterpret_cast<const char*>(frame.data()), frame.size()) >= 0;
}

struct WsFrame {
    uint8_t opcode;  // 0x01 = text, 0x02 = binary
    std::vector<uint8_t> data;
};

std::optional<WsFrame> recv_frame(int timeout_ms = 2000) {
    if (fd_ < 0) return std::nullopt;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t header[2];
    if (recv_all(header, 2) < 0) return std::nullopt;

    uint8_t opcode = header[0] & 0x0F;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv_all(ext, 2) < 0) return std::nullopt;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv_all(ext, 8) < 0) return std::nullopt;
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | ext[i];
    }

    std::vector<uint8_t> buf(payload_len);
    if (payload_len > 0 && recv_all(buf.data(), payload_len) < 0)
        return std::nullopt;

    return WsFrame{opcode, std::move(buf)};
}
```

Also update `recv_text` to use `recv_frame` internally to avoid code duplication:

```cpp
std::optional<std::string> recv_text(int timeout_ms = 2000) {
    auto frame = recv_frame(timeout_ms);
    if (!frame || frame->opcode != 0x01) return std::nullopt;
    return std::string(frame->data.begin(), frame->data.end());
}
```

**Step 2: Run all tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass (existing tests still use `recv_text` which now delegates to `recv_frame`).

**Step 3: Commit**

```bash
git add tests/test_ws_client.h
git commit -m "feat: add binary frame support to TestWsClient"
```

---

### Task 3: Implement LIST command (replace FETCH)

Replace the FETCH handler with LIST. LIST scans TABLE_INBOX_INDEX for the authenticated user's fingerprint, returns metadata + inlined blobs (<=64 KB) or `blob: null` for larger ones.

This task also migrates handle_send to write to the new two-table model instead of the old TABLE_INBOXES.

**Files:**
- Modify: `src/ws/ws_server.h:87` (rename handle_fetch → handle_list)
- Modify: `src/ws/ws_server.cpp:94-475` (dispatch + handler)
- Modify: `tests/test_ws_server.cpp`

**Step 1: Write the failing test for LIST (empty inbox)**

Replace `FetchEmptyInbox` test in `tests/test_ws_server.cpp`:

```cpp
TEST_F(WsServerTest, ListEmptyInbox) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":10})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to LIST";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_RESULT");
    EXPECT_EQ(root["id"].asInt(), 10);
    ASSERT_TRUE(root["messages"].isArray());
    EXPECT_EQ(root["messages"].size(), 0u);

    client.close();
}
```

**Step 2: Write the failing test for LIST with inline message**

Replace `FetchWithMessages` test:

```cpp
TEST_F(WsServerTest, ListWithInlineMessage) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Insert into the NEW tables (TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS)
    crypto::Hash msg_id{};
    msg_id.fill(0xAA);

    crypto::Hash sender_fp{};
    sender_fp.fill(0xBB);

    uint64_t timestamp = 1700000000;
    std::vector<uint8_t> blob = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"

    // TABLE_INBOX_INDEX: key = recipient_fp(32) || msg_id(32)
    //                    value = sender_fp(32) || timestamp(8 BE) || size(4 BE)
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), fingerprint.begin(), fingerprint.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

    std::vector<uint8_t> idx_value;
    idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    uint32_t size = static_cast<uint32_t>(blob.size());
    idx_value.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>(size & 0xFF));

    storage_->put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);

    // TABLE_MESSAGE_BLOBS: key = msg_id(32), value = raw blob
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    // Connect and LIST
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":20})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "no response to LIST";

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_RESULT");
    EXPECT_EQ(root["id"].asInt(), 20);
    ASSERT_TRUE(root["messages"].isArray());
    ASSERT_EQ(root["messages"].size(), 1u);

    auto& entry = root["messages"][0];
    EXPECT_EQ(entry["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(entry["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(entry["timestamp"].asUInt64(), timestamp);
    EXPECT_EQ(entry["size"].asUInt(), blob.size());
    // Small blob (5 bytes < 64 KB) should be inlined
    EXPECT_EQ(entry["blob"].asString(), "SGVsbG8=");  // base64("Hello")

    client.close();
}
```

**Step 3: Write the failing test for LIST with large message (blob=null)**

```cpp
TEST_F(WsServerTest, ListLargeMessageBlobNull) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    crypto::Hash msg_id{};
    msg_id.fill(0xCC);
    crypto::Hash sender_fp{};
    sender_fp.fill(0xDD);
    uint64_t timestamp = 1700000100;

    // Size > 64 KB — blob should NOT be inlined
    uint32_t fake_size = 100000;  // 100 KB

    // TABLE_INBOX_INDEX
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), fingerprint.begin(), fingerprint.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

    std::vector<uint8_t> idx_value;
    idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((fake_size >> 24) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((fake_size >> 16) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((fake_size >> 8) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>(fake_size & 0xFF));

    storage_->put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);

    // Don't even need to store the blob — LIST just reads the index
    // (It would be in TABLE_MESSAGE_BLOBS in production)

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":30})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "LIST_RESULT");
    ASSERT_EQ(root["messages"].size(), 1u);

    auto& entry = root["messages"][0];
    EXPECT_EQ(entry["size"].asUInt(), fake_size);
    EXPECT_TRUE(entry["blob"].isNull()) << "large message blob should be null in LIST";

    client.close();
}
```

**Step 4: Run tests to verify they fail**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -10 && ./tests/chromatin-tests --gtest_filter=WsServerTest.List*`
Expected: FAIL (LIST command not recognized)

**Step 5: Implement LIST handler**

In `src/ws/ws_server.h`, rename `handle_fetch` → `handle_list` on line 87:
```cpp
void handle_list(ws_t* ws, const Json::Value& msg);
```

In `src/ws/ws_server.cpp`, update command dispatch (replace FETCH with LIST):
```cpp
} else if (type == "LIST") {
    if (!require_auth(ws, id)) return;
    handle_list(ws, root);
}
```

Replace `handle_fetch` with `handle_list`:
```cpp
void WsServer::handle_list(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    static constexpr size_t INLINE_THRESHOLD = 64 * 1024;  // 64 KB

    // INDEX key layout: recipient_fp(32) || msg_id(32) = 64 bytes
    // INDEX value layout: sender_fp(32) || timestamp(8 BE) || size(4 BE) = 44 bytes

    Json::Value messages(Json::arrayValue);

    storage_.scan(storage::TABLE_INBOX_INDEX, session->fingerprint,
                  [&](std::span<const uint8_t> key,
                      std::span<const uint8_t> value) -> bool {
        if (key.size() != 64 || value.size() != 44) return true;  // skip malformed

        auto msg_id_span = key.subspan(32, 32);
        auto sender_fp_span = value.subspan(0, 32);

        uint64_t ts = 0;
        for (int i = 0; i < 8; ++i) ts = (ts << 8) | value[32 + i];

        uint32_t size = (static_cast<uint32_t>(value[40]) << 24) |
                        (static_cast<uint32_t>(value[41]) << 16) |
                        (static_cast<uint32_t>(value[42]) << 8) |
                        static_cast<uint32_t>(value[43]);

        Json::Value entry;
        entry["msg_id"] = to_hex(msg_id_span);
        entry["from"] = to_hex(sender_fp_span);
        entry["timestamp"] = Json::UInt64(ts);
        entry["size"] = size;

        if (size <= INLINE_THRESHOLD) {
            // Fetch blob and inline it
            std::vector<uint8_t> blob_key(msg_id_span.begin(), msg_id_span.end());
            auto blob = storage_.get(storage::TABLE_MESSAGE_BLOBS, blob_key);
            if (blob) {
                entry["blob"] = to_base64(*blob);
            } else {
                entry["blob"] = Json::nullValue;
            }
        } else {
            entry["blob"] = Json::nullValue;
        }

        messages.append(entry);
        return true;
    });

    Json::Value resp;
    resp["type"] = "LIST_RESULT";
    resp["id"] = id;
    resp["messages"] = messages;
    send_json(ws, resp);
}
```

Also update `handle_auth` pending message count to scan TABLE_INBOX_INDEX instead of TABLE_INBOXES:
```cpp
storage_.scan(storage::TABLE_INBOX_INDEX, session->fingerprint, ...);
```

**Step 6: Run tests to verify they pass**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -10 && ./tests/chromatin-tests --gtest_filter=WsServerTest.List*`
Expected: All 3 LIST tests PASS

**Step 7: Update SendAndFetch test to use LIST**

Rename to `SendAndList`. After SEND, the recipient connects and sends LIST instead of FETCH. Verify the response has `type: LIST_RESULT` with the expected message entry. Also update handle_send to write to the new tables (TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS) instead of TABLE_INBOXES.

In `handle_send`, change the storage writes:

```cpp
// Build INDEX key: recipient_fp(32) || msg_id(32) = 64 bytes
std::vector<uint8_t> idx_key;
idx_key.reserve(64);
idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

// Build INDEX value: sender_fp(32) || timestamp(8 BE) || size(4 BE) = 44 bytes
std::vector<uint8_t> idx_value;
idx_value.reserve(44);
idx_value.insert(idx_value.end(), session->fingerprint.begin(), session->fingerprint.end());
for (int i = 7; i >= 0; --i)
    idx_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
uint32_t blob_size = static_cast<uint32_t>(blob->size());
idx_value.push_back(static_cast<uint8_t>((blob_size >> 24) & 0xFF));
idx_value.push_back(static_cast<uint8_t>((blob_size >> 16) & 0xFF));
idx_value.push_back(static_cast<uint8_t>((blob_size >> 8) & 0xFF));
idx_value.push_back(static_cast<uint8_t>(blob_size & 0xFF));

// BLOB key: msg_id(32)
std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
```

In the worker:
```cpp
bool ok = storage_.put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);
if (ok) {
    storage_.put(storage::TABLE_MESSAGE_BLOBS, blob_key, *blob);
}
```

Still call `kad_.store(inbox_key, 0x02, message_binary)` for Kademlia replication (the binary format on-wire stays the same for now — the old format carries the full blob for node-to-node replication).

**Step 8: Run all tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass. The old FETCH test should be removed/replaced. The `CommandBeforeAuthFails` test sends `FETCH` which now returns error 400 "unknown command" instead of 401 — update to use LIST:
```cpp
ASSERT_TRUE(client.send_text(R"({"type":"LIST","id":7})"));
```

**Step 9: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: replace FETCH with LIST command, migrate to two-table storage"
```

---

### Task 4: Implement GET command (small blob, JSON response)

GET fetches a single message blob by msg_id. For small blobs (<=64 KB), it returns inline JSON. Large blob chunked download comes in Task 7.

**Files:**
- Modify: `src/ws/ws_server.h` (add handle_get declaration)
- Modify: `src/ws/ws_server.cpp` (dispatch + handler)
- Modify: `tests/test_ws_server.cpp`

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, GetSmallBlob) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    crypto::Hash msg_id{};
    msg_id.fill(0xEE);
    std::vector<uint8_t> blob = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"

    // Store blob in TABLE_MESSAGE_BLOBS
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    Json::Value get_msg;
    get_msg["type"] = "GET";
    get_msg["id"] = 40;
    get_msg["msg_id"] = to_hex(msg_id);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, get_msg)));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "GET_RESULT");
    EXPECT_EQ(root["id"].asInt(), 40);
    EXPECT_EQ(root["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(root["blob"].asString(), "SGVsbG8=");  // base64("Hello")

    client.close();
}

TEST_F(WsServerTest, GetNotFound) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // GET a msg_id that doesn't exist
    crypto::Hash fake_id{};
    fake_id.fill(0xFF);

    Json::Value get_msg;
    get_msg["type"] = "GET";
    get_msg["id"] = 41;
    get_msg["msg_id"] = to_hex(fake_id);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, get_msg)));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 404);

    client.close();
}
```

**Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -10 && ./tests/chromatin-tests --gtest_filter=WsServerTest.Get*`
Expected: FAIL (GET command not recognized)

**Step 3: Implement GET handler**

In `src/ws/ws_server.h`, add:
```cpp
void handle_get(ws_t* ws, const Json::Value& msg);
```

In `src/ws/ws_server.cpp`, add dispatch:
```cpp
} else if (type == "GET") {
    if (!require_auth(ws, id)) return;
    handle_get(ws, root);
}
```

Implement handler:
```cpp
void WsServer::handle_get(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();

    std::string msg_id_hex = msg.get("msg_id", "").asString();
    if (msg_id_hex.size() != 64) {
        send_error(ws, id, 400, "msg_id must be 64 hex chars");
        return;
    }
    auto msg_id_bytes = from_hex(msg_id_hex);
    if (!msg_id_bytes) {
        send_error(ws, id, 400, "invalid hex in msg_id");
        return;
    }

    auto blob = storage_.get(storage::TABLE_MESSAGE_BLOBS, *msg_id_bytes);
    if (!blob) {
        send_error(ws, id, 404, "message not found");
        return;
    }

    static constexpr size_t INLINE_THRESHOLD = 64 * 1024;

    if (blob->size() <= INLINE_THRESHOLD) {
        Json::Value resp;
        resp["type"] = "GET_RESULT";
        resp["id"] = id;
        resp["msg_id"] = msg_id_hex;
        resp["blob"] = to_base64(*blob);
        send_json(ws, resp);
    } else {
        // Chunked download — implemented in Task 7
        // For now, return error
        send_error(ws, id, 500, "chunked download not yet implemented");
    }
}
```

**Step 4: Run tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests --gtest_filter=WsServerTest.Get*`
Expected: PASS

**Step 5: Run all tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 6: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: add GET command for fetching message blobs by msg_id"
```

---

### Task 5: Handle binary WebSocket frames in WsServer

Add binary frame dispatch to the uWS message handler. Binary frames will carry chunked upload/download data. Parse the 7-byte header: `[1B frame_type][4B request_id][2B chunk_index]`.

Also add per-session state to track active chunked uploads.

**Files:**
- Modify: `src/ws/ws_server.h` (Session struct, new members)
- Modify: `src/ws/ws_server.cpp` (message handler, binary dispatch)
- Test: `tests/test_ws_server.cpp`

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, BinaryFrameWithoutUploadReturnsError) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Send a binary frame with no active upload — should get error
    std::vector<uint8_t> binary_frame = {
        0x01,                    // frame_type = UPLOAD_CHUNK
        0x00, 0x00, 0x00, 0x01, // request_id = 1
        0x00, 0x00,              // chunk_index = 0
        0xDE, 0xAD              // payload
    };
    ASSERT_TRUE(client.send_binary(binary_frame));

    auto resp = client.recv_text(3000);
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 400);

    client.close();
}
```

**Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -10 && ./tests/chromatin-tests --gtest_filter=WsServerTest.BinaryFrameWithoutUploadReturnsError`
Expected: FAIL (binary frames are currently silently ignored — `if (opCode != uWS::OpCode::TEXT) return;`)

**Step 3: Implement binary frame handling**

In `src/ws/ws_server.h`, add to Session struct:
```cpp
struct PendingUpload {
    uint32_t request_id = 0;
    crypto::Hash recipient_fp{};
    int id = 0;                // JSON id for the SEND that started this
    uint32_t expected_size = 0;
    uint32_t received = 0;
    uint16_t next_chunk = 0;
    std::vector<uint8_t> data; // accumulated blob data
    std::chrono::steady_clock::time_point started;
};
std::optional<PendingUpload> pending_upload;
```

Add to WsServer private section:
```cpp
void on_binary(ws_t* ws, std::span<const uint8_t> data);
std::atomic<uint32_t> next_request_id_{1};
```

In `src/ws/ws_server.cpp`, update the `.message` handler:
```cpp
.message = [this](ws_t* ws, std::string_view message, uWS::OpCode opCode) {
    if (opCode == uWS::OpCode::TEXT) {
        on_message(ws, message);
    } else if (opCode == uWS::OpCode::BINARY) {
        auto data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(message.data()), message.size());
        on_binary(ws, data);
    }
},
```

Implement `on_binary`:
```cpp
void WsServer::on_binary(ws_t* ws, std::span<const uint8_t> data) {
    auto* session = ws->getUserData();
    if (!session->authenticated) {
        send_error(ws, 0, 401, "not authenticated");
        return;
    }

    // Binary frame header: [1B type][4B request_id][2B chunk_index] = 7 bytes min
    if (data.size() < 7) {
        send_error(ws, 0, 400, "binary frame too short");
        return;
    }

    uint8_t frame_type = data[0];
    uint32_t request_id = (static_cast<uint32_t>(data[1]) << 24) |
                          (static_cast<uint32_t>(data[2]) << 16) |
                          (static_cast<uint32_t>(data[3]) << 8) |
                          static_cast<uint32_t>(data[4]);
    uint16_t chunk_index = (static_cast<uint16_t>(data[5]) << 8) | data[6];
    auto payload = data.subspan(7);

    if (frame_type != 0x01) {  // Only UPLOAD_CHUNK from client
        send_error(ws, 0, 400, "invalid binary frame type");
        return;
    }

    if (!session->pending_upload || session->pending_upload->request_id != request_id) {
        send_error(ws, 0, 400, "no active upload for this request_id");
        return;
    }

    // Chunked upload handling — implemented in Task 6
    auto& upload = *session->pending_upload;
    if (chunk_index != upload.next_chunk) {
        send_error(ws, upload.id, 400, "unexpected chunk index");
        session->pending_upload.reset();
        return;
    }

    static constexpr size_t CHUNK_SIZE = 1048576;  // 1 MiB
    if (payload.size() > CHUNK_SIZE) {
        send_error(ws, upload.id, 400, "chunk exceeds 1 MiB");
        session->pending_upload.reset();
        return;
    }

    upload.data.insert(upload.data.end(), payload.begin(), payload.end());
    upload.received += static_cast<uint32_t>(payload.size());
    upload.next_chunk++;

    if (upload.received >= upload.expected_size) {
        // Upload complete — will be handled in Task 6
    }
}
```

**Step 4: Run tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: add binary WebSocket frame handling for chunked transfers"
```

---

### Task 6: Implement large SEND (chunked upload)

When SEND includes `size` (no `blob`), the server initiates a chunked upload flow: respond with SEND_READY, accept binary UPLOAD_CHUNK frames, then store and ack.

**Files:**
- Modify: `src/ws/ws_server.cpp` (handle_send — add large-SEND path, complete on_binary)
- Modify: `tests/test_ws_server.cpp`

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, SendLargeChunked) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    // Add sender to recipient's allowlist
    std::vector<uint8_t> allow_key;
    allow_key.insert(allow_key.end(), recipient_fp.begin(), recipient_fp.end());
    allow_key.insert(allow_key.end(), sender_fp.begin(), sender_fp.end());
    storage_->put(storage::TABLE_ALLOWLISTS, allow_key, std::vector<uint8_t>{0x01});

    TestWsClient sender_client;
    ASSERT_TRUE(sender_client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(sender_client, sender_kp));

    // Create a blob slightly over 64 KB (to trigger chunked path)
    std::vector<uint8_t> blob(70000, 0x42);  // 70 KB of 'B'
    uint32_t blob_size = static_cast<uint32_t>(blob.size());

    // Step 1: SEND with size (no blob)
    Json::Value send_msg;
    send_msg["type"] = "SEND";
    send_msg["id"] = 200;
    send_msg["to"] = to_hex(recipient_fp);
    send_msg["size"] = blob_size;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(sender_client.send_text(Json::writeString(writer, send_msg)));

    auto ready_resp = sender_client.recv_text(3000);
    ASSERT_TRUE(ready_resp.has_value());

    auto ready = parse_json(*ready_resp);
    ASSERT_EQ(ready["type"].asString(), "SEND_READY");
    ASSERT_EQ(ready["id"].asInt(), 200);
    uint32_t request_id = ready["request_id"].asUInt();

    // Step 2: Send chunks (1 MiB max per chunk, so 70KB = 1 chunk)
    static constexpr size_t CHUNK_SIZE = 1048576;
    size_t offset = 0;
    uint16_t chunk_idx = 0;
    while (offset < blob.size()) {
        size_t chunk_len = std::min(CHUNK_SIZE, blob.size() - offset);

        // Build binary frame: [1B type][4B request_id][2B chunk_index][payload]
        std::vector<uint8_t> frame;
        frame.push_back(0x01);  // UPLOAD_CHUNK
        frame.push_back(static_cast<uint8_t>((request_id >> 24) & 0xFF));
        frame.push_back(static_cast<uint8_t>((request_id >> 16) & 0xFF));
        frame.push_back(static_cast<uint8_t>((request_id >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(request_id & 0xFF));
        frame.push_back(static_cast<uint8_t>((chunk_idx >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(chunk_idx & 0xFF));
        frame.insert(frame.end(), blob.begin() + offset, blob.begin() + offset + chunk_len);

        ASSERT_TRUE(sender_client.send_binary(frame));
        offset += chunk_len;
        chunk_idx++;
    }

    // Step 3: Should receive SEND_ACK
    auto ack_resp = sender_client.recv_text(5000);
    ASSERT_TRUE(ack_resp.has_value()) << "no SEND_ACK after chunked upload";

    auto ack = parse_json(*ack_resp);
    EXPECT_EQ(ack["type"].asString(), "SEND_ACK");
    EXPECT_EQ(ack["id"].asInt(), 200);
    EXPECT_EQ(ack["msg_id"].asString().size(), 64u);

    // Verify blob was stored in TABLE_MESSAGE_BLOBS
    auto stored_msg_id = from_hex(ack["msg_id"].asString());
    auto stored_blob = storage_->get(storage::TABLE_MESSAGE_BLOBS, stored_msg_id);
    ASSERT_TRUE(stored_blob.has_value());
    EXPECT_EQ(stored_blob->size(), blob.size());
    EXPECT_EQ(*stored_blob, blob);

    sender_client.close();
}

TEST_F(WsServerTest, SendTooLargeRejects) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, sender_kp));

    // SEND with size > 50 MiB
    Json::Value send_msg;
    send_msg["type"] = "SEND";
    send_msg["id"] = 201;
    send_msg["to"] = to_hex(recipient_fp);
    send_msg["size"] = Json::UInt(52 * 1024 * 1024);  // 52 MiB

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, send_msg)));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 413);
    EXPECT_EQ(root["reason"].asString(), "attachment too large");

    client.close();
}
```

**Step 2: Run tests to verify they fail**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -10 && ./tests/chromatin-tests --gtest_filter=WsServerTest.Send*`
Expected: FAIL

**Step 3: Implement large SEND path in handle_send**

In `handle_send`, detect whether this is a small (has `blob`) or large (has `size`) SEND:

```cpp
void WsServer::handle_send(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    // Parse recipient
    std::string to_hex_str = msg.get("to", "").asString();
    if (to_hex_str.size() != 64) {
        send_error(ws, id, 400, "to must be 64 hex chars");
        return;
    }
    auto to_bytes = from_hex(to_hex_str);
    if (!to_bytes) {
        send_error(ws, id, 400, "invalid hex in to");
        return;
    }
    crypto::Hash recipient_fp{};
    std::copy(to_bytes->begin(), to_bytes->end(), recipient_fp.begin());

    static constexpr size_t INLINE_THRESHOLD = 64 * 1024;
    static constexpr size_t MAX_BLOB_SIZE = 50ULL * 1024 * 1024;  // 50 MiB

    bool has_blob = msg.isMember("blob") && !msg["blob"].asString().empty();
    bool has_size = msg.isMember("size");

    if (has_size && !has_blob) {
        // Large SEND — chunked upload
        uint64_t size = msg["size"].asUInt64();
        if (size > MAX_BLOB_SIZE) {
            send_error(ws, id, 413, "attachment too large");
            return;
        }
        if (session->pending_upload) {
            send_error(ws, id, 429, "upload already in progress");
            return;
        }

        uint32_t request_id = next_request_id_.fetch_add(1);

        Session::PendingUpload upload;
        upload.request_id = request_id;
        upload.recipient_fp = recipient_fp;
        upload.id = id;
        upload.expected_size = static_cast<uint32_t>(size);
        upload.started = std::chrono::steady_clock::now();
        session->pending_upload = std::move(upload);

        Json::Value resp;
        resp["type"] = "SEND_READY";
        resp["id"] = id;
        resp["request_id"] = request_id;
        send_json(ws, resp);
        return;
    }

    // Small SEND — inline blob (existing logic, updated for new tables)
    // ... (keep existing inline SEND code)
}
```

Complete the `on_binary` handler to finalize upload when all data received:

```cpp
if (upload.received >= upload.expected_size) {
    // Truncate to expected size (in case last chunk overshot)
    upload.data.resize(upload.expected_size);

    // Generate msg_id
    crypto::Hash msg_id{};
    std::random_device rd;
    for (auto& b : msg_id) b = static_cast<uint8_t>(rd());

    auto now = std::chrono::system_clock::now();
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count());

    // Store in TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS
    // (capture all needed data before resetting upload)
    auto recipient_fp = upload.recipient_fp;
    auto sender_fp = session->fingerprint;
    int send_id = upload.id;
    auto blob_data = std::move(upload.data);
    auto msg_id_copy = msg_id;
    session->pending_upload.reset();

    // Build index key/value
    std::vector<uint8_t> idx_key;
    idx_key.reserve(64);
    idx_key.insert(idx_key.end(), recipient_fp.begin(), recipient_fp.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());

    std::vector<uint8_t> idx_value;
    idx_value.reserve(44);
    idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    uint32_t sz = static_cast<uint32_t>(blob_data.size());
    idx_value.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    idx_value.push_back(static_cast<uint8_t>(sz & 0xFF));

    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());

    workers_.post([this, ws, send_id, idx_key = std::move(idx_key),
                   idx_value = std::move(idx_value),
                   blob_key = std::move(blob_key),
                   blob_data = std::move(blob_data),
                   msg_id_copy, recipient_fp]() {
        bool ok = storage_.put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);
        if (ok) storage_.put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob_data);

        // Kademlia replication (build old-format binary for wire compat)
        if (ok) {
            auto inbox_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);
            kad_.store(inbox_key, 0x02, blob_data);
        }

        loop_->defer([this, ws, send_id, ok, msg_id_copy]() {
            if (connections_.count(ws) == 0) return;
            if (ok) {
                Json::Value resp;
                resp["type"] = "SEND_ACK";
                resp["id"] = send_id;
                resp["msg_id"] = to_hex(msg_id_copy);
                send_json(ws, resp);
            } else {
                send_error(ws, send_id, 500, "store failed");
            }
        });
    });
}
```

Also increase `maxPayloadLength` in the uWS config to handle 1 MiB chunks + 7 byte header:
```cpp
.maxPayloadLength = 1048576 + 64,  // 1 MiB chunk + header overhead
```

**Step 4: Run tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: large SEND with chunked binary upload"
```

---

### Task 7: Implement chunked GET (large blob download)

When GET encounters a blob > 64 KB, send a JSON header with `size` + `chunks` count, followed by binary DOWNLOAD_CHUNK frames.

**Files:**
- Modify: `src/ws/ws_server.cpp` (handle_get large path)
- Modify: `tests/test_ws_server.cpp`

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, GetLargeChunked) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();

    // Store a 70 KB blob
    crypto::Hash msg_id{};
    msg_id.fill(0xFA);
    std::vector<uint8_t> blob(70000, 0x42);  // 70 KB of 'B'

    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    Json::Value get_msg;
    get_msg["type"] = "GET";
    get_msg["id"] = 50;
    get_msg["msg_id"] = to_hex(msg_id);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, get_msg)));

    // First: JSON header
    auto header_resp = client.recv_text(3000);
    ASSERT_TRUE(header_resp.has_value());

    auto header = parse_json(*header_resp);
    EXPECT_EQ(header["type"].asString(), "GET_RESULT");
    EXPECT_EQ(header["id"].asInt(), 50);
    EXPECT_EQ(header["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(header["size"].asUInt(), blob.size());
    uint32_t expected_chunks = (blob.size() + 1048575) / 1048576;  // ceil
    EXPECT_EQ(header["chunks"].asUInt(), expected_chunks);

    // Then: binary DOWNLOAD_CHUNK frames
    std::vector<uint8_t> received_data;
    for (uint32_t i = 0; i < expected_chunks; ++i) {
        auto frame = client.recv_frame(3000);
        ASSERT_TRUE(frame.has_value()) << "missing chunk " << i;
        ASSERT_EQ(frame->opcode, 0x02) << "chunk must be binary frame";

        // Parse header: [1B type][4B request_id][2B chunk_index]
        ASSERT_GE(frame->data.size(), 7u);
        EXPECT_EQ(frame->data[0], 0x02);  // DOWNLOAD_CHUNK
        uint16_t cidx = (static_cast<uint16_t>(frame->data[5]) << 8) | frame->data[6];
        EXPECT_EQ(cidx, i);

        auto payload = std::span<const uint8_t>(frame->data.data() + 7, frame->data.size() - 7);
        received_data.insert(received_data.end(), payload.begin(), payload.end());
    }

    EXPECT_EQ(received_data.size(), blob.size());
    EXPECT_EQ(received_data, blob);

    client.close();
}
```

**Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -10 && ./tests/chromatin-tests --gtest_filter=WsServerTest.GetLargeChunked`
Expected: FAIL (currently returns error 500 "chunked download not yet implemented")

**Step 3: Implement chunked download in handle_get**

Replace the large-blob branch in `handle_get`:

```cpp
} else {
    // Chunked download
    static constexpr size_t CHUNK_SIZE = 1048576;  // 1 MiB
    uint32_t total_size = static_cast<uint32_t>(blob->size());
    uint32_t num_chunks = (total_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    uint32_t request_id = next_request_id_.fetch_add(1);

    // Send JSON header
    Json::Value resp;
    resp["type"] = "GET_RESULT";
    resp["id"] = id;
    resp["msg_id"] = msg_id_hex;
    resp["size"] = total_size;
    resp["chunks"] = num_chunks;
    send_json(ws, resp);

    // Send binary DOWNLOAD_CHUNK frames
    for (uint32_t i = 0; i < num_chunks; ++i) {
        size_t offset = static_cast<size_t>(i) * CHUNK_SIZE;
        size_t chunk_len = std::min(CHUNK_SIZE, blob->size() - offset);

        // Build binary frame: [1B type][4B request_id][2B chunk_index][payload]
        std::vector<uint8_t> frame;
        frame.reserve(7 + chunk_len);
        frame.push_back(0x02);  // DOWNLOAD_CHUNK
        frame.push_back(static_cast<uint8_t>((request_id >> 24) & 0xFF));
        frame.push_back(static_cast<uint8_t>((request_id >> 16) & 0xFF));
        frame.push_back(static_cast<uint8_t>((request_id >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(request_id & 0xFF));
        frame.push_back(static_cast<uint8_t>((i >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(i & 0xFF));
        frame.insert(frame.end(), blob->begin() + offset, blob->begin() + offset + chunk_len);

        std::string_view sv(reinterpret_cast<const char*>(frame.data()), frame.size());
        ws->send(sv, uWS::OpCode::BINARY);
    }
}
```

**Step 4: Run tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: chunked GET download with binary DOWNLOAD_CHUNK frames"
```

---

### Task 8: Update push notifications for new storage model

NEW_MESSAGE push should include `size` field. Small messages (<=64 KB) inline the blob. Large messages (>64 KB) send `blob: null`. The on_kademlia_store callback needs to parse the inbox message binary to determine size.

**Files:**
- Modify: `src/ws/ws_server.cpp` (on_kademlia_store)
- Modify: `tests/test_ws_server.cpp`

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, PushLargeMessageBlobNull) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // Build a fake inbox message with a large blob (>64 KB)
    crypto::Hash msg_id{};
    msg_id.fill(0xA1);
    crypto::Hash sender_fp{};
    sender_fp.fill(0xB2);
    uint64_t timestamp = 1700000999;
    std::vector<uint8_t> big_blob(70000, 0x42);

    // msg_id(32) || sender_fp(32) || timestamp(8 BE) || blob_len(4 BE) || blob
    std::vector<uint8_t> msg_binary;
    msg_binary.insert(msg_binary.end(), msg_id.begin(), msg_id.end());
    msg_binary.insert(msg_binary.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        msg_binary.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    uint32_t blob_len = static_cast<uint32_t>(big_blob.size());
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 24) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 16) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>((blob_len >> 8) & 0xFF));
    msg_binary.push_back(static_cast<uint8_t>(blob_len & 0xFF));
    msg_binary.insert(msg_binary.end(), big_blob.begin(), big_blob.end());

    auto inbox_key = crypto::sha3_256_prefixed("inbox:", user_fp);
    server_->on_kademlia_store(inbox_key, 0x02, msg_binary);

    auto resp = client.recv_text(3000);
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "NEW_MESSAGE");
    EXPECT_EQ(root["msg_id"].asString(), to_hex(msg_id));
    EXPECT_EQ(root["from"].asString(), to_hex(sender_fp));
    EXPECT_EQ(root["timestamp"].asUInt64(), timestamp);
    EXPECT_EQ(root["size"].asUInt(), big_blob.size());
    EXPECT_TRUE(root["blob"].isNull()) << "large push should have blob=null";

    client.close();
}
```

**Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -10 && ./tests/chromatin-tests --gtest_filter=WsServerTest.PushLargeMessageBlobNull`
Expected: FAIL (current push always inlines the blob, and doesn't include `size`)

**Step 3: Update on_kademlia_store**

In the `data_type == 0x02` branch of `on_kademlia_store`:

```cpp
static constexpr size_t INLINE_THRESHOLD = 64 * 1024;

Json::Value push;
push["type"] = "NEW_MESSAGE";
push["msg_id"] = to_hex(msg_id);
push["from"] = to_hex(sender);
push["timestamp"] = Json::UInt64(ts);
push["size"] = blob_len;

if (blob_len <= INLINE_THRESHOLD) {
    push["blob"] = to_base64(blob);
} else {
    push["blob"] = Json::nullValue;
}
send_json(ws, push);
```

Also update the existing `PushNotification` test to check for the `size` field.

**Step 4: Run tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: push notifications with size field, large messages send blob=null"
```

---

### Task 9: Add upload timeout (30 seconds)

Incomplete chunked uploads should be cleaned up after 30 seconds. Check in the existing tick timer callback.

**Files:**
- Modify: `src/ws/ws_server.cpp` (tick timer callback)
- Modify: `tests/test_ws_server.cpp`

**Step 1: Write the failing test**

This is hard to test with a real 30s timeout. Instead, make the timeout configurable (or check that the upload state is cleaned up after the timeout). For testing, we verify the mechanism works by checking that a started upload eventually times out. Since we can't wait 30s in a test, we'll test the logic at a lower level.

Add a simple test that verifies a started upload blocks a second upload:

```cpp
TEST_F(WsServerTest, UploadAlreadyInProgressRejects) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, sender_kp));

    // Start a large SEND (don't send chunks)
    Json::Value send1;
    send1["type"] = "SEND";
    send1["id"] = 300;
    send1["to"] = to_hex(recipient_fp);
    send1["size"] = 100000;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, send1)));

    auto ready = client.recv_text(3000);
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(parse_json(*ready)["type"].asString(), "SEND_READY");

    // Try a second large SEND — should get 429
    Json::Value send2;
    send2["type"] = "SEND";
    send2["id"] = 301;
    send2["to"] = to_hex(recipient_fp);
    send2["size"] = 100000;

    ASSERT_TRUE(client.send_text(Json::writeString(writer, send2)));

    auto resp2 = client.recv_text(3000);
    ASSERT_TRUE(resp2.has_value());

    auto root = parse_json(*resp2);
    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 429);

    client.close();
}
```

**Step 2: Run test to verify it fails/passes**

This test might already pass from Task 6. Run it to verify.

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests --gtest_filter=WsServerTest.UploadAlreadyInProgressRejects`

**Step 3: Add timeout check to tick timer**

In the tick timer callback, after `data->kad->tick()`, check all connections for stale uploads:

Since the tick timer fires every 200ms and only has access to Kademlia, we need a different approach. Add a new timer or check stale uploads in the existing one. The simplest approach: expand the `TimerData` struct to also hold a pointer to the WsServer, and add a `check_upload_timeouts()` method:

In `src/ws/ws_server.h`, add:
```cpp
void check_upload_timeouts();
```

In `src/ws/ws_server.cpp`, in the timer setup:
```cpp
struct TimerData { kademlia::Kademlia* kad; WsServer* server; };
// ...
td->server = this;
us_timer_set(tick_timer_, [](struct us_timer_t* t) {
    auto* data = static_cast<TimerData*>(us_timer_ext(t));
    data->kad->tick();
    data->server->check_upload_timeouts();
}, 200, 200);
```

Implement:
```cpp
void WsServer::check_upload_timeouts() {
    static constexpr auto UPLOAD_TIMEOUT = std::chrono::seconds(30);
    auto now = std::chrono::steady_clock::now();

    for (auto* ws : connections_) {
        auto* session = ws->getUserData();
        if (session->pending_upload &&
            (now - session->pending_upload->started) > UPLOAD_TIMEOUT) {
            send_error(ws, session->pending_upload->id, 408, "upload timeout");
            session->pending_upload.reset();
        }
    }
}
```

**Step 4: Run all tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 5: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: upload timeout (30s) and concurrent upload rejection"
```

---

### Task 10: Clean up old TABLE_INBOXES usage

Remove references to the old `TABLE_INBOXES` table constant and the old inbox key format. The old table stays registered for backward compatibility (existing databases may have data in it), but no code writes to or reads from it anymore.

**Files:**
- Modify: `src/ws/ws_server.cpp` (verify no TABLE_INBOXES references remain)
- Modify: `tests/test_ws_server.cpp` (verify no TABLE_INBOXES references remain)
- Review: `src/kademlia/kademlia.cpp` (handle_store still writes to TABLE_INBOXES — leave for now as it handles the on-wire replication format; updating Kademlia storage is a separate task)

**Step 1: Search for remaining TABLE_INBOXES references**

Run: `grep -rn TABLE_INBOXES src/ tests/`

Remove or update any remaining references in ws_server.cpp and test_ws_server.cpp. The Kademlia layer will continue to use the old format for now — it handles TCP replication which is out of scope for this revision.

**Step 2: Run all tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 3: Commit**

```bash
git add src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "refactor: remove old TABLE_INBOXES usage from WS server and tests"
```

---

### Task 11: Update DELETE to use new tables

DELETE should remove from both TABLE_INBOX_INDEX and TABLE_MESSAGE_BLOBS. The current codebase doesn't have DELETE implemented yet (it's in the protocol but wasn't in the WS server tasks). Add it.

**Files:**
- Modify: `src/ws/ws_server.h` (add handle_delete)
- Modify: `src/ws/ws_server.cpp` (dispatch + handler)
- Modify: `tests/test_ws_server.cpp`

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, DeleteMessage) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto fingerprint = crypto::sha3_256(user_kp.public_key);

    // Store a message in both tables
    crypto::Hash msg_id{};
    msg_id.fill(0xDE);
    crypto::Hash sender_fp{};
    sender_fp.fill(0xAD);
    uint64_t timestamp = 1700000500;
    std::vector<uint8_t> blob = {0x01, 0x02, 0x03};

    // INDEX
    std::vector<uint8_t> idx_key;
    idx_key.insert(idx_key.end(), fingerprint.begin(), fingerprint.end());
    idx_key.insert(idx_key.end(), msg_id.begin(), msg_id.end());
    std::vector<uint8_t> idx_value;
    idx_value.insert(idx_value.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        idx_value.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    uint32_t sz = 3;
    idx_value.push_back(0); idx_value.push_back(0);
    idx_value.push_back(0); idx_value.push_back(3);
    storage_->put(storage::TABLE_INBOX_INDEX, idx_key, idx_value);

    // BLOB
    std::vector<uint8_t> blob_key(msg_id.begin(), msg_id.end());
    storage_->put(storage::TABLE_MESSAGE_BLOBS, blob_key, blob);

    // Connect and authenticate
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    ASSERT_TRUE(authenticate(client, user_kp));

    // DELETE the message
    Json::Value del;
    del["type"] = "DELETE";
    del["id"] = 60;
    Json::Value ids(Json::arrayValue);
    ids.append(to_hex(msg_id));
    del["msg_ids"] = ids;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    ASSERT_TRUE(client.send_text(Json::writeString(writer, del)));

    auto resp = client.recv_text(3000);
    ASSERT_TRUE(resp.has_value());

    auto root = parse_json(*resp);
    EXPECT_EQ(root["type"].asString(), "OK");
    EXPECT_EQ(root["id"].asInt(), 60);

    // Verify both tables are cleaned up
    EXPECT_FALSE(storage_->get(storage::TABLE_INBOX_INDEX, idx_key).has_value());
    EXPECT_FALSE(storage_->get(storage::TABLE_MESSAGE_BLOBS, blob_key).has_value());

    client.close();
}
```

**Step 2: Implement DELETE handler**

In `src/ws/ws_server.h`, add:
```cpp
void handle_delete(ws_t* ws, const Json::Value& msg);
```

In dispatch:
```cpp
} else if (type == "DELETE") {
    if (!require_auth(ws, id)) return;
    handle_delete(ws, root);
}
```

Implement:
```cpp
void WsServer::handle_delete(ws_t* ws, const Json::Value& msg) {
    auto* session = ws->getUserData();
    int id = msg.get("id", 0).asInt();

    if (!msg.isMember("msg_ids") || !msg["msg_ids"].isArray()) {
        send_error(ws, id, 400, "missing msg_ids array");
        return;
    }

    auto& ids = msg["msg_ids"];
    for (const auto& mid : ids) {
        std::string mid_hex = mid.asString();
        if (mid_hex.size() != 64) continue;
        auto mid_bytes = from_hex(mid_hex);
        if (!mid_bytes) continue;

        // Delete from TABLE_INBOX_INDEX: key = fingerprint(32) || msg_id(32)
        std::vector<uint8_t> idx_key;
        idx_key.reserve(64);
        idx_key.insert(idx_key.end(), session->fingerprint.begin(), session->fingerprint.end());
        idx_key.insert(idx_key.end(), mid_bytes->begin(), mid_bytes->end());
        storage_.del(storage::TABLE_INBOX_INDEX, idx_key);

        // Delete from TABLE_MESSAGE_BLOBS: key = msg_id(32)
        storage_.del(storage::TABLE_MESSAGE_BLOBS, *mid_bytes);
    }

    Json::Value resp;
    resp["type"] = "OK";
    resp["id"] = id;
    send_json(ws, resp);
}
```

**Step 3: Run all tests**

Run: `cd build && cmake --build . --target chromatin-tests 2>&1 | tail -5 && ./tests/chromatin-tests`
Expected: All tests pass.

**Step 4: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: DELETE command removes from both inbox_index and message_blobs"
```
