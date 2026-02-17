# Helix C++ Server — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a working helix-node server binary with Kademlia DHT, mdbx storage, replication, and WebSocket client interface.

**Architecture:** Layer-by-layer bottom-up: crypto → storage → kademlia (NodeId, RoutingTable, UdpTransport, Engine) → replication → WebSocket server → main wiring. Each layer independently testable. Single binary, two threads (UDP + WS event loop).

**Tech Stack:** C++20, CMake + FetchContent, liboqs (ML-DSA-87, SHA3-256), libmdbx (C++ API), uWebSockets + uSockets, jsoncpp, spdlog, GoogleTest.

---

## Task 1: Project Scaffolding

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/CMakeLists.txt`
- Create: `src/main.cpp`
- Create: `tests/CMakeLists.txt`

**Step 1: Create top-level CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(helix-node VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- liboqs (PQ crypto) ---
FetchContent_Declare(
    liboqs
    GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
    GIT_TAG        0.12.0
    GIT_SHALLOW    ON
)
set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(liboqs)

# --- libmdbx (storage) ---
FetchContent_Declare(
    libmdbx
    GIT_REPOSITORY https://github.com/erthink/libmdbx.git
    GIT_TAG        v0.13.3
    GIT_SHALLOW    ON
)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_CXX ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libmdbx)

# --- spdlog (logging) ---
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.0
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(spdlog)

# --- jsoncpp (JSON) ---
FetchContent_Declare(
    jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG        1.9.6
    GIT_SHALLOW    ON
)
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(jsoncpp)

# --- uSockets (networking, no SSL) ---
FetchContent_Declare(
    usockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG        v0.8.8
    GIT_SHALLOW    ON
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(usockets)

file(GLOB USOCKETS_SRC
    ${usockets_SOURCE_DIR}/src/*.c
    ${usockets_SOURCE_DIR}/src/eventing/*.c
    ${usockets_SOURCE_DIR}/src/crypto/*.c
)
add_library(uSockets STATIC ${USOCKETS_SRC})
target_include_directories(uSockets PUBLIC ${usockets_SOURCE_DIR}/src)
target_compile_definitions(uSockets
    PUBLIC LIBUS_NO_SSL
    PRIVATE LIBUS_USE_EPOLL
)

# --- uWebSockets (header-only WebSocket) ---
FetchContent_Declare(
    uwebsockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG        v20.74.0
    GIT_SHALLOW    ON
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(uwebsockets)

add_library(uWebSockets INTERFACE)
target_include_directories(uWebSockets INTERFACE ${uwebsockets_SOURCE_DIR}/src)
target_compile_definitions(uWebSockets INTERFACE UWS_NO_ZLIB)
target_link_libraries(uWebSockets INTERFACE uSockets)

# --- GoogleTest ---
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
```

**Step 2: Create src/CMakeLists.txt**

```cmake
add_library(helix-core STATIC)
target_sources(helix-core PRIVATE
    # Added incrementally per task
)
target_include_directories(helix-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(helix-core PUBLIC
    oqs
    mdbx-static
    jsoncpp_lib
    spdlog::spdlog
    Threads::Threads
)

add_executable(helix-node main.cpp)
target_link_libraries(helix-node PRIVATE helix-core)
```

**Step 3: Create src/main.cpp**

```cpp
#include <spdlog/spdlog.h>

int main(int argc, char* argv[]) {
    spdlog::info("helix-node v0.1.0 starting");
    spdlog::info("helix-node shutdown");
    return 0;
}
```

**Step 4: Create tests/CMakeLists.txt**

```cmake
add_executable(helix-tests)
target_sources(helix-tests PRIVATE
    # Added incrementally per task
)
target_link_libraries(helix-tests PRIVATE
    helix-core
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(helix-tests)
```

**Step 5: Build and verify**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
./build/src/helix-node
```

Expected: prints startup/shutdown log lines.

**Step 6: Commit**

```bash
git add CMakeLists.txt src/ tests/
git commit -m "feat: project scaffolding with CMake + FetchContent deps"
```

---

## Task 2: Crypto Layer

**Files:**
- Create: `src/crypto/crypto.h`
- Create: `src/crypto/crypto.cpp`
- Create: `tests/test_crypto.cpp`
- Modify: `src/CMakeLists.txt` — add crypto sources
- Modify: `tests/CMakeLists.txt` — add test source

**Step 1: Write src/crypto/crypto.h**

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace helix::crypto {

using Hash = std::array<uint8_t, 32>;

// --- SHA3-256 ---
Hash sha3_256(std::span<const uint8_t> data);
Hash sha3_256_prefixed(std::string_view prefix, std::span<const uint8_t> data);

// --- ML-DSA-87 (Dilithium5, FIPS 204 Level 5) ---
inline constexpr size_t PUBLIC_KEY_SIZE = 2592;
inline constexpr size_t SECRET_KEY_SIZE = 4896;
inline constexpr size_t SIGNATURE_SIZE = 4627;

struct KeyPair {
    std::vector<uint8_t> public_key;  // 2592 bytes
    std::vector<uint8_t> secret_key;  // 4896 bytes
};

KeyPair generate_keypair();

std::vector<uint8_t> sign(std::span<const uint8_t> message,
                          std::span<const uint8_t> secret_key);

bool verify(std::span<const uint8_t> message,
            std::span<const uint8_t> signature,
            std::span<const uint8_t> public_key);

// --- PoW ---
// Hashes (preimage || nonce_le) with SHA3-256, checks >= required_zero_bits leading zeros
bool verify_pow(std::span<const uint8_t> preimage, uint64_t nonce, int required_zero_bits);

// Count leading zero bits in a hash
int leading_zero_bits(const Hash& hash);

} // namespace helix::crypto
```

**Step 2: Write tests/test_crypto.cpp**

```cpp
#include "crypto/crypto.h"
#include <gtest/gtest.h>
#include <string>

using namespace helix::crypto;

// --- SHA3-256 ---

TEST(CryptoTest, Sha3_256_EmptyInput) {
    // NIST test vector: SHA3-256("") = a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a
    std::vector<uint8_t> empty;
    Hash h = sha3_256(empty);
    EXPECT_EQ(h[0], 0xa7);
    EXPECT_EQ(h[1], 0xff);
    EXPECT_EQ(h[31], 0x4a);
}

TEST(CryptoTest, Sha3_256_Deterministic) {
    std::string msg = "hello";
    std::vector<uint8_t> data(msg.begin(), msg.end());
    Hash h1 = sha3_256(data);
    Hash h2 = sha3_256(data);
    EXPECT_EQ(h1, h2);
}

TEST(CryptoTest, Sha3_256_DifferentInputsDifferentOutputs) {
    std::string a = "hello";
    std::string b = "world";
    std::vector<uint8_t> da(a.begin(), a.end()), db(b.begin(), b.end());
    EXPECT_NE(sha3_256(da), sha3_256(db));
}

TEST(CryptoTest, Sha3_256_Prefixed) {
    // sha3_256_prefixed("dna:", data) should equal sha3_256("dna:" || data)
    std::string raw = "testdata";
    std::vector<uint8_t> data(raw.begin(), raw.end());

    Hash prefixed = sha3_256_prefixed("dna:", data);

    std::string combined = "dna:" + raw;
    std::vector<uint8_t> combined_data(combined.begin(), combined.end());
    Hash manual = sha3_256(combined_data);

    EXPECT_EQ(prefixed, manual);
}

// --- ML-DSA-87 ---

TEST(CryptoTest, GenerateKeypair) {
    auto kp = generate_keypair();
    EXPECT_EQ(kp.public_key.size(), PUBLIC_KEY_SIZE);
    EXPECT_EQ(kp.secret_key.size(), SECRET_KEY_SIZE);
}

TEST(CryptoTest, SignVerifyRoundTrip) {
    auto kp = generate_keypair();
    std::string msg = "sign this message";
    std::vector<uint8_t> data(msg.begin(), msg.end());

    auto sig = sign(data, kp.secret_key);
    EXPECT_EQ(sig.size(), SIGNATURE_SIZE);
    EXPECT_TRUE(verify(data, sig, kp.public_key));
}

TEST(CryptoTest, VerifyRejectsWrongMessage) {
    auto kp = generate_keypair();
    std::string msg = "original";
    std::vector<uint8_t> data(msg.begin(), msg.end());
    auto sig = sign(data, kp.secret_key);

    std::string tampered = "tampered";
    std::vector<uint8_t> bad(tampered.begin(), tampered.end());
    EXPECT_FALSE(verify(bad, sig, kp.public_key));
}

TEST(CryptoTest, VerifyRejectsWrongKey) {
    auto kp1 = generate_keypair();
    auto kp2 = generate_keypair();
    std::string msg = "message";
    std::vector<uint8_t> data(msg.begin(), msg.end());
    auto sig = sign(data, kp1.secret_key);

    EXPECT_FALSE(verify(data, sig, kp2.public_key));
}

// --- PoW ---

TEST(CryptoTest, LeadingZeroBits) {
    Hash h = {};
    h.fill(0);
    EXPECT_EQ(leading_zero_bits(h), 256);

    h[0] = 0x01; // 00000001 -> 7 leading zeros in first byte, rest doesn't matter
    EXPECT_EQ(leading_zero_bits(h), 7);

    h[0] = 0x00;
    h[1] = 0x0F; // 00001111 -> 8 + 4 = 12
    EXPECT_EQ(leading_zero_bits(h), 12);

    h[0] = 0x80; // 10000000 -> 0
    EXPECT_EQ(leading_zero_bits(h), 0);
}

TEST(CryptoTest, VerifyPow) {
    // Brute-force a nonce with >= 8 leading zero bits (easy, fast)
    std::string pre = "test_pow_preimage";
    std::vector<uint8_t> preimage(pre.begin(), pre.end());

    uint64_t nonce = 0;
    while (true) {
        if (verify_pow(preimage, nonce, 8)) break;
        nonce++;
        ASSERT_LT(nonce, 100000u) << "Should find 8-bit PoW quickly";
    }
    // Verify the found nonce works
    EXPECT_TRUE(verify_pow(preimage, nonce, 8));
    // And nonce-1 likely doesn't (unless we got lucky at 0)
    if (nonce > 0) {
        // This might pass by chance, but very unlikely
        // Just verify the found nonce is consistent
        EXPECT_TRUE(verify_pow(preimage, nonce, 8));
    }
}
```

