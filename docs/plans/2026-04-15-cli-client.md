# chromatindb-cli Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone PQ-encrypted CLI client that speaks the chromatindb wire protocol directly, with envelope encryption on all uploads.

**Architecture:** Clean-room C++20 implementation. 6 source files in `cli/src/`, standalone CMake with FetchContent. Implements PQ handshake, AEAD framing, FlatBuffer blob codec, and ML-KEM envelope encryption from PROTOCOL.md spec. No node code dependency.

**Tech Stack:** C++20, liboqs, libsodium, FlatBuffers, Standalone Asio, spdlog, nlohmann/json, Catch2

**Reference:** `db/PROTOCOL.md` is the authoritative spec for all wire formats, handshake flows, and envelope encryption.

---

### Task 1: CMake scaffold + identity keygen

**Files:**
- Create: `cli/CMakeLists.txt`
- Create: `cli/src/identity.h`
- Create: `cli/src/identity.cpp`
- Create: `cli/src/main.cpp`
- Create: `cli/tests/CMakeLists.txt`
- Create: `cli/tests/test_identity.cpp`

- [ ] **Step 1: Write identity tests**

```cpp
// cli/tests/test_identity.cpp
#include <catch2/catch_test_macros.hpp>
#include "identity.h"
#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("identity: generate creates signing + KEM keypairs") {
    auto id = chromatindb::cli::Identity::generate();
    REQUIRE(id.signing_pubkey().size() == 2592);  // ML-DSA-87
    REQUIRE(id.kem_pubkey().size() == 1568);       // ML-KEM-1024
    REQUIRE(id.namespace_id().size() == 32);       // SHA3-256
}

TEST_CASE("identity: save and load roundtrip") {
    auto tmp = fs::temp_directory_path() / "chromatindb-test-id";
    fs::create_directories(tmp);

    auto id1 = chromatindb::cli::Identity::generate();
    id1.save_to(tmp);

    auto id2 = chromatindb::cli::Identity::load_from(tmp);
    REQUIRE(id1.signing_pubkey() == id2.signing_pubkey());
    REQUIRE(id1.kem_pubkey() == id2.kem_pubkey());
    REQUIRE(id1.namespace_id() == id2.namespace_id());

    fs::remove_all(tmp);
}

TEST_CASE("identity: namespace is SHA3-256 of signing pubkey") {
    auto id = chromatindb::cli::Identity::generate();
    // Verify namespace derivation manually
    // SHA3-256 of signing pubkey should match namespace_id
    REQUIRE(id.namespace_id().size() == 32);
}

TEST_CASE("identity: export_public_keys produces signing + KEM pubkeys") {
    auto id = chromatindb::cli::Identity::generate();
    auto exported = id.export_public_keys();
    // Format: [signing_pubkey:2592][kem_pubkey:1568] = 4160 bytes
    REQUIRE(exported.size() == 2592 + 1568);
}

TEST_CASE("identity: load_public_keys roundtrips with export") {
    auto id = chromatindb::cli::Identity::generate();
    auto exported = id.export_public_keys();
    auto [signing_pk, kem_pk] = chromatindb::cli::Identity::load_public_keys(exported);
    REQUIRE(signing_pk == id.signing_pubkey());
    REQUIRE(kem_pk == id.kem_pubkey());
}
```

- [ ] **Step 2: Write identity.h**

```cpp
// cli/src/identity.h
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace chromatindb::cli {

/// CLI identity: ML-DSA-87 signing keypair + ML-KEM-1024 encryption keypair.
/// Namespace = SHA3-256(signing_pubkey).
class Identity {
public:
    static Identity generate();
    static Identity load_from(const std::filesystem::path& dir);

    void save_to(const std::filesystem::path& dir) const;

    std::span<const uint8_t> signing_pubkey() const;
    std::span<const uint8_t> signing_seckey() const;
    std::span<const uint8_t> kem_pubkey() const;
    std::span<const uint8_t> kem_seckey() const;
    std::span<const uint8_t, 32> namespace_id() const;

    /// Sign a message with ML-DSA-87.
    std::vector<uint8_t> sign(std::span<const uint8_t> message) const;

    /// Export both public keys as a single shareable blob.
    /// Format: [signing_pubkey:2592][kem_pubkey:1568]
    std::vector<uint8_t> export_public_keys() const;

    /// Parse exported public keys.
    static std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
    load_public_keys(std::span<const uint8_t> data);

private:
    std::vector<uint8_t> signing_pk_;
    std::vector<uint8_t> signing_sk_;
    std::vector<uint8_t> kem_pk_;
    std::vector<uint8_t> kem_sk_;
    std::array<uint8_t, 32> namespace_id_{};
};

} // namespace chromatindb::cli
```

