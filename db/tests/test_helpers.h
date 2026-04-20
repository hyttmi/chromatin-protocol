#pragma once

#include "db/identity/identity.h"
#include "db/util/hex.h"
#include "db/wire/codec.h"
#include "db/crypto/hash.h"
#include "db/storage/storage.h"
#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <array>
#include <span>

namespace chromatindb::test {

namespace fs = std::filesystem;

/// RAII temporary directory for tests.
/// Generates a random path in the system temp directory.
/// Note: Does NOT create the directory in constructor (caller must create if needed).
struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& prefix = "chromatindb_test_") {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        path = fs::temp_directory_path() / (prefix + std::to_string(dist(gen)));
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = default;
    TempDir& operator=(TempDir&&) = default;
};

/// Run an awaitable synchronously using a temporary io_context.
/// Used in tests to call async methods from a synchronous test case.
template <typename T>
T run_async(asio::thread_pool& pool, asio::awaitable<T> aw) {
    asio::io_context ioc;
    T result{};
    asio::co_spawn(ioc, [&result, a = std::move(aw)]() mutable -> asio::awaitable<T> {
        result = co_await std::move(a);
        co_return result;
    }, asio::detached);
    ioc.run();
    return result;
}

/// Get current Unix timestamp in seconds.
inline uint64_t current_timestamp() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/// Sentinel value: pass this as timestamp to auto-use current system time.
constexpr uint64_t TS_AUTO = UINT64_MAX;

/// Build a properly signed BlobData using a NodeIdentity (Phase 122 shape).
/// signer_hint = SHA3(signing pubkey); for owner writes this equals namespace_id.
/// target_namespace (= id.namespace_id()) is carried by the caller and threaded
/// into the signing input per D-01.
inline wire::BlobData make_signed_blob(
    const identity::NodeIdentity& id,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    // For owner writes, target_namespace = SHA3(owner_pk) = signer_hint = id.namespace_id().
    auto signing_input = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

/// Build a properly signed tombstone BlobData for deleting a blob by its content hash.
inline wire::BlobData make_signed_tombstone(
    const identity::NodeIdentity& id,
    const std::array<uint8_t, 32>& target_blob_hash,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData tombstone;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(tombstone.signer_hint.data(), hint.data(), 32);
    tombstone.data = wire::make_tombstone_data(target_blob_hash);
    tombstone.ttl = 0;  // Permanent
    tombstone.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        id.namespace_id(), tombstone.data, tombstone.ttl, tombstone.timestamp);
    tombstone.signature = id.sign(signing_input);

    return tombstone;
}