**Step 3: Run tests to verify they fail**

```bash
cmake --build build -j$(nproc) 2>&1
```

Expected: link errors (crypto functions not defined).

**Step 4: Write src/crypto/crypto.cpp**

```cpp
#include "crypto/crypto.h"

#include <oqs/oqs.h>
#include <cstring>
#include <stdexcept>

namespace helix::crypto {

Hash sha3_256(std::span<const uint8_t> data) {
    Hash result;
    OQS_SHA3_sha3_256(result.data(), data.data(), data.size());
    return result;
}

Hash sha3_256_prefixed(std::string_view prefix, std::span<const uint8_t> data) {
    Hash result;
    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);
    OQS_SHA3_sha3_256_inc_absorb(&ctx,
        reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size());
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());
    OQS_SHA3_sha3_256_inc_finalize(result.data(), &ctx);
    return result;
}

KeyPair generate_keypair() {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) throw std::runtime_error("failed to init ML-DSA-87");

    KeyPair kp;
    kp.public_key.resize(sig->length_public_key);
    kp.secret_key.resize(sig->length_secret_key);

    if (OQS_SIG_keypair(sig, kp.public_key.data(), kp.secret_key.data()) != OQS_SUCCESS) {
        OQS_SIG_free(sig);
        throw std::runtime_error("ML-DSA-87 keypair generation failed");
    }
    OQS_SIG_free(sig);
    return kp;
}

std::vector<uint8_t> sign(std::span<const uint8_t> message,
                          std::span<const uint8_t> secret_key) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) throw std::runtime_error("failed to init ML-DSA-87");

    std::vector<uint8_t> signature(sig->length_signature);
    size_t sig_len = 0;

    OQS_STATUS rc = OQS_SIG_sign(sig, signature.data(), &sig_len,
                                  message.data(), message.size(),
                                  secret_key.data());
    OQS_SIG_free(sig);
    if (rc != OQS_SUCCESS) throw std::runtime_error("ML-DSA-87 signing failed");

    signature.resize(sig_len);
    return signature;
}

bool verify(std::span<const uint8_t> message,
            std::span<const uint8_t> signature,
            std::span<const uint8_t> public_key) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) return false;

    OQS_STATUS rc = OQS_SIG_verify(sig, message.data(), message.size(),
                                    signature.data(), signature.size(),
                                    public_key.data());
    OQS_SIG_free(sig);
    return rc == OQS_SUCCESS;
}

int leading_zero_bits(const Hash& hash) {
    int bits = 0;
    for (uint8_t byte : hash) {
        if (byte == 0) {
            bits += 8;
        } else {
            bits += __builtin_clz(static_cast<unsigned int>(byte)) - 24;
            break;
        }
    }
    return bits;
}

bool verify_pow(std::span<const uint8_t> preimage, uint64_t nonce, int required_zero_bits) {
    // Build buffer: preimage || nonce (little-endian 8 bytes)
    std::vector<uint8_t> buf(preimage.begin(), preimage.end());
    for (int i = 0; i < 8; i++) {
        buf.push_back(static_cast<uint8_t>(nonce >> (i * 8)));
    }
    Hash hash = sha3_256(buf);
    return leading_zero_bits(hash) >= required_zero_bits;
}

} // namespace helix::crypto
```

**Step 5: Update src/CMakeLists.txt — add crypto sources**

Add to `target_sources(helix-core PRIVATE ...)`:
```cmake
    crypto/crypto.cpp
```

**Step 6: Update tests/CMakeLists.txt — add test source**

Add to `target_sources(helix-tests PRIVATE ...)`:
```cmake
    test_crypto.cpp
```

**Step 7: Build and run tests**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

Expected: all crypto tests pass.

**Step 8: Commit**

```bash
git add src/crypto/ tests/test_crypto.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: crypto layer — SHA3-256, ML-DSA-87, PoW verification"
```

---

## Task 3: Storage Layer

**Files:**
- Create: `src/storage/storage.h`
- Create: `src/storage/storage.cpp`
- Create: `tests/test_storage.cpp`
- Modify: `src/CMakeLists.txt` — add storage sources
- Modify: `tests/CMakeLists.txt` — add test source

**Step 1: Write src/storage/storage.h**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mdbx.h++"

namespace helix::storage {

// All table names used by the server
inline constexpr const char* TABLE_PROFILES   = "profiles";
inline constexpr const char* TABLE_NAMES      = "names";
inline constexpr const char* TABLE_INBOXES    = "inboxes";
inline constexpr const char* TABLE_REQUESTS   = "requests";
inline constexpr const char* TABLE_ALLOWLISTS = "allowlists";
inline constexpr const char* TABLE_REPL_LOG   = "repl_log";
inline constexpr const char* TABLE_NODES      = "nodes";
inline constexpr const char* TABLE_REPUTATION = "reputation";

class Storage {
public:
    explicit Storage(const std::filesystem::path& db_path);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    bool put(std::string_view table, std::span<const uint8_t> key,
             std::span<const uint8_t> value);
    std::optional<std::vector<uint8_t>> get(std::string_view table,
                                             std::span<const uint8_t> key) const;
    bool del(std::string_view table, std::span<const uint8_t> key);

    // Iterate all key-value pairs in a table. Return false from callback to stop.
    using Callback = std::function<bool(std::span<const uint8_t> key,
                                         std::span<const uint8_t> value)>;
    void foreach(std::string_view table, Callback cb) const;

private:
    mdbx::env_managed env_;
    std::unordered_map<std::string, mdbx::map_handle> maps_;

    mdbx::map_handle get_map(std::string_view table) const;
};

} // namespace helix::storage
```

**Step 2: Write tests/test_storage.cpp**

```cpp
#include "storage/storage.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <vector>
#include <cstdlib>

using namespace helix::storage;

class StorageTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<Storage> store_;

    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("helix_test_" + std::to_string(getpid()) + "_" +
                    std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(db_path_);
        store_ = std::make_unique<Storage>(db_path_);
    }

    void TearDown() override {
        store_.reset();
        std::filesystem::remove_all(db_path_);
    }

    static std::vector<uint8_t> to_bytes(std::string_view s) {
        return {s.begin(), s.end()};
    }
};

TEST_F(StorageTest, PutAndGet) {
    auto key = to_bytes("key1");
    auto value = to_bytes("value1");
    EXPECT_TRUE(store_->put(TABLE_PROFILES, key, value));

    auto result = store_->get(TABLE_PROFILES, key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, value);
}

TEST_F(StorageTest, GetMissingKey) {
    auto key = to_bytes("nonexistent");
    auto result = store_->get(TABLE_PROFILES, key);
    EXPECT_FALSE(result.has_value());
}

TEST_F(StorageTest, PutOverwrites) {
    auto key = to_bytes("key1");
    auto v1 = to_bytes("first");
    auto v2 = to_bytes("second");
    store_->put(TABLE_PROFILES, key, v1);
    store_->put(TABLE_PROFILES, key, v2);

    auto result = store_->get(TABLE_PROFILES, key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, v2);
}

TEST_F(StorageTest, Delete) {
    auto key = to_bytes("key1");
    auto value = to_bytes("value1");
    store_->put(TABLE_PROFILES, key, value);
    EXPECT_TRUE(store_->del(TABLE_PROFILES, key));
    EXPECT_FALSE(store_->get(TABLE_PROFILES, key).has_value());
}

TEST_F(StorageTest, DeleteMissingKey) {
    auto key = to_bytes("nonexistent");
    EXPECT_FALSE(store_->del(TABLE_PROFILES, key));
}

