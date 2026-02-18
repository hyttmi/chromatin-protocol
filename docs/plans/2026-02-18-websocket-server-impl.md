# WebSocket Server Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the WebSocket server layer on top of the existing Kademlia engine, enabling clients to authenticate, send/fetch messages, manage allowlists, send contact requests, and receive real-time push notifications.

**Architecture:** uWebSockets event loop as main thread, worker thread pool for blocking Kademlia operations (STORE, REDIRECT seq queries), TCP accept loop in background thread for node-to-node. `uWS::Loop::defer()` bridges results back to the event loop thread.

**Tech Stack:** uWebSockets v20.74.0, uSockets v0.8.8 (already fetched), C++20, jsoncpp, liboqs (ML-DSA-87), libmdbx

**Design doc:** `docs/plans/2026-02-18-websocket-server-design.md`

---

### Task 1: WorkerPool

A simple fixed-size thread pool for offloading blocking operations from the uWS event loop.

**Files:**
- Create: `src/ws/worker_pool.h`
- Test: `tests/test_worker_pool.cpp`
- Modify: `tests/CMakeLists.txt` — add `test_worker_pool.cpp`

**Step 1: Write the failing test**

Create `tests/test_worker_pool.cpp`:

```cpp
#include <gtest/gtest.h>

#include "ws/worker_pool.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace chromatin::ws;

TEST(WorkerPoolTest, ExecutesJobs) {
    WorkerPool pool(2);
    std::atomic<int> counter{0};

    pool.post([&] { counter++; });
    pool.post([&] { counter++; });
    pool.post([&] { counter++; });

    // Wait for all jobs to complete
    for (int i = 0; i < 50 && counter.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(counter.load(), 3);
}

TEST(WorkerPoolTest, ConcurrentExecution) {
    WorkerPool pool(4);
    std::atomic<int> running{0};
    std::atomic<int> max_concurrent{0};

    for (int i = 0; i < 4; ++i) {
        pool.post([&] {
            int r = ++running;
            // Track max concurrent
            int expected = max_concurrent.load();
            while (r > expected && !max_concurrent.compare_exchange_weak(expected, r)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            --running;
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_GE(max_concurrent.load(), 2) << "Should run jobs concurrently";
}

TEST(WorkerPoolTest, GracefulShutdown) {
    std::atomic<int> counter{0};
    {
        WorkerPool pool(2);
        for (int i = 0; i < 10; ++i) {
            pool.post([&] { counter++; });
        }
    } // destructor should wait for all jobs
    EXPECT_EQ(counter.load(), 10);
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests 2>&1`
Expected: Compilation error — `ws/worker_pool.h` not found

**Step 3: Write minimal implementation**

Create `src/ws/worker_pool.h`:

```cpp
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace chromatin::ws {

class WorkerPool {
public:
    explicit WorkerPool(size_t num_threads = 4) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] { return !jobs_.empty() || stop_; });
                        if (stop_ && jobs_.empty()) return;
                        job = std::move(jobs_.front());
                        jobs_.pop();
                    }
                    job();
                }
            });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void post(std::function<void()> job) {
        {
            std::lock_guard lock(mutex_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace chromatin::ws
```

Add `test_worker_pool.cpp` to `tests/CMakeLists.txt`:

```cmake
target_sources(chromatin-tests PRIVATE
    ...
    test_worker_pool.cpp
)
```

**Step 4: Run test to verify it passes**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests && ./tests/chromatin-tests --gtest_filter="WorkerPool*"`
Expected: 3 tests PASS

**Step 5: Commit**

```bash
git add src/ws/worker_pool.h tests/test_worker_pool.cpp tests/CMakeLists.txt
git commit -m "feat: WorkerPool — fixed-size thread pool for WS server"
```

---

### Task 2: Storage::scan()

Add prefix-scan method to Storage using mdbx cursor. Enables efficient inbox retrieval by fingerprint prefix.

**Files:**
- Modify: `src/storage/storage.h:38` — add `scan()` declaration after `foreach()`
- Modify: `src/storage/storage.cpp:101` — add `scan()` implementation after `foreach()`
- Modify: `tests/test_storage.cpp` — add scan tests

**Step 1: Write the failing tests**

Append to `tests/test_storage.cpp`:

```cpp
TEST_F(StorageTest, ScanMatchesPrefix) {
    // Simulate composite keys: prefix(2 bytes) + suffix(1 byte)
    std::vector<uint8_t> k1 = {'A', 'A', '1'};
    std::vector<uint8_t> k2 = {'A', 'A', '2'};
    std::vector<uint8_t> k3 = {'A', 'B', '1'};  // different prefix
    std::vector<uint8_t> k4 = {'A', 'A', '3'};

    store_->put(TABLE_INBOXES, k1, to_bytes("v1"));
    store_->put(TABLE_INBOXES, k2, to_bytes("v2"));
    store_->put(TABLE_INBOXES, k3, to_bytes("v3"));
    store_->put(TABLE_INBOXES, k4, to_bytes("v4"));

    std::vector<uint8_t> prefix = {'A', 'A'};
    std::vector<std::vector<uint8_t>> found_keys;

    store_->scan(TABLE_INBOXES, prefix, [&](std::span<const uint8_t> key, std::span<const uint8_t>) {
        found_keys.emplace_back(key.begin(), key.end());
        return true;
    });

    EXPECT_EQ(found_keys.size(), 3u);  // k1, k2, k4
}

TEST_F(StorageTest, ScanEarlyStop) {
    std::vector<uint8_t> k1 = {'X', '1'};
    std::vector<uint8_t> k2 = {'X', '2'};
    std::vector<uint8_t> k3 = {'X', '3'};

    store_->put(TABLE_INBOXES, k1, to_bytes("v1"));
    store_->put(TABLE_INBOXES, k2, to_bytes("v2"));
    store_->put(TABLE_INBOXES, k3, to_bytes("v3"));

    std::vector<uint8_t> prefix = {'X'};
    int count = 0;

    store_->scan(TABLE_INBOXES, prefix, [&](std::span<const uint8_t>, std::span<const uint8_t>) {
        ++count;
        return false;  // stop after first
    });

    EXPECT_EQ(count, 1);
}

TEST_F(StorageTest, ScanNoMatches) {
    std::vector<uint8_t> k1 = {'A', '1'};
    store_->put(TABLE_INBOXES, k1, to_bytes("v1"));

    std::vector<uint8_t> prefix = {'Z'};
    int count = 0;

    store_->scan(TABLE_INBOXES, prefix, [&](std::span<const uint8_t>, std::span<const uint8_t>) {
        ++count;
        return true;
    });

    EXPECT_EQ(count, 0);
}

