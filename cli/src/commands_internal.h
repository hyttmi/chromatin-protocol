#pragma once

// Internal shared surface between cli/src/commands.cpp and unit tests (used by
// plan 124-05 [error_decoder] and 124-04 [cascade] TEST_CASEs). Declarations
// here are NOT part of the cdb CLI public API — consumers must #include this
// header explicitly.

#include "cli/src/connection.h"
#include "cli/src/identity.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::cli {

// =============================================================================
// D-05: ErrorResponse decoder (exported for [error_decoder] unit TEST_CASE)
// =============================================================================
//
// Decode an ErrorResponse payload into user-facing wording.
// Payload layout (post-Phase 122/123): [error_code:1][original_type:1].
// Deliberately avoids leaking internal tokens (PUBK_FIRST_VIOLATION,
// PUBK_MISMATCH) or phase numbers into the user-visible message — see
// feedback_no_phase_leaks_in_user_strings.md.
std::string decode_error_response(std::span<const uint8_t> payload,
                                   const std::string& host_hint,
                                   std::span<const uint8_t, 32> ns_hint);

// =============================================================================
// D-06: CPAR cascade classification for cmd::rm / cmd::rm_batch
// =============================================================================

/// Classification of a single rm target. Consumers:
///   - `cmd::rm` single-target: Plain / Cdat / CparWithChunks dispatch.
///   - `cmd::rm_batch`: aggregate classifications into the BOMB target set,
///     expanding CparWithChunks' chunk_hashes into additional tombstones.
struct RmClassification {
    enum class Kind {
        Plain,             // non-chunked target; single tombstone suffices
        Cdat,              // raw CDAT chunk; operator error, refuse to cascade
        CparWithChunks,    // CPAR manifest; cascade_targets holds chunk hashes
        FetchFailed        // ReadRequest / decrypt / parse failed; caller
                           // decides warn-and-continue vs abort (RESEARCH Q6)
    };
    Kind kind = Kind::Plain;
    std::vector<std::array<uint8_t, 32>> cascade_targets;
};

/// Production surface: classify a single rm target against a live Connection.
/// Extracted from the CPAR-detection block in `cmd::rm` single-target so that
/// `cmd::rm_batch` can reuse the exact same logic (feedback_no_duplicate_code).
///
/// Advances `rid_counter` by 1 (ReadRequest only).
RmClassification classify_rm_target(
    Identity& id,
    Connection& conn,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target_hash,
    uint32_t& rid_counter);

/// Template form — transport-agnostic core for unit-test mockability.
/// Mirrors plan 02's `ensure_pubk_impl<Sender, Receiver>` pattern exactly:
/// the production `classify_rm_target` wrapper binds `conn.send`/`conn.recv`
/// to this template, while unit tests drive it with CapturingSender +
/// ScriptedSource, avoiding real asio::io_context entirely.
///
///   Sender:   bool(MsgType, std::span<const uint8_t>, uint32_t)
///   Receiver: std::optional<DecodedTransport>()
///
/// The template body is in this header (below the declaration) so test
/// translation units can instantiate it without linking commands.cpp.
template <typename Sender, typename Receiver>
RmClassification classify_rm_target_impl(
    Identity& id,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target_hash,
    Sender&& send,
    Receiver&& recv,
    uint32_t& rid_counter);

} // namespace chromatindb::cli

// Template implementation lives here so tests can instantiate without
// depending on commands.cpp.

#include "cli/src/envelope.h"
#include "cli/src/wire.h"

#include <cstring>

namespace chromatindb::cli {

template <typename Sender, typename Receiver>
RmClassification classify_rm_target_impl(
    Identity& id,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target_hash,
    Sender&& send,
    Receiver&& recv,
    uint32_t& rid_counter) {

    RmClassification rc;

    // Issue a ReadRequest for the target.
    // Payload layout (matches existing cmd::rm at commands.cpp:1134-1146):
    //   [0..31]  namespace_id
    //   [32..63] target_hash
    std::vector<uint8_t> read_payload(64);
    std::memcpy(read_payload.data(),       ns.data(),          32);
    std::memcpy(read_payload.data() + 32,  target_hash.data(), 32);

    const uint32_t read_rid = rid_counter++;
    if (!send(MsgType::ReadRequest, std::span<const uint8_t>(read_payload),
              read_rid)) {
        rc.kind = RmClassification::Kind::FetchFailed;
        return rc;
    }

    auto resp = recv();
    if (!resp ||
        resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
        resp->payload.empty() ||
        resp->payload[0] != 0x01) {
        rc.kind = RmClassification::Kind::FetchFailed;
        return rc;
    }

    // ReadResponse payload is [status:1][blob:FlatBuffer] — status==0x01
    // means hit. The blob FlatBuffer starts at payload[1].
    auto blob_bytes = std::span<const uint8_t>(
        resp->payload.data() + 1, resp->payload.size() - 1);
    auto target_blob = decode_blob(blob_bytes);
    if (!target_blob || target_blob->data.size() < 4) {
        rc.kind = RmClassification::Kind::Plain;
        return rc;
    }

    // Magic byte dispatch (mirrors the block at cmd::rm commands.cpp:1150-1188).
    if (std::memcmp(target_blob->data.data(), CDAT_MAGIC.data(), 4) == 0) {
        rc.kind = RmClassification::Kind::Cdat;
        return rc;
    }
    if (std::memcmp(target_blob->data.data(), CPAR_MAGIC.data(), 4) == 0) {
        auto envelope_bytes = std::span<const uint8_t>(
            target_blob->data.data() + 4,
            target_blob->data.size() - 4);
        auto plain = envelope::decrypt(envelope_bytes,
                                        id.kem_seckey(),
                                        id.kem_pubkey());
        if (!plain) {
            rc.kind = RmClassification::Kind::FetchFailed;
            return rc;
        }
        auto manifest = decode_manifest_payload(*plain);
        if (!manifest) {
            rc.kind = RmClassification::Kind::FetchFailed;
            return rc;
        }
        // Extract chunk hashes (32-byte stride) from the manifest.
        rc.cascade_targets.reserve(manifest->segment_count);
        for (uint32_t i = 0; i < manifest->segment_count; ++i) {
            std::array<uint8_t, 32> h{};
            std::memcpy(h.data(),
                        manifest->chunk_hashes.data() +
                            static_cast<size_t>(i) * 32,
                        32);
            rc.cascade_targets.push_back(h);
        }
        rc.kind = RmClassification::Kind::CparWithChunks;
        return rc;
    }

    // Neither CDAT nor CPAR — plain single-blob target.
    rc.kind = RmClassification::Kind::Plain;
    return rc;
}

} // namespace chromatindb::cli
