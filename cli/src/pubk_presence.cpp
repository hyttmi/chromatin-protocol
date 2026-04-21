#include "cli/src/pubk_presence.h"

#include "cli/src/connection.h"
#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <array>
#include <cstring>
#include <unordered_set>

namespace chromatindb::cli {
namespace {

// Hash functor for std::unordered_set<std::array<uint8_t, 32>>.
// XOR-fold 4 x uint64_t slices — good enough for a small in-proc set
// (no adversarial keys; target_namespace is already a SHA3 digest).
struct Ns32Hash {
    size_t operator()(const std::array<uint8_t, 32>& a) const noexcept {
        uint64_t h = 0;
        for (size_t i = 0; i < 32; i += 8) {
            uint64_t w;
            std::memcpy(&w, a.data() + i, 8);
            h ^= w + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return static_cast<size_t>(h);
    }
};

// Process-lifetime cache of namespaces known to have PUBK on the node this
// process has spoken to. Deliberately NOT persisted (CONTEXT deferred idea).
// cdb is single-threaded; no mutex needed. T-124-cache-01 accepts the
// ephemeral-cache threat model.
std::unordered_set<std::array<uint8_t, 32>, Ns32Hash>& pubk_cache() {
    static std::unordered_set<std::array<uint8_t, 32>, Ns32Hash> s;
    return s;
}

}  // namespace

void reset_pubk_presence_cache_for_tests() {
    pubk_cache().clear();
}

void mark_pubk_present_for_invocation(std::span<const uint8_t, 32> target_namespace) {
    std::array<uint8_t, 32> ns_arr{};
    std::memcpy(ns_arr.data(), target_namespace.data(), 32);
    pubk_cache().insert(ns_arr);
}

bool ensure_pubk(const Identity& id,
                 Connection& conn,
                 std::span<const uint8_t, 32> target_namespace,
                 uint32_t& rid_counter) {
    // Capture target_namespace as a concrete array for cache lookups.
    std::array<uint8_t, 32> ns_arr{};
    std::memcpy(ns_arr.data(), target_namespace.data(), 32);

    // Cache hit — zero wire ops.
    if (pubk_cache().count(ns_arr) > 0) {
        return true;
    }

    // D-01a: delegate writes skip auto-PUBK entirely. Owner is determined by
    // target_ns == SHA3(id.signing_pubkey()) == id.namespace_id().
    auto own_ns = id.namespace_id();
    if (std::memcmp(own_ns.data(), target_namespace.data(), 32) != 0) {
        return true;  // delegate — PUBK is the owner's responsibility
    }

    // Owner write on a cache miss: bind Connection to the template's
    // transport-agnostic core. RESEARCH Pitfall #7: strictly serial
    // send/recv; do NOT use send_async/recv_for.
    auto send_fn = [&](MsgType type, std::span<const uint8_t> payload,
                       uint32_t rid) {
        return conn.send(type, payload, rid);
    };
    auto recv_fn = [&]() { return conn.recv(); };

    bool ok = ensure_pubk_impl(id, target_namespace, send_fn, recv_fn,
                               rid_counter);
    if (ok) {
        pubk_cache().insert(ns_arr);
    }
    return ok;
}

}  // namespace chromatindb::cli