TEST_F(StorageTest, ScanEmptyTable) {
    std::vector<uint8_t> prefix = {'A'};
    int count = 0;

    store_->scan(TABLE_NODES, prefix, [&](std::span<const uint8_t>, std::span<const uint8_t>) {
        ++count;
        return true;
    });

    EXPECT_EQ(count, 0);
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests 2>&1`
Expected: Compilation error — `scan` is not a member of `Storage`

**Step 3: Write minimal implementation**

Add declaration to `src/storage/storage.h` after line 39 (`foreach` declaration):

```cpp
    void scan(std::string_view table, std::span<const uint8_t> prefix, Callback cb) const;
```

Add implementation to `src/storage/storage.cpp` after the `foreach()` method:

```cpp
void Storage::scan(std::string_view table, std::span<const uint8_t> prefix, Callback cb) const {
    auto map = get_map(table);
    auto txn = env_.start_read();
    auto cursor = txn.open_cursor(map);

    mdbx::slice prefix_slice(prefix.data(), prefix.size());
    auto result = cursor.lower_bound(prefix_slice, false);
    while (result) {
        auto k = result.key;
        auto v = result.value;

        // Stop if key no longer starts with prefix
        if (k.length() < prefix.size() ||
            std::memcmp(k.data(), prefix.data(), prefix.size()) != 0) {
            break;
        }

        std::span<const uint8_t> key_span(static_cast<const uint8_t*>(k.data()), k.length());
        std::span<const uint8_t> val_span(static_cast<const uint8_t*>(v.data()), v.length());
        if (!cb(key_span, val_span)) break;

        result = cursor.to_next(false);
    }
}
```

Add `#include <cstring>` to `storage.cpp` if not already present (for `std::memcmp`).

**Step 4: Run test to verify it passes**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests && ./tests/chromatin-tests --gtest_filter="Storage*"`
Expected: All storage tests PASS (13 old + 4 new = 17 tests)

**Step 5: Commit**

```bash
git add src/storage/storage.h src/storage/storage.cpp tests/test_storage.cpp
git commit -m "feat: Storage::scan() — prefix scan using mdbx cursor"
```

---

### Task 3: Kademlia::set_on_store() callback

Add a callback that fires after every successful local store in `handle_store()`. This is the bridge between the Kademlia TCP layer and the WS push notification system.

**Files:**
- Modify: `src/kademlia/kademlia.h:55-56` — add StoreCallback typedef and set_on_store() declaration
- Modify: `src/kademlia/kademlia.h:86` — add on_store_ member
- Modify: `src/kademlia/kademlia.cpp:398` — call on_store_ after successful store
- Modify: `src/kademlia/kademlia.cpp:527` — call on_store_ after successful local store in store()
- Modify: `tests/test_kademlia.cpp` — add on_store callback test

**Step 1: Write the failing test**

Append to `tests/test_kademlia.cpp` (after the existing tests):

```cpp
TEST_F(KademliaTest, OnStoreCallback) {
    auto& n1 = create_node();
    auto& n2 = create_node();
    start_all();

    n2.kad->bootstrap({{"127.0.0.1", n1.info.tcp_port}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Set up on_store callback on n1
    std::atomic<int> store_count{0};
    crypto::Hash last_key{};
    uint8_t last_type = 0;

    n1.kad->set_on_store([&](const crypto::Hash& key, uint8_t data_type,
                             std::span<const uint8_t> /*value*/) {
        last_key = key;
        last_type = data_type;
        store_count++;
    });

    // Create a user keypair and register a name (will trigger store on responsible nodes)
    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);
    std::string name = "alice";
    uint64_t nonce = find_pow_nonce(name, user_fp, n1.kad->name_pow_difficulty());
    auto record = build_name_record(name, user_fp, nonce, 1, user_kp);
    auto key = name_key(name);

    n2.kad->store(key, 0x01, record);

    // Wait for store to propagate
    for (int i = 0; i < 50 && store_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // n1 should have received the store callback (if responsible)
    if (n1.kad->is_responsible(key)) {
        EXPECT_GE(store_count.load(), 1);
        EXPECT_EQ(last_type, 0x01);
    }
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests 2>&1`
Expected: Compilation error — `set_on_store` not a member of `Kademlia`

**Step 3: Write minimal implementation**

Add to `src/kademlia/kademlia.h` after line 56 (after `self()` getter):

```cpp
    // Callback invoked after every successful local store (for push notifications).
    // Called on the TCP accept thread — callers must use defer() for thread safety.
    using StoreCallback = std::function<void(const crypto::Hash& key,
                                             uint8_t data_type,
                                             std::span<const uint8_t> value)>;
    void set_on_store(StoreCallback cb);
```

Add to `src/kademlia/kademlia.h` private members (after `int name_pow_difficulty_ = 28;`):

```cpp
    StoreCallback on_store_;
```

Add implementation to `src/kademlia/kademlia.cpp` (after `set_bootstrap_addrs`):

```cpp
void Kademlia::set_on_store(StoreCallback cb) {
    on_store_ = std::move(cb);
}
```

In `handle_store()` (after `spdlog::info("Stored data_type=...")`  at line 399, before the STORE_ACK section):

```cpp
    // Notify WS layer about the new store
    if (on_store_) {
        on_store_(key, data_type, value);
    }
```

In `store()` local storage path (after `repl_log_.append(key, ...)` at line 524, before `stored_any = true;`):

```cpp
            // Notify WS layer about the new store
            if (on_store_) {
                on_store_(key, data_type, value);
            }
```

**Step 4: Run test to verify it passes**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests && ./tests/chromatin-tests --gtest_filter="KademliaTest.OnStoreCallback"`
Expected: PASS

Also run all existing tests to ensure no regressions:

Run: `cd /home/mika/dev/dna-helix/build && make -j$(nproc) chromatin-tests && ./tests/chromatin-tests`
Expected: All tests PASS

**Step 5: Commit**

```bash
git add src/kademlia/kademlia.h src/kademlia/kademlia.cpp tests/test_kademlia.cpp
git commit -m "feat: Kademlia::set_on_store() callback for push notifications"
```

---

### Task 4: WsServer skeleton + CMake

Create the WsServer class with uWS event loop, timer for `tick()`, and graceful shutdown. No command handling yet — just accepts connections and logs open/close.

**Files:**
- Create: `src/ws/ws_server.h`
- Create: `src/ws/ws_server.cpp`
- Modify: `src/CMakeLists.txt` — add ws sources, link uWebSockets
- Create: `tests/test_ws_client.h` — minimal raw-socket WebSocket test client
- Create: `tests/test_ws_server.cpp` — skeleton test
- Modify: `tests/CMakeLists.txt` — add ws test, link uWebSockets

**Step 1: Write the failing test**

Create `tests/test_ws_client.h` — a minimal WebSocket client for testing:

```cpp
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Minimal WebSocket client for testing. Sends/receives JSON text frames.
class TestWsClient {
public:
    bool connect(const std::string& host, uint16_t port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // WebSocket HTTP upgrade handshake
        std::string req =
            "GET / HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";

        if (write_all(req.data(), req.size()) < 0) return false;

        // Read HTTP response (look for 101)
        char buf[1024];
        ssize_t n = recv(fd_, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return false;
        buf[n] = '\0';
        return std::string(buf).find("101") != std::string::npos;
    }

    bool send_text(const std::string& msg) {
        if (fd_ < 0) return false;

        std::vector<uint8_t> frame;
        frame.push_back(0x81);  // FIN + text opcode

        // Mask bit set (client must mask), payload length
        if (msg.size() < 126) {
            frame.push_back(0x80 | static_cast<uint8_t>(msg.size()));
        } else {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((msg.size() >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(msg.size() & 0xFF));
        }

        // Mask key (fixed for simplicity)
        uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.insert(frame.end(), mask, mask + 4);

        // Masked payload
        for (size_t i = 0; i < msg.size(); ++i) {
            frame.push_back(msg[i] ^ mask[i % 4]);
        }

        return write_all(reinterpret_cast<const char*>(frame.data()), frame.size()) >= 0;
    }

    std::optional<std::string> recv_text(int timeout_ms = 2000) {
        if (fd_ < 0) return std::nullopt;

        // Set receive timeout
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t header[2];
        if (recv_all(header, 2) < 0) return std::nullopt;

        // Check FIN + text opcode
        if ((header[0] & 0x0F) != 0x01) return std::nullopt;

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

        std::vector<char> buf(payload_len);
        if (recv_all(reinterpret_cast<uint8_t*>(buf.data()), payload_len) < 0)
            return std::nullopt;

        return std::string(buf.begin(), buf.end());
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~TestWsClient() { close(); }

private:
    int fd_ = -1;

    ssize_t write_all(const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return -1;
            sent += n;
        }
        return static_cast<ssize_t>(sent);
    }

    ssize_t recv_all(uint8_t* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::recv(fd_, buf + got, len - got, 0);
            if (n <= 0) return -1;
            got += n;
        }
        return static_cast<ssize_t>(got);
    }
};
```

Create `tests/test_ws_server.cpp`:

```cpp
#include <gtest/gtest.h>

#include "test_ws_client.h"
#include "ws/ws_server.h"

#include "config/config.h"
#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "kademlia/routing_table.h"
#include "kademlia/tcp_transport.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

#include <filesystem>
#include <thread>

using namespace chromatin;

class WsServerTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<storage::Storage> storage_;
    std::unique_ptr<replication::ReplLog> repl_log_;
    std::unique_ptr<kademlia::RoutingTable> routing_table_;
    std::unique_ptr<kademlia::TcpTransport> transport_;
    std::unique_ptr<kademlia::Kademlia> kademlia_;
    std::unique_ptr<ws::WsServer> server_;
    std::thread tcp_thread_;
    std::thread ws_thread_;
    crypto::KeyPair node_keypair_;
    config::Config cfg_;
    uint16_t ws_port_ = 0;

    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("chromatin_ws_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(db_path_);

        storage_ = std::make_unique<storage::Storage>(db_path_ / "test.mdbx");
        repl_log_ = std::make_unique<replication::ReplLog>(*storage_);
        routing_table_ = std::make_unique<kademlia::RoutingTable>();

        transport_ = std::make_unique<kademlia::TcpTransport>("127.0.0.1", 0);
        node_keypair_ = crypto::generate_keypair();

        auto node_id = kademlia::NodeId::from_pubkey(node_keypair_.public_key);
        kademlia::NodeInfo self;
        self.id = node_id;
        self.address = "127.0.0.1";
        self.tcp_port = transport_->local_port();
        self.ws_port = 0;  // will be set after WS listen
        self.pubkey = node_keypair_.public_key;
        self.last_seen = std::chrono::steady_clock::now();

        kademlia_ = std::make_unique<kademlia::Kademlia>(
            self, *transport_, *routing_table_, *storage_, *repl_log_, node_keypair_);
        kademlia_->set_name_pow_difficulty(8);

        // Start TCP accept thread
        tcp_thread_ = std::thread([this]() {
            transport_->run([this](const kademlia::Message& msg,
                                  const std::string& from, uint16_t port) {
                kademlia_->handle_message(msg, from, port);
            });
        });

        cfg_.bind = "127.0.0.1";
        cfg_.ws_port = 0;  // ephemeral
        cfg_.data_dir = db_path_;
    }

    void start_ws_server() {
        server_ = std::make_unique<ws::WsServer>(
            cfg_, *kademlia_, *storage_, *repl_log_, node_keypair_);

        kademlia_->set_on_store([this](const crypto::Hash& key, uint8_t type,
                                       std::span<const uint8_t> value) {
            server_->on_kademlia_store(key, type, value);
        });

        ws_thread_ = std::thread([this]() { server_->run(); });

        // Wait for server to start listening
        for (int i = 0; i < 50; ++i) {
            ws_port_ = server_->listening_port();
            if (ws_port_ > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ASSERT_GT(ws_port_, 0u) << "WS server failed to start";
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (ws_thread_.joinable()) ws_thread_.join();
        transport_->stop();
        if (tcp_thread_.joinable()) tcp_thread_.join();
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }
};

TEST_F(WsServerTest, AcceptsConnection) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    client.close();
}
```

**Step 2: Run test to verify it fails**

Expected: Compilation error — `ws/ws_server.h` not found

**Step 3: Write minimal implementation**

Create `src/ws/ws_server.h`:

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_set>

#include <uWebSockets/App.h>

#include "config/config.h"
#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "replication/repl_log.h"
#include "storage/storage.h"
#include "ws/worker_pool.h"

namespace chromatin::ws {

struct Session {
    crypto::Hash fingerprint{};
    std::vector<uint8_t> pubkey;
    bool authenticated = false;
    crypto::Hash challenge_nonce{};
};

class WsServer {
public:
    WsServer(const config::Config& cfg,
             kademlia::Kademlia& kad,
             storage::Storage& storage,
             replication::ReplLog& repl_log,
             const crypto::KeyPair& keypair);

    // Block on uWS event loop (call from main thread)
    void run();

    // Stop the event loop (thread-safe)
    void stop();

    // Called from TCP thread when a STORE arrives via Kademlia.
    // Thread-safe: uses defer() internally.
    void on_kademlia_store(const crypto::Hash& key,
                           uint8_t data_type,
                           std::span<const uint8_t> value);

    // Get the listening port (0 if not yet listening)
    uint16_t listening_port() const { return listening_port_.load(); }

private:
    using ws_t = uWS::WebSocket<false, true, Session>;

    const config::Config& cfg_;
    kademlia::Kademlia& kad_;
    storage::Storage& storage_;
    replication::ReplLog& repl_log_;
    const crypto::KeyPair& keypair_;

    WorkerPool workers_{4};
    uWS::Loop* loop_ = nullptr;
    us_listen_socket_t* listen_socket_ = nullptr;
    std::atomic<uint16_t> listening_port_{0};

    // Authenticated sessions: fingerprint -> ws pointer (uWS thread only)
    std::unordered_map<crypto::Hash, ws_t*, crypto::HashHash> authenticated_;

    // Command dispatch
    void on_message(ws_t* ws, std::string_view message);

    // Helpers
    void send_json(ws_t* ws, const Json::Value& msg);
    void send_error(ws_t* ws, int id, int code, const std::string& reason);
};

} // namespace chromatin::ws
```

Create `src/ws/ws_server.cpp`:

```cpp
#include "ws/ws_server.h"

#include <json/json.h>
#include <spdlog/spdlog.h>

namespace chromatin::ws {

WsServer::WsServer(const config::Config& cfg,
                   kademlia::Kademlia& kad,
                   storage::Storage& storage,
                   replication::ReplLog& repl_log,
                   const crypto::KeyPair& keypair)
    : cfg_(cfg)
    , kad_(kad)
    , storage_(storage)
    , repl_log_(repl_log)
    , keypair_(keypair) {}

void WsServer::run() {
    uWS::App app;
    loop_ = uWS::Loop::get();

    app.ws<Session>("/*", {
        .compression = uWS::DISABLED,
        .maxPayloadLength = 512 * 1024,
        .idleTimeout = 120,

        .open = [this](ws_t* ws) {
            spdlog::info("WS: client connected");
        },

        .message = [this](ws_t* ws, std::string_view message, uWS::OpCode opCode) {
            if (opCode != uWS::OpCode::TEXT) return;
            on_message(ws, message);
        },

        .close = [this](ws_t* ws, int /*code*/, std::string_view /*message*/) {
            auto* session = ws->getUserData();
            if (session->authenticated) {
                authenticated_.erase(session->fingerprint);
            }
            spdlog::info("WS: client disconnected");
        }
    });

    app.listen("0.0.0.0", cfg_.ws_port, [this](us_listen_socket_t* socket) {
        if (socket) {
            listen_socket_ = socket;
            listening_port_.store(us_socket_local_port(
                /*ssl=*/false, reinterpret_cast<us_socket_t*>(socket)));
            spdlog::info("WS: listening on port {}", listening_port_.load());
        } else {
            spdlog::error("WS: failed to listen on port {}", cfg_.ws_port);
        }
    });

    // Periodic tick timer (200ms)
    struct us_loop_t* us_loop = reinterpret_cast<struct us_loop_t*>(loop_);
    struct us_timer_t* timer = us_create_timer(us_loop, 0, 0);
    us_timer_set(timer, [](struct us_timer_t* t) {
        // Timer callback — we need access to kademlia.
        // This is set up after the fact via timer user data.
    }, 200, 200);

    // Store kademlia pointer in timer for the callback
    // uWS timers don't have user data, so we use a static/captured approach
    // Instead, use a simpler approach: run tick in the app's pre handler
    us_timer_close(timer);

    // Use a repeated timer via uWS::Loop
    auto tick_timer = [this]() {
        kad_.tick();
    };

    // Create a proper timer
    struct TimerData { kademlia::Kademlia* kad; };
    struct us_timer_t* tick = us_create_timer(us_loop, 0, sizeof(TimerData));
    auto* td = static_cast<TimerData*>(us_timer_ext(tick));
    td->kad = &kad_;
    us_timer_set(tick, [](struct us_timer_t* t) {
        auto* data = static_cast<TimerData*>(us_timer_ext(t));
        data->kad->tick();
    }, 200, 200);

    app.run();
}

void WsServer::stop() {
    if (loop_) {
        loop_->defer([this]() {
            if (listen_socket_) {
                us_listen_socket_close(0, listen_socket_);
                listen_socket_ = nullptr;
            }
            // uWS will exit run() when no listeners and no connections remain
        });
    }
}

void WsServer::on_kademlia_store(const crypto::Hash& key,
                                  uint8_t data_type,
                                  std::span<const uint8_t> value) {
    // Will be implemented in Task 9 (push notifications)
    // For now, this is a no-op placeholder
}

void WsServer::on_message(ws_t* ws, std::string_view message) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(std::string(message));

    if (!Json::parseFromStream(builder, stream, &root, &errs)) {
        send_error(ws, 0, 400, "invalid JSON");
        return;
    }

    std::string type = root.get("type", "").asString();
    int id = root.get("id", 0).asInt();

    if (type.empty()) {
        send_error(ws, id, 400, "missing type");
        return;
    }

    // Command dispatch — will be filled in by subsequent tasks
    send_error(ws, id, 400, "unknown command: " + type);
}

void WsServer::send_json(ws_t* ws, const Json::Value& msg) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string json = Json::writeString(builder, msg);
    ws->send(json, uWS::OpCode::TEXT);
}

void WsServer::send_error(ws_t* ws, int id, int code, const std::string& reason) {
    Json::Value err;
    err["type"] = "ERROR";
    err["id"] = id;
    err["code"] = code;
    err["reason"] = reason;
    send_json(ws, err);
}

} // namespace chromatin::ws
```

Update `src/CMakeLists.txt`:

```cmake
add_library(chromatin-core STATIC)
target_sources(chromatin-core PRIVATE
    crypto/crypto.cpp
    storage/storage.cpp
    kademlia/node_id.cpp
    kademlia/routing_table.cpp
    kademlia/tcp_transport.cpp
    kademlia/kademlia.cpp
    replication/repl_log.cpp
    config/config.cpp
    ws/ws_server.cpp
)
target_include_directories(chromatin-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(chromatin-core PRIVATE ${liboqs_BINARY_DIR}/include)
target_link_libraries(chromatin-core PUBLIC
    oqs
    mdbx-static
    jsoncpp_static
    spdlog::spdlog
    uWebSockets
    Threads::Threads
)
```

Update `tests/CMakeLists.txt` — add `test_ws_server.cpp`:

```cmake
target_sources(chromatin-tests PRIVATE
    ...
    test_ws_server.cpp
)
```

**Step 4: Run test to verify it passes**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests && ./tests/chromatin-tests --gtest_filter="WsServer*"`
Expected: `AcceptsConnection` PASS

Also run all tests to check for regressions.

**Step 5: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp src/ws/worker_pool.h \
        src/CMakeLists.txt tests/test_ws_client.h tests/test_ws_server.cpp \
        tests/CMakeLists.txt
git commit -m "feat: WsServer skeleton with uWS event loop and timer"
```

---

### Task 5: Authentication flow (HELLO, CHALLENGE, AUTH)

Implement the full auth flow: HELLO → REDIRECT or CHALLENGE → AUTH → OK.

**Files:**
- Modify: `src/ws/ws_server.h` — add handle_hello, handle_auth declarations
- Modify: `src/ws/ws_server.cpp` — implement auth handlers
- Modify: `tests/test_ws_server.cpp` — add auth flow tests

**Step 1: Write the failing tests**

Add to `tests/test_ws_server.cpp`:

```cpp
TEST_F(WsServerTest, HelloChallenge) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    // Generate a user keypair
    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Convert fingerprint to hex
    std::string fp_hex;
    for (auto b : user_fp) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        fp_hex += buf;
    }

    // Send HELLO
    std::string hello = R"({"type":"HELLO","id":1,"fingerprint":")" + fp_hex + R"("})";
    ASSERT_TRUE(client.send_text(hello));

    // Expect CHALLENGE or REDIRECT
    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value()) << "No response from server";

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, stream, &root, &errs));

    std::string type = root["type"].asString();
    EXPECT_TRUE(type == "CHALLENGE" || type == "REDIRECT")
        << "Expected CHALLENGE or REDIRECT, got: " << type;
}

