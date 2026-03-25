#pragma once

#include "db/identity/identity.h"
#include "db/util/hex.h"
#include "db/wire/codec.h"
#include "db/crypto/hash.h"
#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

/// Build a properly signed BlobData using a NodeIdentity.
inline wire::BlobData make_signed_blob(
    const identity::NodeIdentity& id,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
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
    std::memcpy(tombstone.namespace_id.data(), id.namespace_id().data(), 32);
    tombstone.pubkey.assign(id.public_key().begin(), id.public_key().end());
    tombstone.data = wire::make_tombstone_data(target_blob_hash);
    tombstone.ttl = 0;  // Permanent
    tombstone.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        tombstone.namespace_id, tombstone.data, tombstone.ttl, tombstone.timestamp);
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
    std::memcpy(blob.namespace_id.data(), owner.namespace_id().data(), 32);
    blob.pubkey.assign(owner.public_key().begin(), owner.public_key().end());
    blob.data = wire::make_delegation_data(delegate.public_key());
    blob.ttl = 0;  // Permanent
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = owner.sign(signing_input);

    return blob;
}

/// Build a properly signed blob written by a delegate to an owner's namespace.
inline wire::BlobData make_delegate_blob(
    const identity::NodeIdentity& owner,
    const identity::NodeIdentity& delegate,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    // Target the owner's namespace
    std::memcpy(blob.namespace_id.data(), owner.namespace_id().data(), 32);
    // Signed by delegate's key
    blob.pubkey.assign(delegate.public_key().begin(), delegate.public_key().end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = delegate.sign(signing_input);

    return blob;
}

} // namespace chromatindb::test