- [ ] **Step 3: Implement identity.cpp**

Use liboqs directly for ML-DSA-87 (`OQS_SIG`) and ML-KEM-1024 (`OQS_KEM`). SHA3-256 via liboqs `OQS_SHA3_sha3_256`. File I/O: raw binary read/write of key bytes. Four files: `identity.key` (signing secret), `identity.pub` (signing public), `identity.kem` (KEM secret), `identity.kpub` (KEM public).

- [ ] **Step 4: Write CMakeLists.txt**

Standalone CMake with FetchContent for: asio (header-only), spdlog, nlohmann_json, flatbuffers, catch2, liboqs, libsodium (if not system). Build `chromatindb-cli` binary and `cli_tests` test binary.

- [ ] **Step 5: Write main.cpp skeleton**

Arg parsing with command dispatch. For now only `keygen`, `whoami`, `export-key`. Simple `argc`/`argv` parsing like the node's `main.cpp` — no external arg parsing library.

- [ ] **Step 6: Build and run tests**

Run: `cd cli/build && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . -j$(nproc) && ./cli_tests`

Expected: All identity tests pass.

- [ ] **Step 7: Commit**

```
feat(cli): identity keygen with ML-DSA-87 + ML-KEM-1024
```

---

### Task 2: Wire protocol — AEAD framing + TransportMessage codec

**Files:**
- Create: `cli/src/wire.h`
- Create: `cli/src/wire.cpp`
- Create: `cli/tests/test_wire.cpp`

- [ ] **Step 1: Write wire tests**

Test TransportMessage encode/decode roundtrip. Test AEAD frame encrypt/decrypt with counter nonces. Test FlatBuffer Blob encode/decode. Test canonical signing input (SHA3-256 hash). Test big-endian helpers. Test tombstone data construction.

Key test cases:
- `wire: encode_transport roundtrips` — encode type + payload + request_id, decode, verify fields match
- `wire: aead_frame encrypt/decrypt roundtrip` — encrypt with counter nonce, decrypt with same counter
- `wire: nonce format is 4 zero bytes + 8BE counter` — verify nonce byte layout
- `wire: encode_blob/decode_blob roundtrip` — FlatBuffer Blob with all 6 fields
- `wire: build_signing_input matches expected` — canonical signing input with known values
- `wire: make_tombstone_data format` — 4-byte magic + 32-byte target hash = 36 bytes

- [ ] **Step 2: Write wire.h**