TEST_F(WsServerTest, FullAuthFlow) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    auto to_hex = [](const auto& bytes) {
        std::string hex;
        for (auto b : bytes) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }
        return hex;
    };

    // HELLO
    std::string hello = R"({"type":"HELLO","id":1,"fingerprint":")" + to_hex(user_fp) + R"("})";
    ASSERT_TRUE(client.send_text(hello));

    auto resp1 = client.recv_text();
    ASSERT_TRUE(resp1.has_value());

    Json::Value challenge_msg;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s1(*resp1);
    ASSERT_TRUE(Json::parseFromStream(builder, s1, &challenge_msg, &errs));

    if (challenge_msg["type"].asString() == "REDIRECT") {
        // Not responsible — test passes (redirect is valid behavior)
        return;
    }

    ASSERT_EQ(challenge_msg["type"].asString(), "CHALLENGE");
    std::string nonce_hex = challenge_msg["nonce"].asString();

    // Parse nonce from hex
    std::vector<uint8_t> nonce;
    for (size_t i = 0; i < nonce_hex.size(); i += 2) {
        nonce.push_back(static_cast<uint8_t>(std::stoi(nonce_hex.substr(i, 2), nullptr, 16)));
    }

    // Sign the nonce
    auto signature = crypto::sign(nonce, user_kp.secret_key);

    // AUTH
    std::string auth = R"({"type":"AUTH","id":1,"signature":")" + to_hex(signature) +
                       R"(","pubkey":")" + to_hex(user_kp.public_key) + R"("})";
    ASSERT_TRUE(client.send_text(auth));

    auto resp2 = client.recv_text();
    ASSERT_TRUE(resp2.has_value());

    Json::Value ok_msg;
    std::istringstream s2(*resp2);
    ASSERT_TRUE(Json::parseFromStream(builder, s2, &ok_msg, &errs));
    EXPECT_EQ(ok_msg["type"].asString(), "OK");
}

