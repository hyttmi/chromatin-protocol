#pragma once

#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <span>

namespace chromatindb::cli {

class Connection;  // fwd-decl; included in .cpp

// =============================================================================
// D-01: Invocation-scoped PUBK presence check
// =============================================================================
//
// On the first call for a given target_namespace within this process, probes
// the node via ListRequest + type_filter=PUBKEY_MAGIC + limit=1. If no PUBK
// exists AND target_namespace == SHA3(id.signing_pubkey()) (owner write),
// emits a PUBK blob via BlobWrite=64 and waits for WriteAck.
//
// Delegate writes (target_ns != id.namespace_id()) return true immediately
// with zero wire operations — PUBK registration is the owner's responsibility
// (D-01a); node cleanly rejects delegate-first-write via
// ERROR_PUBK_FIRST_VIOLATION (plan 04 surfaces this).
//
// Second + subsequent calls for the same target_namespace return true
// immediately (cache hit).
//
// The probe+emit sequence is strictly synchronous (RESEARCH Pitfall #7):
// the user's subsequent blob write MUST NOT race the PUBK WriteAck.
//
// @param id           caller's identity (used to sign + identify ownership)
// @param conn         synchronous send/recv connection (NOT async)
// @param target_ns    the namespace the caller is about to write to
// @param rid_counter  in/out; advanced by 1 (probe only on hit) or 2
//                     (probe + emit on fresh-namespace owner writes),
//                     or unchanged (delegate / cache hit).
// @returns true on success (PUBK either present or freshly emitted);
//          false on hard transport error or node protocol error.
bool ensure_pubk(const Identity& id,
                 Connection& conn,
                 std::span<const uint8_t, 32> target_namespace,
                 uint32_t& rid_counter);

/// Test-only: clear the process-local PUBK-presence cache between TEST_CASEs.
/// No-op in production call sites.
void reset_pubk_presence_cache_for_tests();

// =============================================================================
// Option A (testability): templated probe+emit core
// =============================================================================
//
// `ensure_pubk_impl` is the transport-agnostic probe+emit loop. The public
// `ensure_pubk` wrapper owns the file-scope cache + delegate-skip check;
// this template runs ONLY the wire logic so it can be unit-tested with
// mock Sender / Receiver callables (see cli/tests/test_auto_pubk.cpp).
//
//   Sender:   bool(MsgType, std::span<const uint8_t>, uint32_t)
//   Receiver: std::optional<DecodedTransport>()
//
// Returns true if PUBK is present (probe hit) or successfully emitted
// (probe miss + WriteAck). Returns false on any transport failure.
// Advances `rid_counter` by 1 on probe-only success, 2 on emit success.
template <typename Sender, typename Receiver>
bool ensure_pubk_impl(const Identity& id,
                      std::span<const uint8_t, 32> target_namespace,
                      Sender&& send,
                      Receiver&& recv,
                      uint32_t& rid_counter) {
    // Build 49-byte ListRequest payload (layout copied from
    // cli/src/commands.cpp:150-198 find_pubkey_blob).
    //   [0..31]   target_namespace
    //   [32..39]  since_seq = 0
    //   [40..43]  limit     = 1 BE
    //   [44]      flags     = 0x02 (type_filter present)
    //   [45..48]  type      = PUBKEY_MAGIC ("PUBK")
    std::vector<uint8_t> list_payload(49, 0);
    std::memcpy(list_payload.data(), target_namespace.data(), 32);
    // since_seq [32..39] stays zero-initialized
    store_u32_be(list_payload.data() + 40, 1);
    list_payload[44] = 0x02;
    std::memcpy(list_payload.data() + 45, PUBKEY_MAGIC.data(), 4);

    const uint32_t probe_rid = rid_counter++;
    if (!send(MsgType::ListRequest, std::span<const uint8_t>(list_payload),
              probe_rid)) {
        return false;
    }

    auto resp = recv();
    if (!resp ||
        resp->type != static_cast<uint8_t>(MsgType::ListResponse) ||
        resp->payload.size() < 5) {
        return false;
    }

    uint32_t count = load_u32_be(resp->payload.data());
    if (count > 0) {
        return true;  // PUBK present; no emit needed
    }

    // No PUBK present — emit one via BlobWrite=64 (template copied from
    // cmd::publish in cli/src/commands.cpp).
    auto pubkey_data = make_pubkey_data(id.signing_pubkey(), id.kem_pubkey());
    auto timestamp   = static_cast<uint64_t>(std::time(nullptr));
    auto ns_blob     = build_owned_blob(id, target_namespace, pubkey_data, 0, timestamp);
    auto envelope    = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

    const uint32_t emit_rid = rid_counter++;
    if (!send(MsgType::BlobWrite, std::span<const uint8_t>(envelope),
              emit_rid)) {
        return false;
    }

    auto ack = recv();
    if (!ack ||
        ack->type != static_cast<uint8_t>(MsgType::WriteAck) ||
        ack->payload.size() < 32) {
        return false;
    }

    return true;
}

}  // namespace chromatindb::cli