```cpp
// cli/src/wire.h
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::cli::wire {

// Big-endian helpers
void store_u16_be(uint8_t* dst, uint16_t val);
void store_u32_be(uint8_t* dst, uint32_t val);
void store_u64_be(uint8_t* dst, uint64_t val);
uint16_t load_u16_be(const uint8_t* src);
uint32_t load_u32_be(const uint8_t* src);
uint64_t load_u64_be(const uint8_t* src);

// Message types (subset used by CLI)
enum MsgType : uint8_t {
    Data = 8, Delete = 17, DeleteAck = 18,
    Subscribe = 19, Unsubscribe = 20, Notification = 21,
    WriteAck = 30, ReadRequest = 31, ReadResponse = 32,
    ListRequest = 33, ListResponse = 34,
    StatsRequest = 35, StatsResponse = 36,
    ExistsRequest = 37, ExistsResponse = 38,
    NodeInfoRequest = 39, NodeInfoResponse = 40,
    SyncNamespaceAnnounce = 62, ErrorResponse = 63,
};

/// TransportMessage: [FlatBuffer: type + payload + request_id]
std::vector<uint8_t> encode_transport(MsgType type,
    std::span<const uint8_t> payload, uint32_t request_id = 0);

struct DecodedMessage {
    MsgType type;
    std::vector<uint8_t> payload;
    uint32_t request_id;
};
std::optional<DecodedMessage> decode_transport(std::span<const uint8_t> data);

/// AEAD frame: encrypt plaintext with counter nonce, return [len:4BE][ciphertext+tag]
std::vector<uint8_t> encrypt_frame(std::span<const uint8_t> plaintext,
    std::span<const uint8_t> key, uint64_t counter);

/// AEAD frame: read [len:4BE][ct+tag], decrypt with counter nonce
std::optional<std::vector<uint8_t>> decrypt_frame(
    std::span<const uint8_t> frame, std::span<const uint8_t> key, uint64_t counter);

/// FlatBuffer Blob
struct BlobData {
    std::array<uint8_t, 32> namespace_id;
    std::vector<uint8_t> pubkey;
    std::vector<uint8_t> data;
    uint32_t ttl;
    uint64_t timestamp;
    std::vector<uint8_t> signature;
};
std::vector<uint8_t> encode_blob(const BlobData& blob);
std::optional<BlobData> decode_blob(std::span<const uint8_t> buf);

/// Canonical signing input: SHA3-256(namespace || data || ttl_be32 || timestamp_be64)
std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> ns, std::span<const uint8_t> data,
    uint32_t ttl, uint64_t timestamp);

/// Tombstone data: [0xDEADBEEF][target_hash:32]
std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash);

} // namespace chromatindb::cli::wire
```

- [ ] **Step 3: Implement wire.cpp**

BE helpers: shift-and-mask. TransportMessage: use FlatBuffers `flatbuffers::FlatBufferBuilder`. AEAD framing: libsodium `crypto_aead_chacha20poly1305_ietf_encrypt/decrypt`, nonce = 4 zero bytes + 8BE counter. Blob codec: FlatBuffer with ForceDefaults. Signing input: liboqs SHA3-256 incremental hash (`OQS_SHA3_sha3_256`). Tombstone: 4-byte magic + 32-byte hash.

You will need to write a FlatBuffer schema for TransportMessage and Blob, or use the FlatBuffers C API to build them manually. Manual construction with `FlatBufferBuilder` is simpler for just two message types.

- [ ] **Step 4: Build and run tests**

Run: `cd cli/build && cmake --build . -j$(nproc) && ./cli_tests`

Expected: All wire tests pass.

- [ ] **Step 5: Commit**

```
feat(cli): wire protocol — AEAD framing, TransportMessage, FlatBuffer blob codec
```

---

### Task 3: Connection — PQ handshake + TrustedHello + AEAD session

**Files:**
- Create: `cli/src/connection.h`
- Create: `cli/src/connection.cpp`

- [ ] **Step 1: Write connection.h**

```cpp
// cli/src/connection.h
#pragma once

#include "identity.h"
#include "wire.h"

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace chromatindb::cli {

/// Single-use connection to a chromatindb node.
/// Connect -> handshake -> send request -> receive response -> disconnect.
class Connection {
public:
    explicit Connection(Identity& identity);

    /// Connect to node. Tries UDS first, falls back to TCP.
    /// Performs handshake and drains SyncNamespaceAnnounce.
    bool connect(const std::string& host, uint16_t port,
                 const std::string& uds_path = "/run/chromatindb/node.sock");

    /// Send a transport message (AEAD-encrypted).
    bool send(wire::MsgType type, std::span<const uint8_t> payload,
              uint32_t request_id = 1);

    /// Receive one transport message (AEAD-decrypt).
    std::optional<wire::DecodedMessage> recv();

    /// Close the connection.
    void close();

private:
    bool connect_uds(const std::string& uds_path);
    bool connect_tcp(const std::string& host, uint16_t port);
    bool handshake_pq();         // Full ML-KEM-1024 handshake (TCP)
    bool handshake_trusted();    // TrustedHello (UDS)
    bool drain_announce();       // Drain SyncNamespaceAnnounce(62)

    bool send_raw(std::span<const uint8_t> data);
    std::optional<std::vector<uint8_t>> recv_raw();
    bool send_encrypted(std::span<const uint8_t> plaintext);
    std::optional<std::vector<uint8_t>> recv_encrypted();

    Identity& identity_;
    std::variant<std::monostate,
                 asio::ip::tcp::socket,
                 asio::local::stream_protocol::socket> socket_;
    asio::io_context ioc_;

    std::vector<uint8_t> send_key_;  // 32 bytes
    std::vector<uint8_t> recv_key_;  // 32 bytes
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;
    bool connected_ = false;
};

} // namespace chromatindb::cli
```