TEST_F(StorageTest, SeparateTables) {
    auto key = to_bytes("same_key");
    auto v1 = to_bytes("profile_data");
    auto v2 = to_bytes("name_data");
    store_->put(TABLE_PROFILES, key, v1);
    store_->put(TABLE_NAMES, key, v2);

    EXPECT_EQ(*store_->get(TABLE_PROFILES, key), v1);
    EXPECT_EQ(*store_->get(TABLE_NAMES, key), v2);
}

TEST_F(StorageTest, Foreach) {
    store_->put(TABLE_NODES, to_bytes("a"), to_bytes("1"));
    store_->put(TABLE_NODES, to_bytes("b"), to_bytes("2"));
    store_->put(TABLE_NODES, to_bytes("c"), to_bytes("3"));

    int count = 0;
    store_->foreach(TABLE_NODES, [&](auto key, auto value) {
        count++;
        return true; // continue
    });
    EXPECT_EQ(count, 3);
}

TEST_F(StorageTest, ForeachEarlyStop) {
    store_->put(TABLE_NODES, to_bytes("a"), to_bytes("1"));
    store_->put(TABLE_NODES, to_bytes("b"), to_bytes("2"));
    store_->put(TABLE_NODES, to_bytes("c"), to_bytes("3"));

    int count = 0;
    store_->foreach(TABLE_NODES, [&](auto key, auto value) {
        count++;
        return false; // stop after first
    });
    EXPECT_EQ(count, 1);
}
```

**Step 3: Write src/storage/storage.cpp**

```cpp
#include "storage/storage.h"

#include <stdexcept>

namespace helix::storage {

static const std::vector<const char*> ALL_TABLES = {
    TABLE_PROFILES, TABLE_NAMES, TABLE_INBOXES, TABLE_REQUESTS,
    TABLE_ALLOWLISTS, TABLE_REPL_LOG, TABLE_NODES, TABLE_REPUTATION
};

Storage::Storage(const std::filesystem::path& db_path) {
    mdbx::env_managed::create_parameters cp;
    cp.geometry.make_dynamic(1 * mdbx::env::geometry::MiB,
                             4 * mdbx::env::geometry::GiB);

    mdbx::env::operate_parameters op(
        static_cast<unsigned>(ALL_TABLES.size() + 2), // max_maps
        126,                                           // max_readers
        mdbx::env::mode::write_mapped_io,
        mdbx::env::durability::lazy_weak_tail
    );

    env_ = mdbx::env_managed(db_path.string(), cp, op);

    // Create all tables
    auto wtxn = env_.start_write();
    for (const char* name : ALL_TABLES) {
        maps_[name] = wtxn.create_map(name);
    }
    wtxn.commit();
}

Storage::~Storage() {
    env_.close();
}

mdbx::map_handle Storage::get_map(std::string_view table) const {
    auto it = maps_.find(std::string(table));
    if (it == maps_.end()) {
        throw std::runtime_error("unknown table: " + std::string(table));
    }
    return it->second;
}

bool Storage::put(std::string_view table, std::span<const uint8_t> key,
                  std::span<const uint8_t> value) {
    auto map = get_map(table);
    auto wtxn = env_.start_write();
    wtxn.upsert(map, mdbx::slice(key.data(), key.size()),
                     mdbx::slice(value.data(), value.size()));
    wtxn.commit();
    return true;
}

std::optional<std::vector<uint8_t>> Storage::get(std::string_view table,
                                                  std::span<const uint8_t> key) const {
    auto map = get_map(table);
    auto rtxn = env_.start_read();
    try {
        auto val = rtxn.get(map, mdbx::slice(key.data(), key.size()));
        auto ptr = static_cast<const uint8_t*>(val.data());
        return std::vector<uint8_t>(ptr, ptr + val.size());
    } catch (const mdbx::not_found&) {
        return std::nullopt;
    }
}

bool Storage::del(std::string_view table, std::span<const uint8_t> key) {
    auto map = get_map(table);
    auto wtxn = env_.start_write();
    bool erased = wtxn.erase(map, mdbx::slice(key.data(), key.size()));
    wtxn.commit();
    return erased;
}

void Storage::foreach(std::string_view table, Callback cb) const {
    auto map = get_map(table);
    auto rtxn = env_.start_read();
    auto cursor = rtxn.open_cursor(map);

    auto result = cursor.to_first(false);
    while (result) {
        auto k_ptr = static_cast<const uint8_t*>(result.key.data());
        auto v_ptr = static_cast<const uint8_t*>(result.value.data());
        std::span<const uint8_t> k_span(k_ptr, result.key.size());
        std::span<const uint8_t> v_span(v_ptr, result.value.size());

        if (!cb(k_span, v_span)) break;
        result = cursor.to_next(false);
    }
}

} // namespace helix::storage
```

**Step 4: Update CMakeLists, build, and run tests**

Add `storage/storage.cpp` to `src/CMakeLists.txt` and `test_storage.cpp` to `tests/CMakeLists.txt`.

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

Expected: all tests pass (crypto + storage).

**Step 5: Commit**

```bash
git add src/storage/ tests/test_storage.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: storage layer — libmdbx wrapper with 8 named tables"
```

---

## Task 4: NodeId + XOR Distance

**Files:**
- Create: `src/kademlia/node_id.h`
- Create: `src/kademlia/node_id.cpp`
- Create: `tests/test_node_id.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

**Step 1: Write src/kademlia/node_id.h**

```cpp
#pragma once

#include "crypto/crypto.h"
#include <cstddef>
#include <functional>

namespace helix::kademlia {

struct NodeId {
    crypto::Hash id{};

    static NodeId from_pubkey(std::span<const uint8_t> pubkey);

    crypto::Hash distance_to(const NodeId& other) const;

    bool operator==(const NodeId& other) const = default;
    auto operator<=>(const NodeId& other) const = default;
};

// For use in unordered containers
struct NodeIdHash {
    size_t operator()(const NodeId& n) const;
};

} // namespace helix::kademlia
```

**Step 2: Write tests/test_node_id.cpp**

```cpp
#include "kademlia/node_id.h"
#include <gtest/gtest.h>

using namespace helix::kademlia;
using namespace helix::crypto;

TEST(NodeIdTest, FromPubkey) {
    auto kp = generate_keypair();
    NodeId id = NodeId::from_pubkey(kp.public_key);
    Hash expected = sha3_256(kp.public_key);
    EXPECT_EQ(id.id, expected);
}

TEST(NodeIdTest, DistanceToSelfIsZero) {
    auto kp = generate_keypair();
    NodeId id = NodeId::from_pubkey(kp.public_key);
    Hash dist = id.distance_to(id);
    Hash zero{};
    EXPECT_EQ(dist, zero);
}

TEST(NodeIdTest, DistanceIsSymmetric) {
    auto kp1 = generate_keypair();
    auto kp2 = generate_keypair();
    NodeId a = NodeId::from_pubkey(kp1.public_key);
    NodeId b = NodeId::from_pubkey(kp2.public_key);
    EXPECT_EQ(a.distance_to(b), b.distance_to(a));
}

TEST(NodeIdTest, DistanceToOtherIsNonZero) {
    auto kp1 = generate_keypair();
    auto kp2 = generate_keypair();
    NodeId a = NodeId::from_pubkey(kp1.public_key);
    NodeId b = NodeId::from_pubkey(kp2.public_key);
    Hash zero{};
    EXPECT_NE(a.distance_to(b), zero);
}

TEST(NodeIdTest, XorDistanceOrdering) {
    // Create 3 nodes, verify distance ordering is consistent
    auto kp1 = generate_keypair();
    auto kp2 = generate_keypair();
    auto kp3 = generate_keypair();
    NodeId a = NodeId::from_pubkey(kp1.public_key);
    NodeId b = NodeId::from_pubkey(kp2.public_key);
    NodeId c = NodeId::from_pubkey(kp3.public_key);

    // Just verify distances are computed without error and are deterministic
    Hash d_ab = a.distance_to(b);
    Hash d_ac = a.distance_to(c);
    EXPECT_EQ(a.distance_to(b), d_ab);
    EXPECT_EQ(a.distance_to(c), d_ac);
}

TEST(NodeIdTest, Equality) {
    auto kp = generate_keypair();
    NodeId a = NodeId::from_pubkey(kp.public_key);
    NodeId b = NodeId::from_pubkey(kp.public_key);
    EXPECT_EQ(a, b);
}
```

**Step 3: Write src/kademlia/node_id.cpp**

```cpp
#include "kademlia/node_id.h"
#include <cstring>

namespace helix::kademlia {

NodeId NodeId::from_pubkey(std::span<const uint8_t> pubkey) {
    NodeId n;
    n.id = crypto::sha3_256(pubkey);
    return n;
}

crypto::Hash NodeId::distance_to(const NodeId& other) const {
    crypto::Hash dist;
    for (size_t i = 0; i < 32; i++) {
        dist[i] = id[i] ^ other.id[i];
    }
    return dist;
}

size_t NodeIdHash::operator()(const NodeId& n) const {
    size_t result;
    std::memcpy(&result, n.id.data(), sizeof(result));
    return result;
}

} // namespace helix::kademlia
```