TEST_F(WsServerTest, CommandBeforeAuthFails) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    // Try FETCH without authenticating
    ASSERT_TRUE(client.send_text(R"({"type":"FETCH","id":1})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    EXPECT_EQ(root["type"].asString(), "ERROR");
    EXPECT_EQ(root["code"].asInt(), 401);
}
```

**Step 2: Run test to verify it fails**

Expected: Tests fail — HELLO returns "unknown command"

**Step 3: Write minimal implementation**

Add to `ws_server.h` private section:

```cpp
    void handle_hello(ws_t* ws, const Json::Value& msg);
    void handle_auth(ws_t* ws, const Json::Value& msg);
    bool require_auth(ws_t* ws, int id);
```

Add to `ws_server.cpp` `on_message()` dispatch (replace the fallthrough error):

```cpp
    if (type == "HELLO") {
        handle_hello(ws, root);
    } else if (type == "AUTH") {
        handle_auth(ws, root);
    } else if (type == "FETCH" || type == "SEND" || type == "ALLOW" ||
               type == "REVOKE" || type == "CONTACT_REQUEST") {
        if (!require_auth(ws, id)) return;
        // Individual handlers will be added in later tasks
        send_error(ws, id, 400, "not yet implemented: " + type);
    } else {
        send_error(ws, id, 400, "unknown command: " + type);
    }
```

Implement `handle_hello()`:

```cpp
void WsServer::handle_hello(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();
    std::string fp_hex = msg.get("fingerprint", "").asString();

    if (fp_hex.size() != 64) {
        send_error(ws, id, 400, "fingerprint must be 64 hex chars");
        return;
    }

    // Parse fingerprint from hex
    crypto::Hash fp{};
    for (size_t i = 0; i < 32; ++i) {
        fp[i] = static_cast<uint8_t>(std::stoi(fp_hex.substr(i * 2, 2), nullptr, 16));
    }

    // Check responsibility
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", fp);
    if (!kad_.is_responsible(inbox_key)) {
        // REDIRECT: query responsible nodes for seq numbers
        auto nodes = kad_.responsible_nodes(inbox_key);
        Json::Value redirect;
        redirect["type"] = "REDIRECT";
        redirect["id"] = id;
        Json::Value node_list(Json::arrayValue);
        for (const auto& node : nodes) {
            auto seq = repl_log_.current_seq(inbox_key);
            Json::Value n;
            n["address"] = node.address;
            n["ws_port"] = node.ws_port;
            n["seq"] = static_cast<Json::UInt64>(seq);
            node_list.append(n);
        }
        redirect["nodes"] = node_list;
        send_json(ws, redirect);
        ws->close();
        return;
    }

    // Generate 32-byte random nonce
    auto* session = ws->getUserData();
    session->fingerprint = fp;

    // Generate random nonce
    std::random_device rd;
    for (auto& b : session->challenge_nonce) {
        b = static_cast<uint8_t>(rd());
    }

    // Send CHALLENGE
    std::string nonce_hex;
    for (auto b : session->challenge_nonce) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        nonce_hex += buf;
    }

    Json::Value challenge;
    challenge["type"] = "CHALLENGE";
    challenge["id"] = id;
    challenge["nonce"] = nonce_hex;
    send_json(ws, challenge);
}
```

Implement `handle_auth()`:

```cpp
void WsServer::handle_auth(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();
    auto* session = ws->getUserData();

    std::string sig_hex = msg.get("signature", "").asString();
    std::string pk_hex = msg.get("pubkey", "").asString();

    if (sig_hex.empty() || pk_hex.empty()) {
        send_error(ws, id, 400, "missing signature or pubkey");
        return;
    }

    // Parse hex
    auto from_hex = [](const std::string& hex) -> std::vector<uint8_t> {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        }
        return bytes;
    };

    auto signature = from_hex(sig_hex);
    auto pubkey = from_hex(pk_hex);

    // Verify fingerprint == SHA3-256(pubkey)
    auto computed_fp = crypto::sha3_256(pubkey);
    if (computed_fp != session->fingerprint) {
        send_error(ws, id, 403, "fingerprint does not match pubkey");
        return;
    }

    // Verify ML-DSA-87 signature over the nonce
    std::span<const uint8_t> nonce_span(session->challenge_nonce.data(),
                                         session->challenge_nonce.size());
    if (!crypto::verify(nonce_span, signature, pubkey)) {
        send_error(ws, id, 403, "signature verification failed");
        return;
    }

    // Authentication successful
    session->pubkey = pubkey;
    session->authenticated = true;
    authenticated_[session->fingerprint] = ws;

    // Count pending messages
    int pending = 0;
    storage_.scan(storage::TABLE_INBOXES, session->fingerprint,
                  [&](std::span<const uint8_t>, std::span<const uint8_t>) {
                      pending++;
                      return true;
                  });

    Json::Value ok;
    ok["type"] = "OK";
    ok["id"] = id;
    ok["pending_messages"] = pending;
    send_json(ws, ok);

    spdlog::info("WS: client authenticated");
}
```

Implement `require_auth()`:

```cpp
bool WsServer::require_auth(ws_t* ws, int id) {
    auto* session = ws->getUserData();
    if (!session->authenticated) {
        send_error(ws, id, 401, "not authenticated");
        return false;
    }
    return true;
}
```

**Step 4: Run test to verify it passes**

Run: `cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-tests && ./tests/chromatin-tests --gtest_filter="WsServer*"`
Expected: All WS tests PASS

**Step 5: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: WS auth flow — HELLO, CHALLENGE, AUTH with ML-DSA-87"
```