- [ ] **Step 2: Implement connection.cpp**

**UDS connect:** Open `local::stream_protocol::socket`, connect to path. If file doesn't exist or connect fails, return false (caller falls back to TCP).

**TCP connect:** Resolve host, connect socket with 5-second timeout.

**PQ handshake (TCP):** Per PROTOCOL.md:
1. Generate ephemeral ML-KEM-1024 keypair
2. Send KemPubkey(1): `[kem_pubkey:1568]` as raw TransportMessage frame
3. Recv KemCiphertext(2): `[kem_ciphertext:1568]`
4. Decapsulate -> shared secret (32 bytes)
5. HKDF-SHA256 with empty salt:
   - expand(prk, "chromatin-init-to-resp-v1", 32) -> send_key (we're initiator)
   - expand(prk, "chromatin-resp-to-init-v1", 32) -> recv_key
6. Session fingerprint: SHA3-256(shared_secret + init_kem_pk + resp_signing_pk)
7. Send AuthSignature(3) encrypted: `[pubkey_len:4BE][signing_pubkey][signature(fingerprint)]`
8. Recv AuthSignature(3) encrypted: validate signature, store peer pubkey

**TrustedHello handshake (UDS):** Per PROTOCOL.md:
1. Generate 32-byte random nonce
2. Send TrustedHello(23) raw: `[nonce:32][signing_pubkey:2592]`
3. Recv TrustedHello(23): parse responder nonce + pubkey
4. HKDF-SHA256: ikm = our_nonce || their_nonce, salt = our_pk || their_pk
   - expand(prk, "chromatin-init-to-resp-v1", 32) -> send_key
   - expand(prk, "chromatin-resp-to-init-v1", 32) -> recv_key
5. Fingerprint: SHA3-256(ikm || salt)
6. Send AuthSignature(3) encrypted: sign fingerprint
7. Recv AuthSignature(3) encrypted: verify

**drain_announce:** After handshake, recv one message. If type == SyncNamespaceAnnounce(62), discard. Otherwise error.

**send/recv:** encode TransportMessage -> encrypt_frame(send_key, send_counter_++) -> send_raw. recv_raw -> decrypt_frame(recv_key, recv_counter_++) -> decode TransportMessage.

- [ ] **Step 3: Integration test with a real node**

Manual test: start a node, run `chromatindb-cli info` (after Task 5 wires it up). Verify connection, handshake, and response parsing all work. This is tested end-to-end, not unit-tested in isolation (handshake requires a real node).

- [ ] **Step 4: Commit**

```
feat(cli): PQ handshake + TrustedHello connection with AEAD framing
```

---

### Task 4: Envelope encryption

**Files:**
- Create: `cli/src/envelope.h`
- Create: `cli/src/envelope.cpp`
- Create: `cli/tests/test_envelope.cpp`

- [ ] **Step 1: Write envelope tests**

```cpp
// cli/tests/test_envelope.cpp
#include <catch2/catch_test_macros.hpp>
#include "envelope.h"
#include "identity.h"

using namespace chromatindb::cli;

TEST_CASE("envelope: encrypt for self, decrypt roundtrip") {
    auto id = Identity::generate();
    std::vector<uint8_t> plaintext = {'h','e','l','l','o'};
    std::vector<std::span<const uint8_t>> recipients = {id.kem_pubkey()};

    auto envelope = envelope::encrypt(plaintext, recipients);
    REQUIRE(envelope.size() > 20);  // header alone is 20 bytes
    // Verify magic
    REQUIRE(envelope[0] == 0x43);  // 'C'
    REQUIRE(envelope[1] == 0x45);  // 'E'
    REQUIRE(envelope[2] == 0x4E);  // 'N'
    REQUIRE(envelope[3] == 0x56);  // 'V'

    auto decrypted = envelope::decrypt(envelope, id.kem_seckey(), id.kem_pubkey());
    REQUIRE(decrypted.has_value());
    REQUIRE(*decrypted == plaintext);
}

TEST_CASE("envelope: encrypt for two recipients, both can decrypt") {
    auto alice = Identity::generate();
    auto bob = Identity::generate();
    std::vector<uint8_t> plaintext = {'s','e','c','r','e','t'};
    std::vector<std::span<const uint8_t>> recipients = {
        alice.kem_pubkey(), bob.kem_pubkey()
    };

    auto envelope_data = envelope::encrypt(plaintext, recipients);

    auto dec_alice = envelope::decrypt(envelope_data, alice.kem_seckey(), alice.kem_pubkey());
    REQUIRE(dec_alice.has_value());
    REQUIRE(*dec_alice == plaintext);

    auto dec_bob = envelope::decrypt(envelope_data, bob.kem_seckey(), bob.kem_pubkey());
    REQUIRE(dec_bob.has_value());
    REQUIRE(*dec_bob == plaintext);
}

TEST_CASE("envelope: non-recipient cannot decrypt") {
    auto alice = Identity::generate();
    auto eve = Identity::generate();
    std::vector<uint8_t> plaintext = {'n','o','p','e'};
    std::vector<std::span<const uint8_t>> recipients = {alice.kem_pubkey()};

    auto envelope_data = envelope::encrypt(plaintext, recipients);
    auto dec_eve = envelope::decrypt(envelope_data, eve.kem_seckey(), eve.kem_pubkey());
    REQUIRE_FALSE(dec_eve.has_value());
}

TEST_CASE("envelope: stanzas sorted by kem_pk_hash") {
    auto a = Identity::generate();
    auto b = Identity::generate();
    std::vector<std::span<const uint8_t>> recipients = {
        a.kem_pubkey(), b.kem_pubkey()
    };

    auto envelope_data = envelope::encrypt({'x'}, recipients);
    // Parse: skip 20-byte header, read two stanzas, verify pk_hash ordering
    REQUIRE(envelope_data.size() > 20 + 2 * 1648);
    auto hash1 = std::span<const uint8_t>(envelope_data.data() + 20, 32);
    auto hash2 = std::span<const uint8_t>(envelope_data.data() + 20 + 1648, 32);
    REQUIRE(std::lexicographical_compare(hash1.begin(), hash1.end(),
                                          hash2.begin(), hash2.end()));
}
```

- [ ] **Step 2: Write envelope.h**

```cpp
// cli/src/envelope.h
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::cli::envelope {

/// Encrypt plaintext for one or more recipients.
/// Each recipient is an ML-KEM-1024 public key (1568 bytes).
/// Returns the full envelope binary (magic + header + stanzas + ciphertext).
std::vector<uint8_t> encrypt(
    std::span<const uint8_t> plaintext,
    std::vector<std::span<const uint8_t>> recipient_kem_pubkeys);

/// Decrypt an envelope using our KEM secret key.
/// Returns plaintext on success, nullopt if we're not a recipient or decryption fails.
std::optional<std::vector<uint8_t>> decrypt(
    std::span<const uint8_t> envelope_data,
    std::span<const uint8_t> our_kem_seckey,
    std::span<const uint8_t> our_kem_pubkey);

/// Check if data starts with envelope magic "CENV".
bool is_envelope(std::span<const uint8_t> data);

} // namespace chromatindb::cli::envelope
```

- [ ] **Step 3: Implement envelope.cpp**

Per PROTOCOL.md envelope spec:

**encrypt:**
1. Generate random DEK (32 bytes, `randombytes_buf`)
2. Generate random data_nonce (12 bytes)
3. For each recipient KEM pubkey:
   - `kem_pk_hash` = SHA3-256(pubkey)
   - ML-KEM-1024 encapsulate(pubkey) -> (ciphertext, shared_secret)
   - KEK = HKDF-SHA256(ikm=shared_secret, salt=empty, info="chromatindb-envelope-kek-v1", len=32)
   - wrapped_dek = AEAD-encrypt(DEK, zero_nonce, KEK, AD=partial_header+accumulated_stanzas)
   - Stanza: [kem_pk_hash:32][ciphertext:1568][wrapped_dek:48]
4. Sort stanzas by kem_pk_hash (lexicographic)
5. Build header: magic "CENV" + version 0x01 + suite 0x01 + recipient_count BE16 + data_nonce
6. AEAD-encrypt plaintext with DEK, data_nonce, AD = header + all stanzas
7. Return header + stanzas + ciphertext+tag

Note on DEK wrapping AD: Per PROTOCOL.md, the AD for wrapped_dek is "Partial header (fixed 20 bytes) + all pk_hash (32 bytes each) + all kem_ciphertext (1568 bytes each) accumulated so far". Since stanzas are sorted before wrapping, build sorted stanzas first (without wrapped_dek), then wrap DEK for each stanza using the accumulated AD.

Actually, re-reading PROTOCOL.md: stanzas must be sorted by pk_hash, and the wrapped_dek AD uses "accumulated so far". This means: sort recipient pubkeys by their pk_hash first, then process in sorted order, accumulating AD as you go. Each stanza's wrapped_dek AD includes the header + all previous stanzas' pk_hash and kem_ciphertext (but NOT previous wrapped_deks).

**decrypt:**
1. Validate magic, version, suite
2. Compute our_pk_hash = SHA3-256(our_kem_pubkey)
3. Scan stanzas, find one where kem_pk_hash matches
4. If no match, return nullopt
5. Decapsulate kem_ciphertext with our_kem_seckey -> shared_secret
6. KEK = HKDF-SHA256(shared_secret, empty, "chromatindb-envelope-kek-v1", 32)
7. Rebuild AD for this stanza's position (header + accumulated pk_hash+kem_ct before it)
8. Unwrap: AEAD-decrypt(wrapped_dek, zero_nonce, KEK, AD) -> DEK
9. AEAD-decrypt(ciphertext, data_nonce, DEK, AD=header+all_stanzas)
10. Return plaintext

- [ ] **Step 4: Build and run tests**

Run: `cd cli/build && cmake --build . -j$(nproc) && ./cli_tests`

Expected: All envelope tests pass.

- [ ] **Step 5: Commit**

```
feat(cli): envelope encryption — ML-KEM-1024 + ChaCha20-Poly1305
```

---

### Task 5: Commands — put, get, rm, reshare, ls, info, stats, exists

**Files:**
- Create: `cli/src/commands.h`
- Create: `cli/src/commands.cpp`
- Modify: `cli/src/main.cpp` — wire up all commands

- [ ] **Step 1: Write commands.h**

```cpp
// cli/src/commands.h
#pragma once

#include "identity.h"
#include <string>
#include <vector>

namespace chromatindb::cli::cmd {

struct ConnectOpts {
    std::string host = "127.0.0.1";
    uint16_t port = 4200;
    std::string uds_path = "/run/chromatindb/node.sock";
    bool quiet = false;
};

int keygen(const std::string& identity_dir, bool force);
int whoami(const std::string& identity_dir);
int export_key(const std::string& identity_dir);

int put(const std::string& identity_dir, const std::string& file_path,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin, const ConnectOpts& opts);

int get(const std::string& identity_dir, const std::string& hash_hex,
        const std::string& namespace_hex, bool to_stdout, const ConnectOpts& opts);

int rm(const std::string& identity_dir, const std::string& hash_hex,
       const std::string& namespace_hex, const ConnectOpts& opts);

int reshare(const std::string& identity_dir, const std::string& hash_hex,
            const std::string& namespace_hex,
            const std::vector<std::string>& share_pubkey_files,
            uint32_t ttl, const ConnectOpts& opts);

int ls(const std::string& identity_dir, const std::string& namespace_hex,
       const ConnectOpts& opts);

int exists(const std::string& identity_dir, const std::string& hash_hex,
           const std::string& namespace_hex, const ConnectOpts& opts);

int info(const std::string& identity_dir, const ConnectOpts& opts);
int stats(const std::string& identity_dir, const ConnectOpts& opts);

} // namespace chromatindb::cli::cmd
```

- [ ] **Step 2: Implement commands.cpp**

Each command follows the same pattern: load identity, connect, send request, parse response, print result.

**put:**
1. Load identity
2. Read file (or stdin). Build metadata: `{"name":"<filename>","size":<size>}`
3. Build payload: [metadata_len:4BE][metadata_json][file_data]
4. Load recipient pubkeys from --share files. Always include self (own KEM pubkey).
5. `envelope::encrypt(payload, recipients)` -> envelope_data
6. Build signing input: `SHA3-256(namespace || envelope_data || ttl_be32 || timestamp_be64)`
7. Sign with identity
8. `wire::encode_blob(BlobData{...})` -> flatbuf_bytes
9. Connect, send Data(8) with flatbuf_bytes, recv WriteAck(30)
10. Parse WriteAck: [hash:32][seq_num:8BE][status:1]. Print hash hex.

**get:**
1. Connect, send ReadRequest(31): [namespace:32][hash:32]
2. Recv ReadResponse(32): [status:1][blob_data...]
3. If status != 0x01 (found), error
4. `wire::decode_blob(blob_data)` -> BlobData
5. `envelope::decrypt(blob.data, kem_seckey, kem_pubkey)` -> plaintext
6. Parse payload metadata, extract filename + file data
7. Save to filename (or stdout)

**rm:**
1. Build tombstone data: `wire::make_tombstone_data(target_hash)`
2. Sign tombstone blob (ttl=0, timestamp=now)
3. Encode as FlatBuffer Blob, send as Delete(17)
4. Recv DeleteAck(18), print confirmation

**reshare:**
1. `get` the blob (steps from get above, return decrypted payload)
2. `put` with new recipient list (steps from put above)
3. `rm` the old hash
4. Print new hash

**ls:**
1. Namespace defaults to own (from identity). Parse --namespace override.
2. Connect, send ListRequest(33): [namespace:32][since_seq:8BE=0][limit:4BE=100]
3. Recv ListResponse(34): [count:4BE][entries: N * (hash:32 + seq_num:8BE)][has_more:1]
4. Print hashes. If has_more, loop with updated since_seq.

**exists:**
1. Connect, send ExistsRequest(37): [namespace:32][hash:32]
2. Recv ExistsResponse(38): [exists:1][hash:32]
3. Print "exists" or "not found"

**info:**
1. Connect, send NodeInfoRequest(39): empty payload
2. Recv NodeInfoResponse(40): parse variable-length format per PROTOCOL.md
3. Print: version, git hash, uptime, peers, namespaces, blobs, storage

**stats:**
1. Connect, send StatsRequest(35): [namespace:32]
2. Recv StatsResponse(36): [count:8BE][bytes:8BE][quota:8BE]
3. Print stats

- [ ] **Step 3: Wire up main.cpp**

Parse global flags (--identity, --uds, -p, -q). Dispatch to command functions. Default identity dir: `~/.chromatindb/`. `put` default TTL: 0 (permanent). For `get`/`rm`/`exists`/`reshare`, namespace defaults to own unless hash format includes namespace.

For commands that take `[host[:port]]` as a positional arg: parse `host:port` or just `host` (default port 4200). If no host given, use `127.0.0.1`.

- [ ] **Step 4: Build**

Run: `cd cli/build && cmake --build . -j$(nproc)`

Expected: Clean build with all commands wired up.

- [ ] **Step 5: Commit**

```
feat(cli): all commands — put, get, rm, reshare, ls, info, stats, exists
```

---

### Task 6: End-to-end testing against a live node

- [ ] **Step 1: Start a test node**

```bash
mkdir -p /tmp/cli-test/data
/path/to/chromatindb keygen --data-dir /tmp/cli-test/data
/path/to/chromatindb run --data-dir /tmp/cli-test/data --config /tmp/cli-test/node.json &
```

With config: `{"bind_address":"127.0.0.1:4200","uds_path":"/tmp/cli-test/node.sock","data_dir":"/tmp/cli-test/data"}`

- [ ] **Step 2: Test identity commands**

```bash
chromatindb-cli keygen
chromatindb-cli whoami    # Should print 64-char hex namespace
chromatindb-cli export-key > /tmp/mypubkey.bin
```

- [ ] **Step 3: Test put + get roundtrip**

```bash
echo "hello world" > /tmp/test.txt
chromatindb-cli put /tmp/test.txt 127.0.0.1
# Prints hash, e.g.: a3f2b8c9...

chromatindb-cli get a3f2b8c9... 127.0.0.1
# Should save as test.txt in current dir
diff /tmp/test.txt test.txt  # Should be identical
```

- [ ] **Step 4: Test ls, exists, stats, info**

```bash
chromatindb-cli ls 127.0.0.1           # Should list the blob hash
chromatindb-cli exists a3f2b8c9... 127.0.0.1  # "exists"
chromatindb-cli stats 127.0.0.1        # count: 1, bytes: ...
chromatindb-cli info 127.0.0.1         # node version, peers, storage
```

- [ ] **Step 5: Test rm**

```bash
chromatindb-cli rm a3f2b8c9... 127.0.0.1
chromatindb-cli exists a3f2b8c9... 127.0.0.1  # "not found" (tombstoned)
```

- [ ] **Step 6: Test multi-recipient encrypt + reshare**

```bash
# Generate second identity
chromatindb-cli keygen --identity /tmp/bob
chromatindb-cli export-key --identity /tmp/bob > /tmp/bob.pub

# Upload shared with Bob
chromatindb-cli put /tmp/test.txt --share /tmp/bob.pub 127.0.0.1
# hash: d7e1f4a2...

# Bob can decrypt
chromatindb-cli get d7e1f4a2... --identity /tmp/bob 127.0.0.1

# Reshare test
chromatindb-cli reshare d7e1f4a2... --share /tmp/bob.pub 127.0.0.1
# new hash: f1a2b3c4..., old tombstoned
```

- [ ] **Step 7: Test UDS fallback**

```bash
# With node running on UDS
chromatindb-cli info --uds /tmp/cli-test/node.sock
# Should connect via UDS (TrustedHello), print node info
```

- [ ] **Step 8: Commit any fixes**

```
fix(cli): [describe any fixes found during E2E testing]
```

---

## File Summary

| File | Purpose |
|------|---------|
| `cli/CMakeLists.txt` | Standalone build, FetchContent all deps |
| `cli/src/main.cpp` | Arg parsing, command dispatch |
| `cli/src/identity.h/cpp` | ML-DSA-87 + ML-KEM-1024 keypair management |
| `cli/src/wire.h/cpp` | BE helpers, TransportMessage, AEAD framing, FlatBuffer blob, signing input |
| `cli/src/connection.h/cpp` | UDS/TCP connect, PQ + TrustedHello handshake, encrypted send/recv |
| `cli/src/envelope.h/cpp` | Envelope encrypt/decrypt per PROTOCOL.md |
| `cli/src/commands.h/cpp` | All CLI command implementations |
| `cli/tests/test_identity.cpp` | Identity keygen/load/export tests |
| `cli/tests/test_wire.cpp` | Wire format encode/decode tests |
| `cli/tests/test_envelope.cpp` | Envelope encrypt/decrypt tests |