**Step 4: Update CMakeLists, build, run tests**

Add `kademlia/node_id.cpp` and `test_node_id.cpp`.

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

**Step 5: Commit**

```bash
git add src/kademlia/ tests/test_node_id.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: NodeId with SHA3-256 identity and XOR distance"
```

---

## Task 5: Routing Table

**Files:**
- Create: `src/kademlia/routing_table.h`
- Create: `src/kademlia/routing_table.cpp`
- Create: `tests/test_routing_table.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

**Step 1: Write src/kademlia/routing_table.h**

```cpp
#pragma once

#include "kademlia/node_id.h"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix::kademlia {

struct NodeInfo {
    NodeId id;
    std::string address;
    uint16_t udp_port = 0;
    uint16_t ws_port = 0;
    std::vector<uint8_t> pubkey;
    std::chrono::steady_clock::time_point last_seen;
};

class RoutingTable {
public:
    void add_or_update(NodeInfo info);
    void remove(const NodeId& id);
    std::optional<NodeInfo> find(const NodeId& id) const;
    std::vector<NodeInfo> all_nodes() const;

    // R closest nodes to a key (sorted by XOR distance, ascending)
    std::vector<NodeInfo> closest_to(const crypto::Hash& key, size_t count) const;

    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<NodeInfo> nodes_;
};

} // namespace helix::kademlia
```

**Step 2: Write tests/test_routing_table.cpp**

```cpp
#include "kademlia/routing_table.h"
#include <gtest/gtest.h>

using namespace helix::kademlia;
using namespace helix::crypto;

static NodeInfo make_node(const std::string& addr, uint16_t port) {
    auto kp = generate_keypair();
    NodeInfo ni;
    ni.id = NodeId::from_pubkey(kp.public_key);
    ni.address = addr;
    ni.udp_port = port;
    ni.ws_port = port + 1000;
    ni.pubkey = kp.public_key;
    ni.last_seen = std::chrono::steady_clock::now();
    return ni;
}

TEST(RoutingTableTest, AddAndFind) {
    RoutingTable rt;
    auto node = make_node("127.0.0.1", 5000);
    NodeId id = node.id;
    rt.add_or_update(std::move(node));

    auto found = rt.find(id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, id);
    EXPECT_EQ(found->udp_port, 5000);
}

TEST(RoutingTableTest, FindMissing) {
    RoutingTable rt;
    auto kp = generate_keypair();
    NodeId id = NodeId::from_pubkey(kp.public_key);
    EXPECT_FALSE(rt.find(id).has_value());
}

TEST(RoutingTableTest, Remove) {
    RoutingTable rt;
    auto node = make_node("127.0.0.1", 5000);
    NodeId id = node.id;
    rt.add_or_update(std::move(node));
    EXPECT_EQ(rt.size(), 1);

    rt.remove(id);
    EXPECT_EQ(rt.size(), 0);
    EXPECT_FALSE(rt.find(id).has_value());
}

TEST(RoutingTableTest, UpdateExisting) {
    RoutingTable rt;
    auto node = make_node("127.0.0.1", 5000);
    NodeId id = node.id;
    auto pubkey = node.pubkey;
    rt.add_or_update(std::move(node));

    NodeInfo updated;
    updated.id = id;
    updated.address = "192.168.1.1";
    updated.udp_port = 6000;
    updated.ws_port = 7000;
    updated.pubkey = pubkey;
    updated.last_seen = std::chrono::steady_clock::now();
    rt.add_or_update(std::move(updated));

    EXPECT_EQ(rt.size(), 1);
    auto found = rt.find(id);
    EXPECT_EQ(found->udp_port, 6000);
    EXPECT_EQ(found->address, "192.168.1.1");
}

TEST(RoutingTableTest, ClosestTo) {
    RoutingTable rt;
    std::vector<NodeId> ids;
    for (int i = 0; i < 10; i++) {
        auto node = make_node("127.0.0.1", 5000 + i);
        ids.push_back(node.id);
        rt.add_or_update(std::move(node));
    }

    // Pick a random key and find 3 closest
    std::string key_str = "test_key_for_lookup";
    std::vector<uint8_t> key_data(key_str.begin(), key_str.end());
    Hash key = sha3_256(key_data);

    auto closest = rt.closest_to(key, 3);
    ASSERT_EQ(closest.size(), 3);

    // Verify they are sorted by distance (ascending)
    for (size_t i = 1; i < closest.size(); i++) {
        Hash d_prev = closest[i-1].id.distance_to(NodeId{key});
        Hash d_curr = closest[i].id.distance_to(NodeId{key});
        EXPECT_LE(d_prev, d_curr);
    }
}

TEST(RoutingTableTest, ClosestToRequestsMoreThanAvailable) {
    RoutingTable rt;
    auto node = make_node("127.0.0.1", 5000);
    rt.add_or_update(std::move(node));

    Hash key{};
    auto closest = rt.closest_to(key, 5);
    EXPECT_EQ(closest.size(), 1);
}

TEST(RoutingTableTest, AllNodes) {
    RoutingTable rt;
    for (int i = 0; i < 5; i++) {
        rt.add_or_update(make_node("127.0.0.1", 5000 + i));
    }
    EXPECT_EQ(rt.all_nodes().size(), 5);
}
```

**Step 3: Write src/kademlia/routing_table.cpp**

```cpp
#include "kademlia/routing_table.h"
#include <algorithm>

namespace helix::kademlia {

void RoutingTable::add_or_update(NodeInfo info) {
    std::lock_guard lock(mutex_);
    for (auto& n : nodes_) {
        if (n.id == info.id) {
            n = std::move(info);
            return;
        }
    }
    nodes_.push_back(std::move(info));
}

void RoutingTable::remove(const NodeId& id) {
    std::lock_guard lock(mutex_);
    std::erase_if(nodes_, [&](const NodeInfo& n) { return n.id == id; });
}

std::optional<NodeInfo> RoutingTable::find(const NodeId& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& n : nodes_) {
        if (n.id == id) return n;
    }
    return std::nullopt;
}

std::vector<NodeInfo> RoutingTable::all_nodes() const {
    std::lock_guard lock(mutex_);
    return nodes_;
}

std::vector<NodeInfo> RoutingTable::closest_to(const crypto::Hash& key, size_t count) const {
    std::lock_guard lock(mutex_);
    std::vector<NodeInfo> sorted = nodes_;
    NodeId target{key};

    std::sort(sorted.begin(), sorted.end(),
        [&target](const NodeInfo& a, const NodeInfo& b) {
            return a.id.distance_to(target) < b.id.distance_to(target);
        });

    if (sorted.size() > count) sorted.resize(count);
    return sorted;
}

size_t RoutingTable::size() const {
    std::lock_guard lock(mutex_);
    return nodes_.size();
}

} // namespace helix::kademlia
```

**Step 4: Update CMakeLists, build, run tests**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

**Step 5: Commit**

```bash
git add src/kademlia/routing_table.* tests/test_routing_table.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: routing table — full membership with closest_to by XOR distance"
```

---

## Task 6: UDP Transport + Message Serialization

**Files:**
- Create: `src/kademlia/udp_transport.h`
- Create: `src/kademlia/udp_transport.cpp`
- Create: `tests/test_udp_transport.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

**Step 1: Write src/kademlia/udp_transport.h**

```cpp
#pragma once

#include "kademlia/node_id.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix::kademlia {

enum class MessageType : uint8_t {
    PING = 0,
    PONG,
    FIND_NODE,
    NODES,
    STORE,
    FIND_VALUE,
    VALUE,
    SYNC_REQ,
    SYNC_RESP
};

struct Message {
    MessageType type;
    NodeId sender;
    std::vector<uint8_t> payload;   // JSON body
    std::vector<uint8_t> signature; // ML-DSA over (type || payload)
};

// Serialize/deserialize (for testing independently of sockets)
std::vector<uint8_t> serialize_message(const Message& msg);
std::optional<Message> deserialize_message(std::span<const uint8_t> data);

// Sign the message (type || payload) and set signature field
void sign_message(Message& msg, std::span<const uint8_t> secret_key);

// Verify the message signature
bool verify_message(const Message& msg, std::span<const uint8_t> public_key);

class UdpTransport {
public:
    UdpTransport(const std::string& bind_addr, uint16_t port);
    ~UdpTransport();

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    void send(const std::string& addr, uint16_t port, const Message& msg);

    using Handler = std::function<void(const Message& msg,
                                       const std::string& from_addr,
                                       uint16_t from_port)>;
    void run(Handler handler); // blocking recv loop
    void stop();

    uint16_t local_port() const { return port_; }

private:
    int sockfd_ = -1;
    uint16_t port_;
    std::atomic<bool> running_{false};
};

} // namespace helix::kademlia
```

**Step 2: Write tests/test_udp_transport.cpp**

Test serialization (pure) and loopback send/recv:

```cpp
#include "kademlia/udp_transport.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace helix::kademlia;
using namespace helix::crypto;

// --- Serialization ---

TEST(UdpTransportTest, SerializeDeserializeRoundTrip) {
    Message msg;
    msg.type = MessageType::PING;
    msg.sender.id.fill(0x42);
    msg.payload = {1, 2, 3, 4, 5};
    msg.signature = {10, 20, 30};

    auto bytes = serialize_message(msg);
    auto restored = deserialize_message(bytes);

    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->type, msg.type);
    EXPECT_EQ(restored->sender.id, msg.sender.id);
    EXPECT_EQ(restored->payload, msg.payload);
    EXPECT_EQ(restored->signature, msg.signature);
}

TEST(UdpTransportTest, DeserializeInvalidData) {
    std::vector<uint8_t> garbage = {0xFF, 0x01};
    auto result = deserialize_message(garbage);
    EXPECT_FALSE(result.has_value());
}

TEST(UdpTransportTest, SignAndVerifyMessage) {
    auto kp = generate_keypair();

    Message msg;
    msg.type = MessageType::STORE;
    msg.sender = NodeId::from_pubkey(kp.public_key);
    msg.payload = {1, 2, 3, 4, 5};

    sign_message(msg, kp.secret_key);
    EXPECT_FALSE(msg.signature.empty());
    EXPECT_TRUE(verify_message(msg, kp.public_key));
}

TEST(UdpTransportTest, VerifyRejectsTamperedPayload) {
    auto kp = generate_keypair();

    Message msg;
    msg.type = MessageType::STORE;
    msg.sender = NodeId::from_pubkey(kp.public_key);
    msg.payload = {1, 2, 3};

    sign_message(msg, kp.secret_key);
    msg.payload[0] = 99; // tamper
    EXPECT_FALSE(verify_message(msg, kp.public_key));
}

// --- Loopback send/recv ---

TEST(UdpTransportTest, SendRecvLoopback) {
    UdpTransport transport("127.0.0.1", 0); // 0 = ephemeral port

    Message received;
    bool got_message = false;

    std::thread recv_thread([&]() {
        transport.run([&](const Message& msg, const std::string& from, uint16_t port) {
            received = msg;
            got_message = true;
            transport.stop();
        });
    });

    // Give recv thread time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Message msg;
    msg.type = MessageType::PING;
    msg.sender.id.fill(0xAB);
    msg.payload = {10, 20, 30};
    msg.signature = {1, 2, 3};

    transport.send("127.0.0.1", transport.local_port(), msg);

    recv_thread.join();
    ASSERT_TRUE(got_message);
    EXPECT_EQ(received.type, MessageType::PING);
    EXPECT_EQ(received.sender.id, msg.sender.id);
    EXPECT_EQ(received.payload, msg.payload);
}
```

**Step 3: Write src/kademlia/udp_transport.cpp**

Wire format: `[1-byte type][32-byte sender_id][4-byte payload_len BE][payload][4-byte sig_len BE][signature]`

```cpp
#include "kademlia/udp_transport.h"
#include "crypto/crypto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace helix::kademlia {

static void write_u32_be(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val >> 24));
    buf.push_back(static_cast<uint8_t>(val >> 16));
    buf.push_back(static_cast<uint8_t>(val >> 8));
    buf.push_back(static_cast<uint8_t>(val));
}

static uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

std::vector<uint8_t> serialize_message(const Message& msg) {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(msg.type));
    buf.insert(buf.end(), msg.sender.id.begin(), msg.sender.id.end());
    write_u32_be(buf, static_cast<uint32_t>(msg.payload.size()));
    buf.insert(buf.end(), msg.payload.begin(), msg.payload.end());
    write_u32_be(buf, static_cast<uint32_t>(msg.signature.size()));
    buf.insert(buf.end(), msg.signature.begin(), msg.signature.end());
    return buf;
}

std::optional<Message> deserialize_message(std::span<const uint8_t> data) {
    // Minimum: 1 (type) + 32 (sender) + 4 (payload_len) + 4 (sig_len) = 41
    if (data.size() < 41) return std::nullopt;

    Message msg;
    size_t pos = 0;

    msg.type = static_cast<MessageType>(data[pos++]);
    std::copy(data.begin() + pos, data.begin() + pos + 32, msg.sender.id.begin());
    pos += 32;

    uint32_t payload_len = read_u32_be(data.data() + pos);
    pos += 4;
    if (pos + payload_len + 4 > data.size()) return std::nullopt;

    msg.payload.assign(data.begin() + pos, data.begin() + pos + payload_len);
    pos += payload_len;

    uint32_t sig_len = read_u32_be(data.data() + pos);
    pos += 4;
    if (pos + sig_len > data.size()) return std::nullopt;

    msg.signature.assign(data.begin() + pos, data.begin() + pos + sig_len);
    return msg;
}

static std::vector<uint8_t> signable_bytes(const Message& msg) {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(msg.type));
    buf.insert(buf.end(), msg.payload.begin(), msg.payload.end());
    return buf;
}

void sign_message(Message& msg, std::span<const uint8_t> secret_key) {
    auto data = signable_bytes(msg);
    msg.signature = crypto::sign(data, secret_key);
}

bool verify_message(const Message& msg, std::span<const uint8_t> public_key) {
    auto data = signable_bytes(msg);
    return crypto::verify(data, msg.signature, public_key);
}

UdpTransport::UdpTransport(const std::string& bind_addr, uint16_t port)
    : port_(port) {
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) throw std::runtime_error("socket() failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr);

    if (bind(sockfd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sockfd_);
        throw std::runtime_error("bind() failed");
    }

    // If ephemeral port (0), read back assigned port
    if (port == 0) {
        socklen_t len = sizeof(addr);
        getsockname(sockfd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }
}

UdpTransport::~UdpTransport() {
    stop();
    if (sockfd_ >= 0) close(sockfd_);
}

void UdpTransport::send(const std::string& addr, uint16_t port, const Message& msg) {
    auto bytes = serialize_message(msg);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, addr.c_str(), &dest.sin_addr);

    sendto(sockfd_, bytes.data(), bytes.size(), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

void UdpTransport::run(Handler handler) {
    running_ = true;
    std::vector<uint8_t> buf(65536);

    while (running_) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);

        // Use poll/select with timeout so we can check running_ periodically
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sockfd_, &fds);
        timeval tv{.tv_sec = 0, .tv_usec = 100000}; // 100ms

        int ready = select(sockfd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        ssize_t n = recvfrom(sockfd_, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) continue;

        auto msg = deserialize_message(std::span(buf.data(), static_cast<size_t>(n)));
        if (!msg) continue;

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, addr_str, sizeof(addr_str));

        handler(*msg, addr_str, ntohs(from.sin_port));
    }
}

void UdpTransport::stop() {
    running_ = false;
}

} // namespace helix::kademlia
```

**Step 4: Update CMakeLists, build, run tests**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

**Step 5: Commit**

```bash
git add src/kademlia/udp_transport.* tests/test_udp_transport.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: UDP transport — message serialization, signing, loopback send/recv"
```

---

## Task 7: Kademlia Engine

**Files:**
- Create: `src/kademlia/kademlia.h`
- Create: `src/kademlia/kademlia.cpp`
- Create: `tests/test_kademlia.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

**Step 1: Write src/kademlia/kademlia.h**

```cpp
#pragma once

#include "crypto/crypto.h"
#include "kademlia/node_id.h"
#include "kademlia/routing_table.h"
#include "kademlia/udp_transport.h"
#include "storage/storage.h"

#include <cstddef>

namespace helix::kademlia {

class Kademlia {
public:
    Kademlia(NodeInfo self, UdpTransport& transport, RoutingTable& table,
             storage::Storage& storage, const crypto::KeyPair& keypair);

    // Join the network via bootstrap nodes
    void bootstrap(const std::vector<std::pair<std::string, uint16_t>>& bootstrap_addrs);

    // Handle incoming UDP message (called from recv loop)
    void handle_message(const Message& msg, const std::string& from_addr, uint16_t from_port);

    // High-level data operations
    bool store(const crypto::Hash& key, std::span<const uint8_t> value);
    std::optional<std::vector<uint8_t>> find_value(const crypto::Hash& key);

    // Responsibility
    bool is_responsible(const crypto::Hash& key) const;
    std::vector<NodeInfo> responsible_nodes(const crypto::Hash& key) const;
    size_t replication_factor() const;

    const NodeInfo& self() const { return self_; }

private:
    NodeInfo self_;
    UdpTransport& transport_;
    RoutingTable& table_;
    storage::Storage& storage_;
    crypto::KeyPair keypair_;

    void handle_ping(const Message& msg, const std::string& from_addr, uint16_t from_port);
    void handle_pong(const Message& msg, const std::string& from_addr, uint16_t from_port);
    void handle_find_node(const Message& msg, const std::string& from_addr, uint16_t from_port);
    void handle_nodes(const Message& msg, const std::string& from_addr, uint16_t from_port);
    void handle_store(const Message& msg, const std::string& from_addr, uint16_t from_port);
    void handle_find_value(const Message& msg, const std::string& from_addr, uint16_t from_port);
    void handle_value(const Message& msg, const std::string& from_addr, uint16_t from_port);