/// Build a properly signed delegation BlobData: owner delegates to delegate.
inline wire::BlobData make_signed_delegation(
    const identity::NodeIdentity& owner,
    const identity::NodeIdentity& delegate,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(owner.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = wire::make_delegation_data(delegate.public_key());
    blob.ttl = 0;  // Permanent
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        owner.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = owner.sign(signing_input);

    return blob;
}

/// Build a properly signed blob written by a delegate to an owner's namespace.
/// D-01 cross-namespace replay defense: signer is the delegate (signer_hint =
/// SHA3(delegate_pk)) but target_namespace is the OWNER's namespace.
inline wire::BlobData make_delegate_blob(
    const identity::NodeIdentity& owner,
    const identity::NodeIdentity& delegate,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    // Delegate signs; signer_hint = SHA3(delegate's signing pubkey).
    auto hint = crypto::sha3_256(delegate.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    // target_namespace = OWNER's namespace (D-01 cross-namespace replay protection).
    auto signing_input = wire::build_signing_input(
        owner.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = delegate.sign(signing_input);

    return blob;
}

/// Build a PUBK blob for testing (Phase 122 D-03 / D-05).
/// Body layout: [magic:4][signing_pk:2592][kem_pk:1568] = 4164 bytes.
/// If kem_pk is nullopt, the KEM portion is zero-filled (sufficient for tests
/// that don't exercise KEM). Pass an explicit kem_pk for KEM-rotation tests.
///
/// target_namespace = id.namespace_id() = SHA3(signing_pk) = signer_hint
/// (owner self-publication).
inline wire::BlobData make_pubk_blob(
    const identity::NodeIdentity& id,
    std::optional<std::array<uint8_t, 1568>> kem_pk = std::nullopt,
    uint64_t timestamp = TS_AUTO)
{
    std::vector<uint8_t> pubk_data;
    pubk_data.reserve(wire::PUBKEY_DATA_SIZE);
    pubk_data.insert(pubk_data.end(),
                     wire::PUBKEY_MAGIC.begin(), wire::PUBKEY_MAGIC.end());
    auto spk = id.public_key();
    pubk_data.insert(pubk_data.end(), spk.begin(), spk.end());
    if (kem_pk.has_value()) {
        pubk_data.insert(pubk_data.end(), kem_pk->begin(), kem_pk->end());
    } else {
        pubk_data.resize(wire::PUBKEY_DATA_SIZE, 0);  // zero-fill KEM portion
    }

    wire::BlobData blob;
    auto hint = crypto::sha3_256(spk);
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = std::move(pubk_data);
    blob.ttl = 0;  // PUBK is permanent
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    // target_namespace = id.namespace_id() = SHA3(signing_pk) = signer_hint
    auto signing_input = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

/// Format a PeerManager's actual listening address as "127.0.0.1:PORT".
/// Use after pm.start() to get the ephemeral port for bootstrap_peers.
inline std::string listening_address(uint16_t port) {
    return "127.0.0.1:" + std::to_string(port);
}

/// Phase 123 D-03: build a properly signed NAME blob pointing `name` → `target_hash`.
/// signer_hint = SHA3(id.public_key()); signing_input commits to id.namespace_id()
/// per the Phase 122 canonical form.
///
/// `name` is opaque bytes (D-04): any byte sequence up to 65535 bytes.
/// `ttl` defaults to 0 (permanent); callers may pass ttl>0 for expiring NAME blobs.
inline wire::BlobData make_name_blob(
    const identity::NodeIdentity& id,
    std::span<const uint8_t> name,
    std::span<const uint8_t, 32> target_hash,
    uint32_t ttl = 0,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = wire::make_name_data(name, target_hash);
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

/// Phase 123 D-05 / D-13(1): build a properly signed BOMB blob covering `targets`.
/// ttl is MANDATORILY 0 (D-13(1) — permanent batched tombstone); NO ttl knob.
/// A test that needs a ttl!=0 BOMB to exercise the ingest rejection path must
/// construct the BlobData manually (bypass this helper).
inline wire::BlobData make_bomb_blob(
    const identity::NodeIdentity& id,
    std::span<const std::array<uint8_t, 32>> targets,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = wire::make_bomb_data(targets);
    blob.ttl = 0;  // D-13(1) mandatory — BOMB is permanent
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

/// Phase 122-07: convenience span for owner namespace_id.
/// Many tests construct a NodeIdentity, then want to call
/// `engine.ingest(ns_span(id), blob)`. The cascade replaces hundreds of
/// pre-122 `engine.ingest(blob)` calls where target_namespace = signer_hint
/// = SHA3(id.public_key()) = id.namespace_id() for owner writes.
inline std::span<const uint8_t, 32> ns_span(const identity::NodeIdentity& id) {
    return id.namespace_id();
}

/// Phase 122-07: convenience span for a raw 32-byte namespace array.
inline std::span<const uint8_t, 32> ns_span(const std::array<uint8_t, 32>& ns) {
    return std::span<const uint8_t, 32>(ns);
}

/// Phase 122-07: register a PUBK directly into storage's owner_pubkeys DBI,
/// bypassing engine.ingest. Use when tests need the PUBK-first gate to pass
/// WITHOUT the PUBK occupying a seq slot or counting toward quotas/capacity.
/// This is the analog of what engine.cpp Step 4.5 does post-verify — but we
/// skip the blob storage entirely for test setup.
inline void register_pubk(
    storage::Storage& store,
    const identity::NodeIdentity& id)
{
    auto pk = id.public_key();  // 2592-byte span
    auto hint = crypto::sha3_256(pk);
    store.register_owner_pubkey(
        std::span<const uint8_t, 32>(hint),
        std::span<const uint8_t, 2592>(pk.data(), 2592));
}

} // namespace chromatindb::test