---

### Task 6: FETCH command

Retrieve messages from local storage using prefix scan.

**Files:**
- Modify: `src/ws/ws_server.h` — add handle_fetch declaration
- Modify: `src/ws/ws_server.cpp` — implement FETCH handler
- Modify: `tests/test_ws_server.cpp` — add FETCH test

**Step 1: Write the failing test**

Add helper to `WsServerTest` class for authenticating a client:

```cpp
    // Helper: authenticate a client, returns true if authenticated (not redirected)
    bool authenticate(TestWsClient& client, const crypto::KeyPair& user_kp) {
        auto user_fp = crypto::sha3_256(user_kp.public_key);
        auto to_hex = [](const auto& bytes) {
            std::string hex;
            for (auto b : bytes) {
                char buf[3]; snprintf(buf, sizeof(buf), "%02x", b); hex += buf;
            }
            return hex;
        };
        auto from_hex = [](const std::string& hex) {
            std::vector<uint8_t> bytes;
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            return bytes;
        };

        client.send_text(R"({"type":"HELLO","id":1,"fingerprint":")" + to_hex(user_fp) + R"("})");
        auto resp = client.recv_text();
        if (!resp) return false;

        Json::Value msg;
        Json::CharReaderBuilder b;
        std::string e;
        std::istringstream s(*resp);
        Json::parseFromStream(b, s, &msg, &e);

        if (msg["type"].asString() == "REDIRECT") return false;
        if (msg["type"].asString() != "CHALLENGE") return false;

        auto nonce = from_hex(msg["nonce"].asString());
        auto sig = crypto::sign(nonce, user_kp.secret_key);

        client.send_text(R"({"type":"AUTH","id":1,"signature":")" + to_hex(sig) +
                         R"(","pubkey":")" + to_hex(user_kp.public_key) + R"("})");
        auto resp2 = client.recv_text();
        if (!resp2) return false;

        Json::Value ok;
        std::istringstream s2(*resp2);
        Json::parseFromStream(b, s2, &ok, &e);
        return ok["type"].asString() == "OK";
    }
```

Add FETCH test:

```cpp
TEST_F(WsServerTest, FetchEmptyInbox) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    auto user_kp = crypto::generate_keypair();
    if (!authenticate(client, user_kp)) {
        GTEST_SKIP() << "Node not responsible for this user's inbox";
    }

    // FETCH with no messages stored
    ASSERT_TRUE(client.send_text(R"({"type":"FETCH","id":2})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    EXPECT_EQ(root["type"].asString(), "MESSAGES");
    EXPECT_EQ(root["id"].asInt(), 2);
    EXPECT_EQ(root["messages"].size(), 0u);
}

TEST_F(WsServerTest, FetchWithMessages) {
    start_ws_server();

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));

    auto user_kp = crypto::generate_keypair();
    if (!authenticate(client, user_kp)) {
        GTEST_SKIP() << "Node not responsible for this user's inbox";
    }

    auto user_fp = crypto::sha3_256(user_kp.public_key);

    // Manually insert a message into the inbox
    // Key: recipient_fp(32) || timestamp(8 BE) || msg_id(32)
    crypto::Hash msg_id{};
    msg_id[0] = 0x42;
    uint64_t ts = 1708000100;

    std::vector<uint8_t> inbox_key;
    inbox_key.insert(inbox_key.end(), user_fp.begin(), user_fp.end());
    for (int i = 7; i >= 0; --i)
        inbox_key.push_back(static_cast<uint8_t>((ts >> (i * 8)) & 0xFF));
    inbox_key.insert(inbox_key.end(), msg_id.begin(), msg_id.end());

    // Value: msg_id(32) || sender_fp(32) || timestamp(8) || blob
    crypto::Hash sender_fp{};
    sender_fp[0] = 0x99;
    std::vector<uint8_t> blob = {'h', 'e', 'l', 'l', 'o'};

    std::vector<uint8_t> inbox_val;
    inbox_val.insert(inbox_val.end(), msg_id.begin(), msg_id.end());
    inbox_val.insert(inbox_val.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        inbox_val.push_back(static_cast<uint8_t>((ts >> (i * 8)) & 0xFF));
    uint32_t blob_len = static_cast<uint32_t>(blob.size());
    for (int i = 3; i >= 0; --i)
        inbox_val.push_back(static_cast<uint8_t>((blob_len >> (i * 8)) & 0xFF));
    inbox_val.insert(inbox_val.end(), blob.begin(), blob.end());

    storage_->put(storage::TABLE_INBOXES, inbox_key, inbox_val);

    // FETCH
    ASSERT_TRUE(client.send_text(R"({"type":"FETCH","id":3})"));

    auto resp = client.recv_text();
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    EXPECT_EQ(root["type"].asString(), "MESSAGES");
    EXPECT_EQ(root["messages"].size(), 1u);
    EXPECT_EQ(root["messages"][0]["timestamp"].asUInt64(), 1708000100u);
}
```

**Step 2: Run test to verify it fails**

Expected: "not yet implemented: FETCH"

**Step 3: Implement FETCH handler**

Add `handle_fetch` to `on_message()` dispatch:

```cpp
    } else if (type == "FETCH") {
        if (!require_auth(ws, id)) return;
        handle_fetch(ws, root);
```

Implement:

```cpp
void WsServer::handle_fetch(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();
    auto* session = ws->getUserData();

    uint64_t since = 0;
    if (msg.isMember("since")) {
        since = msg["since"].asUInt64();
    }

    auto to_hex = [](std::span<const uint8_t> bytes) {
        std::string hex;
        for (auto b : bytes) {
            char buf[3]; snprintf(buf, sizeof(buf), "%02x", b); hex += buf;
        }
        return hex;
    };

    Json::Value messages(Json::arrayValue);

    storage_.scan(storage::TABLE_INBOXES, session->fingerprint,
                  [&](std::span<const uint8_t> key, std::span<const uint8_t> value) {
        // Key: recipient_fp(32) || timestamp(8 BE) || msg_id(32)
        if (key.size() < 72) return true;

        uint64_t ts = 0;
        for (int i = 0; i < 8; ++i)
            ts = (ts << 8) | key[32 + i];

        if (ts < since) return true;  // skip older messages

        // Value: msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4) || blob
        if (value.size() < 76) return true;

        auto msg_id = value.subspan(0, 32);
        auto sender = value.subspan(32, 32);

        uint32_t blob_len = 0;
        for (int i = 0; i < 4; ++i)
            blob_len = (blob_len << 8) | value[72 + i];

        if (76 + blob_len > value.size()) return true;
        auto blob = value.subspan(76, blob_len);

        // Base64 encode blob (simple implementation)
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64_blob;
        for (size_t i = 0; i < blob.size(); i += 3) {
            uint32_t n = static_cast<uint32_t>(blob[i]) << 16;
            if (i + 1 < blob.size()) n |= static_cast<uint32_t>(blob[i + 1]) << 8;
            if (i + 2 < blob.size()) n |= static_cast<uint32_t>(blob[i + 2]);
            b64_blob += b64[(n >> 18) & 0x3F];
            b64_blob += b64[(n >> 12) & 0x3F];
            b64_blob += (i + 1 < blob.size()) ? b64[(n >> 6) & 0x3F] : '=';
            b64_blob += (i + 2 < blob.size()) ? b64[n & 0x3F] : '=';
        }

        Json::Value m;
        m["msg_id"] = to_hex(msg_id);
        m["from"] = to_hex(sender);
        m["blob"] = b64_blob;
        m["timestamp"] = static_cast<Json::UInt64>(ts);
        messages.append(m);

        return true;
    });

    Json::Value resp;
    resp["type"] = "MESSAGES";
    resp["id"] = id;
    resp["messages"] = messages;
    send_json(ws, resp);
}
```

**Step 4: Run test to verify it passes**

Run: `./tests/chromatin-tests --gtest_filter="WsServer*Fetch*"`
Expected: PASS

**Step 5: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: WS FETCH command — retrieve inbox messages via prefix scan"
```

---

### Task 7: SEND command

Store a message in a recipient's inbox via the worker pool. The Kademlia store runs on a worker thread; the SEND_ACK is deferred back to the uWS thread.

**Files:**
- Modify: `src/ws/ws_server.cpp` — implement handle_send
- Modify: `tests/test_ws_server.cpp` — add SEND test

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, SendAndFetch) {
    start_ws_server();

    // Create sender and recipient
    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    auto to_hex = [](const auto& bytes) {
        std::string hex;
        for (auto b : bytes) {
            char buf[3]; snprintf(buf, sizeof(buf), "%02x", b); hex += buf;
        }
        return hex;
    };

    // Add sender to recipient's allowlist manually
    auto allowlist_key = crypto::sha3_256_prefixed("allowlist:", recipient_fp);
    std::vector<uint8_t> allow_entry_key;
    allow_entry_key.insert(allow_entry_key.end(), allowlist_key.begin(), allowlist_key.end());
    allow_entry_key.insert(allow_entry_key.end(), sender_fp.begin(), sender_fp.end());
    std::vector<uint8_t> allow_entry_val = {0x01};  // action=allow
    storage_->put(storage::TABLE_ALLOWLISTS, allow_entry_key, allow_entry_val);

    // Authenticate sender
    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    if (!authenticate(client, sender_kp)) {
        GTEST_SKIP() << "Node not responsible for sender's inbox";
    }

    // SEND a message
    std::string send_msg = R"({"type":"SEND","id":2,"to":")" + to_hex(recipient_fp) +
                           R"(","blob":"aGVsbG8="})";  // base64 "hello"
    ASSERT_TRUE(client.send_text(send_msg));

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    // Should get SEND_ACK or ERROR (depending on responsibility)
    std::string resp_type = root["type"].asString();
    EXPECT_TRUE(resp_type == "SEND_ACK" || resp_type == "ERROR")
        << "Got: " << resp_type;

    if (resp_type == "SEND_ACK") {
        EXPECT_FALSE(root["msg_id"].asString().empty());
    }
}
```

**Step 2: Run test — fails with "not yet implemented: SEND"**

**Step 3: Implement SEND handler**

Add base64 decode helper and `handle_send()`:

```cpp
void WsServer::handle_send(ws_t* ws, const Json::Value& msg) {
    int id = msg.get("id", 0).asInt();
    auto* session = ws->getUserData();

    std::string to_hex_str = msg.get("to", "").asString();
    std::string blob_b64 = msg.get("blob", "").asString();

    if (to_hex_str.size() != 64) {
        send_error(ws, id, 400, "invalid recipient fingerprint");
        return;
    }

    // Parse recipient fingerprint
    crypto::Hash recipient_fp{};
    for (size_t i = 0; i < 32; ++i) {
        recipient_fp[i] = static_cast<uint8_t>(
            std::stoi(to_hex_str.substr(i * 2, 2), nullptr, 16));
    }

    // Decode base64 blob
    auto blob = base64_decode(blob_b64);
    if (blob.size() > 256 * 1024) {
        send_error(ws, id, 413, "blob exceeds 256 KiB");
        return;
    }

    // Generate msg_id and timestamp
    crypto::Hash msg_id{};
    std::random_device rd;
    for (auto& b : msg_id) b = static_cast<uint8_t>(rd());

    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

    // Build inbox message binary: msg_id(32) || sender_fp(32) || timestamp(8) || blob_len(4) || blob
    std::vector<uint8_t> message_binary;
    message_binary.insert(message_binary.end(), msg_id.begin(), msg_id.end());
    message_binary.insert(message_binary.end(),
                          session->fingerprint.begin(), session->fingerprint.end());
    for (int i = 7; i >= 0; --i)
        message_binary.push_back(static_cast<uint8_t>((timestamp >> (i * 8)) & 0xFF));
    uint32_t blob_len = static_cast<uint32_t>(blob.size());
    for (int i = 3; i >= 0; --i)
        message_binary.push_back(static_cast<uint8_t>((blob_len >> (i * 8)) & 0xFF));
    message_binary.insert(message_binary.end(), blob.begin(), blob.end());

    // Compute inbox key
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", recipient_fp);

    // Dispatch to worker pool (kademlia.store is blocking)
    auto msg_id_copy = msg_id;
    workers_.post([this, ws, id, inbox_key, message_binary = std::move(message_binary),
                   msg_id_copy]() {
        bool ok = kad_.store(inbox_key, 0x02, message_binary);

        loop_->defer([this, ws, id, ok, msg_id_copy]() {
            // Check if WS is still connected
            if (authenticated_.empty()) return;  // rough check

            auto to_hex = [](const crypto::Hash& h) {
                std::string hex;
                for (auto b : h) {
                    char buf[3]; snprintf(buf, sizeof(buf), "%02x", b); hex += buf;
                }
                return hex;
            };

            if (ok) {
                Json::Value ack;
                ack["type"] = "SEND_ACK";
                ack["id"] = id;
                ack["msg_id"] = to_hex(msg_id_copy);
                send_json(ws, ack);
            } else {
                send_error(ws, id, 500, "store failed");
            }
        });
    });
}
```

Add base64 decode helper (private in ws_server.cpp or as a static function):

```cpp
static std::vector<uint8_t> base64_decode(const std::string& encoded) {
    static const uint8_t lookup[256] = {
        // ... standard base64 decode table
    };
    // ... standard implementation
}
```

**Notes for implementation:**
- The `ws` pointer captured in the worker lambda is used after `defer()` — this is safe because `defer()` runs on the uWS thread where `ws` is valid. However, we need to check if the connection is still open. Track this via the `authenticated_` map or a `std::unordered_set<ws_t*> connections_`.
- Add a `std::unordered_set<ws_t*> connections_` member to track all open connections. Check membership in deferred callbacks before using `ws`.

**Step 4: Run test**

**Step 5: Commit**

```bash
git add src/ws/ws_server.h src/ws/ws_server.cpp tests/test_ws_server.cpp
git commit -m "feat: WS SEND command — store inbox messages via worker pool"
```

---

### Task 8: ALLOW + REVOKE commands

Signed allowlist management. The client signs `action || allowed_fp || sequence` and the node verifies.

**Files:**
- Modify: `src/ws/ws_server.cpp` — implement handle_allow, handle_revoke
- Modify: `tests/test_ws_server.cpp` — add ALLOW/REVOKE tests

**Step 1: Write the failing test**

```cpp
TEST_F(WsServerTest, AllowAndRevoke) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);
    auto contact_kp = crypto::generate_keypair();
    auto contact_fp = crypto::sha3_256(contact_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    if (!authenticate(client, user_kp)) {
        GTEST_SKIP() << "Node not responsible";
    }

    auto to_hex = [](const auto& bytes) {
        std::string hex;
        for (auto b : bytes) {
            char buf[3]; snprintf(buf, sizeof(buf), "%02x", b); hex += buf;
        }
        return hex;
    };

    // Build signature: action(0x01) || allowed_fp(32) || sequence(8 BE)
    std::vector<uint8_t> signed_data;
    signed_data.push_back(0x01);  // action = allow
    signed_data.insert(signed_data.end(), contact_fp.begin(), contact_fp.end());
    uint64_t seq = 1;
    for (int i = 7; i >= 0; --i)
        signed_data.push_back(static_cast<uint8_t>((seq >> (i * 8)) & 0xFF));

    auto sig = crypto::sign(signed_data, user_kp.secret_key);

    std::string allow_msg = R"({"type":"ALLOW","id":4,"fingerprint":")" + to_hex(contact_fp) +
                            R"(","sequence":1,"signature":")" + to_hex(sig) + R"("})";
    ASSERT_TRUE(client.send_text(allow_msg));

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    EXPECT_TRUE(root["type"].asString() == "OK" || root["type"].asString() == "ERROR");
}
```

**Steps 2-5:** Implement `handle_allow()` and `handle_revoke()` following the same worker pool dispatch pattern as SEND. Build the allowlist entry binary per PROTOCOL-SPEC.md section 3 (data_type 0x04), call `kademlia.store()` on the worker thread, defer OK response back to uWS thread.

Key implementation detail for allowlist storage key: `SHA3-256("allowlist:" || owner_fp)`.

Commit: `git commit -m "feat: WS ALLOW/REVOKE — signed allowlist management"`

---

### Task 9: CONTACT_REQUEST + Push notifications

Two pieces: (1) CONTACT_REQUEST command with PoW verification, (2) push notifications when STORE arrives for a connected client's inbox.

**Files:**
- Modify: `src/ws/ws_server.cpp` — handle_contact_request, on_kademlia_store
- Modify: `tests/test_ws_server.cpp` — add tests

**Step 1: Write the failing tests**