    Message make_message(MessageType type, const std::vector<uint8_t>& payload);
    void send_to_node(const NodeInfo& node, const Message& msg);
};

} // namespace helix::kademlia
```

**Step 2: Write tests/test_kademlia.cpp**

Tests use 3 in-process nodes on localhost with ephemeral ports:

```cpp
#include "kademlia/kademlia.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>

using namespace helix::kademlia;
using namespace helix::crypto;
using namespace helix::storage;

class KademliaTest : public ::testing::Test {
protected:
    struct TestNode {
        crypto::KeyPair keypair;
        NodeInfo info;
        std::unique_ptr<UdpTransport> transport;
        std::unique_ptr<RoutingTable> table;
        std::unique_ptr<Storage> storage;
        std::unique_ptr<Kademlia> kad;
        std::thread recv_thread;
        std::filesystem::path db_path;
    };

    std::vector<std::unique_ptr<TestNode>> nodes_;

    TestNode& create_node() {
        auto node = std::make_unique<TestNode>();
        node->keypair = generate_keypair();
        node->transport = std::make_unique<UdpTransport>("127.0.0.1", 0);
        node->table = std::make_unique<RoutingTable>();

        node->db_path = std::filesystem::temp_directory_path() /
                        ("helix_kad_test_" + std::to_string(getpid()) + "_" +
                         std::to_string(nodes_.size()));
        std::filesystem::create_directories(node->db_path);
        node->storage = std::make_unique<Storage>(node->db_path);

        node->info.id = NodeId::from_pubkey(node->keypair.public_key);
        node->info.address = "127.0.0.1";
        node->info.udp_port = node->transport->local_port();
        node->info.ws_port = 0;
        node->info.pubkey = node->keypair.public_key;
        node->info.last_seen = std::chrono::steady_clock::now();

        node->kad = std::make_unique<Kademlia>(
            node->info, *node->transport, *node->table,
            *node->storage, node->keypair);

        // Start recv loop
        auto* kad_ptr = node->kad.get();
        auto* transport_ptr = node->transport.get();
        node->recv_thread = std::thread([kad_ptr, transport_ptr]() {
            transport_ptr->run([kad_ptr](const Message& msg,
                                         const std::string& from, uint16_t port) {
                kad_ptr->handle_message(msg, from, port);
            });
        });

        nodes_.push_back(std::move(node));
        return *nodes_.back();
    }

    void TearDown() override {
        for (auto& node : nodes_) {
            node->transport->stop();
            if (node->recv_thread.joinable()) node->recv_thread.join();
            node->storage.reset();
            std::filesystem::remove_all(node->db_path);
        }
        nodes_.clear();
    }