```cpp
TEST_F(WsServerTest, ContactRequestWithPoW) {
    start_ws_server();

    auto sender_kp = crypto::generate_keypair();
    auto recipient_kp = crypto::generate_keypair();
    auto sender_fp = crypto::sha3_256(sender_kp.public_key);
    auto recipient_fp = crypto::sha3_256(recipient_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    if (!authenticate(client, sender_kp)) {
        GTEST_SKIP() << "Node not responsible";
    }

    // Compute PoW: SHA3-256("request:" || sender_fp || recipient_fp || nonce) >= 16 zero bits
    // For testing, use a low difficulty or find a valid nonce
    std::vector<uint8_t> pow_preimage;
    std::string prefix = "request:";
    pow_preimage.insert(pow_preimage.end(), prefix.begin(), prefix.end());
    pow_preimage.insert(pow_preimage.end(), sender_fp.begin(), sender_fp.end());
    pow_preimage.insert(pow_preimage.end(), recipient_fp.begin(), recipient_fp.end());

    uint64_t nonce = 0;
    while (true) {
        if (crypto::verify_pow(pow_preimage, nonce, 16)) break;
        nonce++;
        if (nonce > 1000000) {
            GTEST_SKIP() << "Could not find PoW nonce in time";
        }
    }

    auto to_hex = [](const auto& bytes) {
        std::string hex;
        for (auto b : bytes) {
            char buf[3]; snprintf(buf, sizeof(buf), "%02x", b); hex += buf;
        }
        return hex;
    };

    std::string cr = R"({"type":"CONTACT_REQUEST","id":6,"to":")" + to_hex(recipient_fp) +
                     R"(","blob":"aGVsbG8=","pow_nonce":)" + std::to_string(nonce) + "}";
    ASSERT_TRUE(client.send_text(cr));

    auto resp = client.recv_text(5000);
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    EXPECT_TRUE(root["type"].asString() == "OK" || root["type"].asString() == "ERROR");
}

TEST_F(WsServerTest, PushNotification) {
    start_ws_server();

    auto user_kp = crypto::generate_keypair();
    auto user_fp = crypto::sha3_256(user_kp.public_key);

    TestWsClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", ws_port_));
    if (!authenticate(client, user_kp)) {
        GTEST_SKIP() << "Node not responsible";
    }

    // Simulate an inbox store arriving via Kademlia (as if another node forwarded it)
    auto inbox_key = crypto::sha3_256_prefixed("inbox:", user_fp);

    crypto::Hash msg_id{};
    msg_id[0] = 0xAA;
    crypto::Hash sender_fp{};
    sender_fp[0] = 0xBB;
    uint64_t ts = 1708001000;
    std::vector<uint8_t> blob = {'t', 'e', 's', 't'};

    std::vector<uint8_t> msg_binary;
    msg_binary.insert(msg_binary.end(), msg_id.begin(), msg_id.end());
    msg_binary.insert(msg_binary.end(), sender_fp.begin(), sender_fp.end());
    for (int i = 7; i >= 0; --i)
        msg_binary.push_back(static_cast<uint8_t>((ts >> (i * 8)) & 0xFF));
    uint32_t blen = static_cast<uint32_t>(blob.size());
    for (int i = 3; i >= 0; --i)
        msg_binary.push_back(static_cast<uint8_t>((blen >> (i * 8)) & 0xFF));
    msg_binary.insert(msg_binary.end(), blob.begin(), blob.end());

    // Trigger the on_store callback (simulates Kademlia delivering a message)
    server_->on_kademlia_store(inbox_key, 0x02, msg_binary);

    // Should receive NEW_MESSAGE push
    auto resp = client.recv_text(2000);
    ASSERT_TRUE(resp.has_value());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream s(*resp);
    ASSERT_TRUE(Json::parseFromStream(builder, s, &root, &errs));

    EXPECT_EQ(root["type"].asString(), "NEW_MESSAGE");
    EXPECT_EQ(root["timestamp"].asUInt64(), 1708001000u);
}
```

**Steps 2-5:** Implement `handle_contact_request()` (verify PoW, verify blob <= 64 KiB, store via worker pool) and `on_kademlia_store()` (check if data_type is inbox message 0x02 or contact request 0x03, extract recipient fingerprint from inbox key, check authenticated_ map, if connected send NEW_MESSAGE or CONTACT_REQUEST push).

Key implementation for `on_kademlia_store()`:

```cpp
void WsServer::on_kademlia_store(const crypto::Hash& key,
                                  uint8_t data_type,
                                  std::span<const uint8_t> value) {
    if (data_type != 0x02 && data_type != 0x03) return;

    // Copy value for the deferred lambda
    std::vector<uint8_t> value_copy(value.begin(), value.end());
    auto key_copy = key;

    loop_->defer([this, key_copy, data_type, value_copy = std::move(value_copy)]() {
        // Find which connected client this inbox belongs to
        for (auto& [fp, ws] : authenticated_) {
            auto inbox_key = crypto::sha3_256_prefixed("inbox:", fp);
            auto request_key = crypto::sha3_256_prefixed("requests:", fp);

            if ((data_type == 0x02 && key_copy == inbox_key) ||
                (data_type == 0x03 && key_copy == request_key)) {
                // Parse message and push
                // ... build NEW_MESSAGE or CONTACT_REQUEST JSON
                // send_json(ws, push_msg);
                break;
            }
        }
    });
}
```

Commit: `git commit -m "feat: WS CONTACT_REQUEST + push notifications"`

---

### Task 10: main.cpp integration + Docker test

Replace the main loop with WsServer, verify everything works end-to-end.

**Files:**
- Modify: `src/main.cpp` — replace sleep loop with ws.run()
- Modify: `docker/docker-compose.yml` or test script — add WS test
- Create: `tests/integration/ws_integration_test.sh` (optional)

**Step 1: Update main.cpp**

Replace the current main loop (lines 136-140):

```cpp
    // Old:
    // while (g_running.load()) {
    //     kademlia.tick();
    //     std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // }

    // New: WebSocket server is the main event loop
    chromatin::ws::WsServer ws(cfg, kademlia, storage, repl_log, keypair);
    kademlia.set_on_store([&](const chromatin::crypto::Hash& key, uint8_t type,
                              std::span<const uint8_t> value) {
        ws.on_kademlia_store(key, type, value);
    });

    // Signal handler stops the WS event loop
    // (need to capture ws pointer for signal handler — use a global or callback)
    static chromatin::ws::WsServer* g_ws = &ws;
    sa.sa_handler = [](int) { g_ws->stop(); };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    spdlog::info("node ready — WS on port {}, TCP on port {}", cfg.ws_port, cfg.tcp_port);
    ws.run();  // blocks until signal
```

Add `#include "ws/ws_server.h"` to main.cpp.

Update shutdown:

```cpp
    spdlog::info("shutting down...");
    transport.stop();
    recv_thread.join();
    spdlog::info("chromatin-node shutdown");
```

**Step 2: Build and verify**

```bash
cd /home/mika/dev/dna-helix/build && cmake .. && make -j$(nproc) chromatin-node
```

**Step 3: Docker integration test**

Run the existing Docker tests to verify nothing is broken:

```bash
cd /home/mika/dev/dna-helix && docker compose -f docker/docker-compose.yml build
docker compose -f docker/docker-compose.yml run --rm test
```

All 5 existing tests should still pass. The nodes now start with WebSocket server, but the existing TCP-based tests are unaffected.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: main.cpp — WebSocket server as main event loop"
```

**Step 5: Final verification**

Run all unit tests:
```bash
cd /home/mika/dev/dna-helix/build && make -j$(nproc) chromatin-tests && ./tests/chromatin-tests
```

Run Docker integration:
```bash
cd /home/mika/dev/dna-helix && docker compose -f docker/docker-compose.yml run --rm test
```

**Final commit:**
```bash
git commit -m "feat: WebSocket server — complete MVP implementation"
```

---

## Dependency Graph

```
Task 1 (WorkerPool)  ──┐
Task 2 (scan)        ──┼──> Task 4 (skeleton) ──> Task 5 (auth) ──> Task 6 (FETCH)
Task 3 (on_store)    ──┘                                         ──> Task 7 (SEND)
                                                                 ──> Task 8 (ALLOW/REVOKE)
                                                                 ──> Task 9 (CONTACT_REQ + push)
                                                                 ──> Task 10 (main.cpp + Docker)
```

Tasks 1-3 are independent and can be parallelized.
Tasks 6-9 depend on Task 5 (auth flow) but are independent of each other.
Task 10 depends on all prior tasks.

## Notes for Implementor

- **uWS event loop must never block.** All `kademlia.store()` calls go through the worker pool. Only `storage.scan()` (FETCH) runs on the uWS thread — it's a local read, not a network round-trip.
- **Thread safety:** `authenticated_` map and `send_json()` are only accessed on the uWS thread. Worker pool results are posted back via `loop_->defer()`.
- **Connection validity:** When a deferred callback fires, the `ws` pointer may be stale (client disconnected). Track open connections in a `std::unordered_set<ws_t*>` and check membership before using `ws`.
- **Hex encoding:** Consider extracting a `to_hex()` / `from_hex()` utility to avoid repeating the conversion code.
- **Base64:** The implementation needs a proper base64 encoder/decoder. Consider a small standalone implementation (~30 lines each) in a utility header.
- **Single-node testing:** With only one node, `is_responsible()` always returns true. Tests with REDIRECT behavior need a multi-node setup (Docker integration).
- **Fix warnings:** The CLAUDE.md says "Always fix warnings after you see them when compiling code." Address any compiler warnings immediately after each build step.