    void wait_ms(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

TEST_F(KademliaTest, ReplicationFactor) {
    auto& n1 = create_node();
    // Just self, R = min(3, 0) = 0 (or 1 if we count self)
    // With no peers, R should handle gracefully
    EXPECT_GE(n1.kad->replication_factor(), 1);
}

TEST_F(KademliaTest, BootstrapAddsNodes) {
    auto& n1 = create_node();
    auto& n2 = create_node();

    // n2 bootstraps from n1
    n2.kad->bootstrap({{"127.0.0.1", n1.info.udp_port}});
    wait_ms(500);

    // Both should know about each other
    EXPECT_GE(n1.table->size(), 1);
    EXPECT_GE(n2.table->size(), 1);
}

TEST_F(KademliaTest, ThreeNodeBootstrap) {
    auto& n1 = create_node();
    auto& n2 = create_node();
    auto& n3 = create_node();

    // n2 and n3 bootstrap from n1
    n2.kad->bootstrap({{"127.0.0.1", n1.info.udp_port}});
    wait_ms(300);
    n3.kad->bootstrap({{"127.0.0.1", n1.info.udp_port}});
    wait_ms(500);

    // All nodes should know about all others
    EXPECT_GE(n1.table->size(), 2);
    EXPECT_GE(n2.table->size(), 2);
    EXPECT_GE(n3.table->size(), 2);
}

TEST_F(KademliaTest, StoreAndFindValue) {
    auto& n1 = create_node();
    auto& n2 = create_node();
    auto& n3 = create_node();

    n2.kad->bootstrap({{"127.0.0.1", n1.info.udp_port}});
    wait_ms(300);
    n3.kad->bootstrap({{"127.0.0.1", n1.info.udp_port}});
    wait_ms(500);

    // Store a value from n1
    std::string key_str = "test_data_key";
    std::vector<uint8_t> key_data(key_str.begin(), key_str.end());
    Hash key = sha3_256(key_data);

    std::string val_str = "hello_from_kademlia";
    std::vector<uint8_t> value(val_str.begin(), val_str.end());

    EXPECT_TRUE(n1.kad->store(key, value));
    wait_ms(500);

    // Find value from n2
    auto found = n2.kad->find_value(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, value);
}

TEST_F(KademliaTest, ResponsibleNodes) {
    auto& n1 = create_node();
    auto& n2 = create_node();
    auto& n3 = create_node();

    n2.kad->bootstrap({{"127.0.0.1", n1.info.udp_port}});
    n3.kad->bootstrap({{"127.0.0.1", n1.info.udp_port}});
    wait_ms(500);

    Hash key{};
    key.fill(0x42);
    auto responsible = n1.kad->responsible_nodes(key);

    // R = min(3, 3) = 3, so all nodes should be responsible
    EXPECT_EQ(responsible.size(), 3);
}
```

**Step 3: Write src/kademlia/kademlia.cpp**

Implementation uses jsoncpp for message payloads. The key logic:
- `bootstrap`: send FIND_NODE to bootstrap, parse NODES response, populate routing table
- `handle_find_node`: respond with full node list as NODES
- `store`: compute responsible nodes, send STORE to each via UDP
- `handle_store`: if responsible, write to storage
- `find_value`: check local storage first, then send FIND_VALUE to responsible nodes
- `responsible_nodes`: self + routing table sorted by XOR distance, take top R
- `replication_factor`: `min(3, network_size)`

The JSON payload format for NODES:
```json
{
    "nodes": [
        {"id": "<hex>", "address": "1.2.3.4", "udp_port": 5000, "ws_port": 6000, "pubkey": "<hex>"}
    ]
}
```

For STORE: `{"key": "<hex>", "value": "<base64>"}`
For FIND_VALUE: `{"key": "<hex>"}`
For VALUE: `{"key": "<hex>", "value": "<base64>"}`

Implementation is straightforward dispatch. Write the full `.cpp` implementing all handlers.

**Note:** Use `json/json.h` for jsoncpp. Use hex encoding for hashes/IDs and base64 for values (implement simple helpers or use raw hex for both).

**Step 4: Update CMakeLists, build, run tests**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

**Step 5: Commit**

```bash
git add src/kademlia/kademlia.* tests/test_kademlia.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: Kademlia engine — bootstrap, store, find_value, responsibility"
```

---

## Task 8: Replication Log

**Files:**
- Create: `src/replication/repl_log.h`
- Create: `src/replication/repl_log.cpp`
- Create: `tests/test_repl_log.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

**Step 1: Write src/replication/repl_log.h**

```cpp
#pragma once

#include "crypto/crypto.h"
#include "storage/storage.h"
#include <cstdint>
#include <vector>

namespace helix::replication {

enum class Op : uint8_t { ADD = 0, DEL = 1, UPD = 2 };

struct LogEntry {
    uint64_t seq;
    Op op;
    uint64_t timestamp;
    std::vector<uint8_t> data;
};

// Serialize/deserialize log entries for storage
std::vector<uint8_t> serialize_entry(const LogEntry& entry);
LogEntry deserialize_entry(std::span<const uint8_t> data);

class ReplLog {
public:
    explicit ReplLog(storage::Storage& storage);

    // Append a new entry, returns assigned seq number
    uint64_t append(const crypto::Hash& key, Op op, std::span<const uint8_t> data);

    // Get entries after a given seq (for sync)
    std::vector<LogEntry> entries_after(const crypto::Hash& key, uint64_t after_seq) const;

    // Apply remote entries (idempotent — skips if seq already exists)
    void apply(const crypto::Hash& key, const std::vector<LogEntry>& entries);

    // Current highest seq for a key (0 if no entries)
    uint64_t current_seq(const crypto::Hash& key) const;

    // Delete entries with seq < before_seq (compaction)
    void compact(const crypto::Hash& key, uint64_t before_seq);

private:
    storage::Storage& storage_;

    // Composite key: [32-byte hash][8-byte seq big-endian]
    static std::array<uint8_t, 40> make_repl_key(const crypto::Hash& key, uint64_t seq);
    // Prefix for range scans: just the 32-byte hash
    static std::vector<uint8_t> make_prefix(const crypto::Hash& key);
};

} // namespace helix::replication
```

**Step 2: Write tests/test_repl_log.cpp**

```cpp
#include "replication/repl_log.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace helix::replication;
using namespace helix::crypto;
using namespace helix::storage;

class ReplLogTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<Storage> store_;
    std::unique_ptr<ReplLog> log_;

    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("helix_repl_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(db_path_);
        store_ = std::make_unique<Storage>(db_path_);
        log_ = std::make_unique<ReplLog>(*store_);
    }

    void TearDown() override {
        log_.reset();
        store_.reset();
        std::filesystem::remove_all(db_path_);
    }

    crypto::Hash test_key(const std::string& name) {
        std::vector<uint8_t> data(name.begin(), name.end());
        return sha3_256(data);
    }
};

TEST_F(ReplLogTest, AppendIncrementsSeq) {
    auto key = test_key("inbox:alice");
    std::vector<uint8_t> data = {1, 2, 3};

    uint64_t s1 = log_->append(key, Op::ADD, data);
    uint64_t s2 = log_->append(key, Op::ADD, data);
    uint64_t s3 = log_->append(key, Op::DEL, {});

    EXPECT_EQ(s1, 1);
    EXPECT_EQ(s2, 2);
    EXPECT_EQ(s3, 3);
}

TEST_F(ReplLogTest, CurrentSeq) {
    auto key = test_key("inbox:bob");
    EXPECT_EQ(log_->current_seq(key), 0);

    log_->append(key, Op::ADD, {1});
    EXPECT_EQ(log_->current_seq(key), 1);

    log_->append(key, Op::ADD, {2});
    EXPECT_EQ(log_->current_seq(key), 2);
}

TEST_F(ReplLogTest, EntriesAfter) {
    auto key = test_key("inbox:charlie");
    log_->append(key, Op::ADD, {10});
    log_->append(key, Op::ADD, {20});
    log_->append(key, Op::DEL, {});
    log_->append(key, Op::ADD, {30});

    auto entries = log_->entries_after(key, 2);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].seq, 3);
    EXPECT_EQ(entries[0].op, Op::DEL);
    EXPECT_EQ(entries[1].seq, 4);
    EXPECT_EQ(entries[1].op, Op::ADD);
    EXPECT_EQ(entries[1].data, std::vector<uint8_t>{30});
}

TEST_F(ReplLogTest, EntriesAfterZero) {
    auto key = test_key("inbox:all");
    log_->append(key, Op::ADD, {1});
    log_->append(key, Op::ADD, {2});

    auto entries = log_->entries_after(key, 0);
    EXPECT_EQ(entries.size(), 2);
}

TEST_F(ReplLogTest, ApplyIdempotent) {
    auto key = test_key("inbox:sync");
    log_->append(key, Op::ADD, {1});
    log_->append(key, Op::ADD, {2});

    // "Remote" entries including seq 2 (already exists) and seq 3 (new)
    std::vector<LogEntry> remote = {
        {2, Op::ADD, 100, {2}},  // duplicate — should skip
        {3, Op::ADD, 200, {3}},  // new — should apply
    };
    log_->apply(key, remote);

    EXPECT_EQ(log_->current_seq(key), 3);
    auto entries = log_->entries_after(key, 0);
    EXPECT_EQ(entries.size(), 3);
}

TEST_F(ReplLogTest, SeparateKeys) {
    auto key_a = test_key("inbox:alice");
    auto key_b = test_key("inbox:bob");

    log_->append(key_a, Op::ADD, {1});
    log_->append(key_a, Op::ADD, {2});
    log_->append(key_b, Op::ADD, {10});

    EXPECT_EQ(log_->current_seq(key_a), 2);
    EXPECT_EQ(log_->current_seq(key_b), 1);
}

TEST_F(ReplLogTest, Compact) {
    auto key = test_key("inbox:compact");
    log_->append(key, Op::ADD, {1});
    log_->append(key, Op::ADD, {2});
    log_->append(key, Op::ADD, {3});
    log_->append(key, Op::ADD, {4});

    log_->compact(key, 3); // delete seq 1, 2

    auto entries = log_->entries_after(key, 0);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].seq, 3);
    EXPECT_EQ(entries[1].seq, 4);
}

TEST_F(ReplLogTest, SerializeDeserializeEntry) {
    LogEntry e{42, Op::UPD, 1708000000, {10, 20, 30}};
    auto bytes = serialize_entry(e);
    auto restored = deserialize_entry(bytes);

    EXPECT_EQ(restored.seq, 42);
    EXPECT_EQ(restored.op, Op::UPD);
    EXPECT_EQ(restored.timestamp, 1708000000u);
    EXPECT_EQ(restored.data, (std::vector<uint8_t>{10, 20, 30}));
}
```

**Step 3: Write src/replication/repl_log.cpp**

Key implementation details:
- Composite key in `repl_log` table: `[32-byte hash][8-byte seq BE]`
- `append`: read current_seq, increment, write new entry
- `entries_after`: cursor lower_bound on `key || (after_seq+1)`, iterate while key prefix matches
- `apply`: for each entry, check if seq already exists, if not write it
- `compact`: cursor to `key || 0`, delete entries while seq < before_seq
- Entry serialization: `[8B seq BE][1B op][8B timestamp BE][remaining = data]`

**Step 4: Update CMakeLists, build, run tests**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

**Step 5: Commit**

```bash
git add src/replication/ tests/test_repl_log.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: replication log — seq-based append, sync, compaction"
```

---

## Task 9: WebSocket Server

**Files:**
- Create: `src/server/ws_server.h`
- Create: `src/server/ws_server.cpp`
- Create: `tests/test_ws_server.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

**Step 1: Write src/server/ws_server.h**

```cpp
#pragma once

#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

#include <App.h> // uWebSockets
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace helix::server {

class WsServer {
public:
    WsServer(uint16_t port, kademlia::Kademlia& kad,
             storage::Storage& storage, replication::ReplLog& repl_log,
             const crypto::KeyPair& node_keypair);

    void run();   // blocking — runs uWS event loop
    void stop();

    // Push message to a locally-connected client (called from Kademlia STORE handler)
    void push_new_message(const crypto::Hash& recipient_fp,
                          const std::vector<uint8_t>& blob);

private:
    struct Session {
        crypto::Hash fingerprint{};
        bool authenticated = false;
        std::vector<uint8_t> challenge_nonce;
    };

    uint16_t port_;
    kademlia::Kademlia& kad_;
    storage::Storage& storage_;
    replication::ReplLog& repl_log_;
    crypto::KeyPair node_keypair_;

    std::atomic<bool> running_{false};
    struct us_listen_socket_t* listen_socket_ = nullptr;

    // Connected authenticated clients
    std::mutex clients_mutex_;
    // Maps fingerprint -> websocket pointer
    // Note: uWS::WebSocket<false, true>* is the server-side WS type
    std::unordered_map<crypto::Hash, void*,
        kademlia::NodeIdHash> connected_clients_;

    // Message handlers
    void handle_message(void* ws, Session& session, std::string_view message);
    void handle_hello(void* ws, Session& session, const Json::Value& msg);
    void handle_auth(void* ws, Session& session, const Json::Value& msg);
    void handle_send(void* ws, Session& session, const Json::Value& msg);
    void handle_ack(void* ws, Session& session, const Json::Value& msg);
    void handle_allow(void* ws, Session& session, const Json::Value& msg);
    void handle_revoke(void* ws, Session& session, const Json::Value& msg);
    void handle_contact_request(void* ws, Session& session, const Json::Value& msg);
    void handle_fetch_pending(void* ws, Session& session, const Json::Value& msg);

    void send_json(void* ws, const Json::Value& msg);
    void send_error(void* ws, const std::string& reason);
};

} // namespace helix::server
```

**Step 2: Write tests/test_ws_server.cpp**

Test the auth flow and basic commands using a uWS client connecting to the server on localhost. The test starts the server in a background thread, connects a client, and exercises the protocol:

```cpp
#include "server/ws_server.h"
#include "kademlia/kademlia.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>

// WebSocket server integration tests
// These test the full auth flow and command handling

using namespace helix;

class WsServerTest : public ::testing::Test {
protected:
    // Full node setup (reuse KademliaTest pattern)
    crypto::KeyPair node_kp_;
    std::filesystem::path db_path_;
    std::unique_ptr<storage::Storage> store_;
    std::unique_ptr<kademlia::UdpTransport> udp_;
    std::unique_ptr<kademlia::RoutingTable> table_;
    std::unique_ptr<kademlia::Kademlia> kad_;
    std::unique_ptr<replication::ReplLog> repl_;
    std::unique_ptr<server::WsServer> ws_;
    std::thread udp_thread_;

    void SetUp() override {
        node_kp_ = crypto::generate_keypair();
        db_path_ = std::filesystem::temp_directory_path() /
                   ("helix_ws_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(db_path_);
        store_ = std::make_unique<storage::Storage>(db_path_);
        udp_ = std::make_unique<kademlia::UdpTransport>("127.0.0.1", 0);
        table_ = std::make_unique<kademlia::RoutingTable>();
        repl_ = std::make_unique<replication::ReplLog>(*store_);

        kademlia::NodeInfo self;
        self.id = kademlia::NodeId::from_pubkey(node_kp_.public_key);
        self.address = "127.0.0.1";
        self.udp_port = udp_->local_port();
        self.ws_port = 0; // assigned by WsServer
        self.pubkey = node_kp_.public_key;
        self.last_seen = std::chrono::steady_clock::now();

        kad_ = std::make_unique<kademlia::Kademlia>(
            self, *udp_, *table_, *store_, node_kp_);

        auto* kad_ptr = kad_.get();
        auto* udp_ptr = udp_.get();
        udp_thread_ = std::thread([kad_ptr, udp_ptr]() {
            udp_ptr->run([kad_ptr](const kademlia::Message& msg,
                                    const std::string& from, uint16_t port) {
                kad_ptr->handle_message(msg, from, port);
            });
        });
    }

    void TearDown() override {
        if (ws_) ws_->stop();
        udp_->stop();
        if (udp_thread_.joinable()) udp_thread_.join();
        store_.reset();
        std::filesystem::remove_all(db_path_);
    }
};

// Basic test: server starts and stops without crashing
TEST_F(WsServerTest, StartStop) {
    ws_ = std::make_unique<server::WsServer>(
        0, *kad_, *store_, *repl_, node_kp_);

    std::thread ws_thread([this]() { ws_->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ws_->stop();
    ws_thread.join();
}

// Additional tests for auth flow and commands would use a WS client library
// or test the handler functions directly. For now, verify startup/shutdown.
```

**Step 3: Write src/server/ws_server.cpp**

Implement the full WebSocket server with uWebSockets:
- `run()`: set up uWS::App, configure WS route with per-socket Session data
- Auth flow: HELLO → generate random nonce → CHALLENGE → verify ML-DSA signature → OK
- SEND: verify authenticated, compute responsible nodes, forward via Kademlia store
- ACK: verify authenticated, append DEL to repl log
- ALLOW/REVOKE: update allowlist in storage
- FETCH_PENDING: read inbox from storage, send queued messages
- `push_new_message`: find connected client by fingerprint, send NEW_MESSAGE

All messages are JSON. Use jsoncpp for parse/emit.

**Step 4: Update CMakeLists — add uWebSockets to helix-core**

In `src/CMakeLists.txt`, add:
```cmake
target_link_libraries(helix-core PUBLIC ... uWebSockets)
```

**Step 5: Build, run tests**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

**Step 6: Commit**

```bash
git add src/server/ tests/test_ws_server.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: WebSocket server — auth flow, inbox commands, real-time push"
```

---

## Task 10: main.cpp + Integration

**Files:**
- Modify: `src/main.cpp`

**Step 1: Update src/main.cpp to wire everything together**

```cpp
#include "crypto/crypto.h"
#include "kademlia/kademlia.h"
#include "kademlia/routing_table.h"
#include "kademlia/udp_transport.h"
#include "replication/repl_log.h"
#include "server/ws_server.h"
#include "storage/storage.h"

#include <spdlog/spdlog.h>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    // Defaults
    std::string data_dir = "./helix-data";
    std::string bind_addr = "0.0.0.0";
    uint16_t udp_port = 4000;
    uint16_t ws_port = 4001;
    std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes;

    // Parse args (--data-dir, --bind, --udp-port, --ws-port, --bootstrap)
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--bind" && i + 1 < argc) bind_addr = argv[++i];
        else if (arg == "--udp-port" && i + 1 < argc) udp_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--ws-port" && i + 1 < argc) ws_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--bootstrap" && i + 1 < argc) {
            std::string bs = argv[++i];
            auto colon = bs.rfind(':');
            if (colon != std::string::npos) {
                bootstrap_nodes.emplace_back(
                    bs.substr(0, colon),
                    static_cast<uint16_t>(std::stoi(bs.substr(colon + 1))));
            }
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create data directory
    std::filesystem::create_directories(data_dir);
    auto db_path = std::filesystem::path(data_dir) / "db";
    auto key_path = std::filesystem::path(data_dir) / "node.key";

    // Load or generate node keypair
    helix::crypto::KeyPair keypair;
    if (std::filesystem::exists(key_path)) {
        spdlog::info("loading node keypair from {}", key_path.string());
        std::ifstream f(key_path, std::ios::binary);
        uint32_t pk_len, sk_len;
        f.read(reinterpret_cast<char*>(&pk_len), 4);
        f.read(reinterpret_cast<char*>(&sk_len), 4);
        keypair.public_key.resize(pk_len);
        keypair.secret_key.resize(sk_len);
        f.read(reinterpret_cast<char*>(keypair.public_key.data()), pk_len);
        f.read(reinterpret_cast<char*>(keypair.secret_key.data()), sk_len);
    } else {
        spdlog::info("generating new node keypair");
        keypair = helix::crypto::generate_keypair();
        std::ofstream f(key_path, std::ios::binary);
        uint32_t pk_len = keypair.public_key.size();
        uint32_t sk_len = keypair.secret_key.size();
        f.write(reinterpret_cast<char*>(&pk_len), 4);
        f.write(reinterpret_cast<char*>(&sk_len), 4);
        f.write(reinterpret_cast<char*>(keypair.public_key.data()), pk_len);
        f.write(reinterpret_cast<char*>(keypair.secret_key.data()), sk_len);
    }

    auto node_id = helix::kademlia::NodeId::from_pubkey(keypair.public_key);
    spdlog::info("node ID: starting with {:02x}{:02x}{:02x}{:02x}...",
                 node_id.id[0], node_id.id[1], node_id.id[2], node_id.id[3]);

    // Init components
    helix::storage::Storage storage(db_path);
    helix::kademlia::RoutingTable table;
    helix::kademlia::UdpTransport udp_transport(bind_addr, udp_port);
    helix::replication::ReplLog repl_log(storage);

    helix::kademlia::NodeInfo self;
    self.id = node_id;
    self.address = bind_addr;
    self.udp_port = udp_port;
    self.ws_port = ws_port;
    self.pubkey = keypair.public_key;
    self.last_seen = std::chrono::steady_clock::now();

    helix::kademlia::Kademlia kad(self, udp_transport, table, storage, keypair);
    helix::server::WsServer ws_server(ws_port, kad, storage, repl_log, keypair);

    // Bootstrap
    if (!bootstrap_nodes.empty()) {
        spdlog::info("bootstrapping from {} nodes", bootstrap_nodes.size());
        kad.bootstrap(bootstrap_nodes);
    }

    // Start UDP recv loop in background thread
    std::thread udp_thread([&]() {
        udp_transport.run([&](const helix::kademlia::Message& msg,
                              const std::string& from, uint16_t port) {
            kad.handle_message(msg, from, port);
        });
    });

    spdlog::info("helix-node running — UDP:{} WS:{}", udp_port, ws_port);

    // Run WebSocket server on main thread
    // (WsServer::run() blocks until stop() is called)
    std::thread ws_thread([&]() { ws_server.run(); });

    // Wait for signal
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spdlog::info("shutting down...");
    ws_server.stop();
    udp_transport.stop();
    ws_thread.join();
    udp_thread.join();
    spdlog::info("helix-node stopped");
    return 0;
}
```

**Step 2: Build and smoke test**

```bash
cmake --build build -j$(nproc)
./build/src/helix-node --data-dir /tmp/helix-test-node --udp-port 4000 --ws-port 4001
# Should print startup logs, Ctrl-C to stop
```

**Step 3: Multi-node smoke test**

In 3 terminals:
```bash
# Terminal 1 (bootstrap node)
./build/src/helix-node --data-dir /tmp/node0 --udp-port 4000 --ws-port 4001

# Terminal 2
./build/src/helix-node --data-dir /tmp/node1 --udp-port 4010 --ws-port 4011 --bootstrap 127.0.0.1:4000

# Terminal 3
./build/src/helix-node --data-dir /tmp/node2 --udp-port 4020 --ws-port 4021 --bootstrap 127.0.0.1:4000
```

Verify logs show nodes discovering each other.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: main.cpp — wire all components, CLI args, keypair persistence"
```

---

## Post-Implementation

After all 10 tasks, the server has:
- PQ crypto (SHA3-256, ML-DSA-87, PoW verification)
- Persistent storage (8 named tables in libmdbx)
- Full Kademlia engine (bootstrap, store, find_value, XOR responsibility)
- Sequence-based replication log with sync and compaction
- WebSocket client interface (auth, inbox commands, real-time push)
- Single binary with CLI args

**Open items for follow-up:**
- SYNC_REQ/SYNC_RESP handlers in Kademlia (periodic background sync)
- Reputation tracking
- 7-day TTL enforcement on unacknowledged messages
- Contact request PoW verification in WsServer
- Hardcoded bootstrap DNS resolution (cpunk.io)
- Integration test suite with Docker cluster
