#include "cli/src/chunked.h"
#include "cli/src/commands.h"
#include "cli/src/connection.h"
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace chromatindb::cli::chunked {

// =============================================================================
// Low-level helpers (exposed; tested in test_chunked.cpp without a socket)
// =============================================================================

std::size_t read_next_chunk(std::ifstream& f, std::vector<uint8_t>& buf,
                            std::size_t max_bytes) {
    if (buf.size() < max_bytes) buf.resize(max_bytes);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(max_bytes));
    auto got = static_cast<std::size_t>(f.gcount());
    if (f.bad()) throw std::runtime_error("I/O error reading chunk");
    return got;
}

std::vector<std::array<uint8_t, 32>> plan_tombstone_targets(
    const ManifestData& manifest,
    std::span<const uint8_t, 32> manifest_hash) {
    std::vector<std::array<uint8_t, 32>> out;
    out.reserve(static_cast<size_t>(manifest.segment_count) + 1);
    for (uint32_t i = 0; i < manifest.segment_count; ++i) {
        std::array<uint8_t, 32> h{};
        std::memcpy(h.data(),
                    manifest.chunk_hashes.data() + static_cast<size_t>(i) * 32,
                    32);
        out.push_back(h);
    }
    std::array<uint8_t, 32> mh{};
    std::memcpy(mh.data(), manifest_hash.data(), 32);
    out.push_back(mh);
    return out;
}

namespace {

// Build one CDAT chunk blob's encoded FlatBuffer bytes from raw plaintext.
// The encoded blob.data layout (D-13) is:
//     [CDAT magic:4][CENV envelope(plaintext_chunk)]
// The ML-DSA-87 signature covers (ns || blob.data || ttl || timestamp).
std::vector<uint8_t> build_cdat_blob_flatbuf(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t> plaintext_chunk,
    const std::vector<std::span<const uint8_t>>& recipient_spans,
    uint32_t ttl,
    uint64_t timestamp) {

    auto cenv = envelope::encrypt(plaintext_chunk, recipient_spans);

    std::vector<uint8_t> blob_data;
    blob_data.reserve(4 + cenv.size());
    blob_data.insert(blob_data.end(), CDAT_MAGIC.begin(), CDAT_MAGIC.end());
    blob_data.insert(blob_data.end(), cenv.begin(), cenv.end());

    auto signing_input = build_signing_input(ns, blob_data, ttl, timestamp);
    auto signature     = id.sign(signing_input);

    BlobData blob{};
    std::memcpy(blob.namespace_id.data(), ns.data(), 32);
    blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
    blob.data      = std::move(blob_data);
    blob.ttl       = ttl;
    blob.timestamp = timestamp;
    blob.signature = std::move(signature);
    return encode_blob(blob);
}

// Build one tombstone blob's encoded FlatBuffer bytes targeting `target`.
std::vector<uint8_t> build_tombstone_flatbuf(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target,
    uint64_t timestamp) {

    auto td = make_tombstone_data(target);
    auto si = build_signing_input(ns, td, 0, timestamp);
    auto sg = id.sign(si);

    BlobData b{};
    std::memcpy(b.namespace_id.data(), ns.data(), 32);
    b.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
    b.data      = std::move(td);
    b.ttl       = 0;
    b.timestamp = timestamp;
    b.signature = std::move(sg);
    return encode_blob(b);
}

} // namespace

// =============================================================================
// put_chunked — streaming upload + manifest fan-out (D-01, D-04, D-11, D-13, D-15)
// =============================================================================

int put_chunked(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    std::vector<std::span<const uint8_t>> recipient_spans,
    const std::string& path,
    const std::string& filename,
    uint32_t ttl,
    Connection& conn,
    const cmd::ConnectOpts& opts) {

    // Open the input file and re-assert size bounds. The caller (cmd::put)
    // already checked, but put_chunked is a public helper so check defensively.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "Error: cannot open %s\n", path.c_str());
        return 1;
    }
    const auto end_pos = static_cast<uint64_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (end_pos < CHUNK_THRESHOLD_BYTES || end_pos > MAX_CHUNKED_FILE_SIZE) {
        std::fprintf(stderr,
            "Error: %s size %llu outside chunked range [%llu, %llu]\n",
            path.c_str(),
            static_cast<unsigned long long>(end_pos),
            static_cast<unsigned long long>(CHUNK_THRESHOLD_BYTES),
            static_cast<unsigned long long>(MAX_CHUNKED_FILE_SIZE));
        return 1;
    }

    const uint32_t chunk_size = CHUNK_SIZE_BYTES_DEFAULT;
    const uint64_t num_chunks_u64 = (end_pos + chunk_size - 1) / chunk_size;
    if (num_chunks_u64 == 0 || num_chunks_u64 > MAX_CHUNKS) {
        std::fprintf(stderr,
            "Error: %s would require %llu chunks (max %u)\n",
            path.c_str(),
            static_cast<unsigned long long>(num_chunks_u64), MAX_CHUNKS);
        return 1;
    }
    const uint32_t num_chunks = static_cast<uint32_t>(num_chunks_u64);

    // Per P-119-05: one timestamp + one ttl, shared across every CDAT and
    // the manifest, so the manifest never outlives its chunks.
    const uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

    std::vector<uint8_t> buf;           // reusable plaintext read buffer
    buf.reserve(chunk_size);
    Sha3Hasher hasher;                  // D-04 whole-file SHA3

    std::vector<uint8_t> chunk_hashes(static_cast<size_t>(num_chunks) * 32, 0);

    // Phase 120-02 two-phase pump.
    // Phase A: greedy-fill up to Connection::kPipelineDepth.
    // Phase B: drain one WriteAck via conn.recv() in arrival order, map
    //          the rid back to chunk_index, copy the final blob_hash into
    //          chunk_hashes at [chunk_index * 32 .. *32 + 32).
    uint32_t rid = 1;
    std::unordered_map<uint32_t, uint32_t> rid_to_chunk_index;

    uint32_t next_chunk = 0;
    uint32_t completed  = 0;

    while (completed < num_chunks) {
        // Phase A
        if (next_chunk < num_chunks &&
            rid_to_chunk_index.size() < Connection::kPipelineDepth) {

            const uint64_t off = static_cast<uint64_t>(next_chunk) * chunk_size;
            f.clear();
            f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
            auto got = read_next_chunk(f, buf, chunk_size);
            if (got == 0) {
                std::fprintf(stderr,
                    "Error: premature EOF at chunk %u of %s\n",
                    next_chunk, path.c_str());
                return 1;
            }
            // D-04 / CHUNK-05: absorb plaintext before envelope::encrypt
            // releases the buffer reference.
            std::span<const uint8_t> pt(buf.data(), got);
            hasher.absorb(pt);

            auto flatbuf = build_cdat_blob_flatbuf(id, ns, pt, recipient_spans,
                                                    ttl, timestamp);

            const uint32_t this_rid = rid++;
            bool sent = conn.send_async(MsgType::Data, flatbuf, this_rid);
            if (!sent) {
                // D-15: transient failure — retry up to RETRY_ATTEMPTS with
                // exponential backoff. Re-read the same slice (file on disk
                // is authoritative) and re-sign freshly (ML-DSA-87 is
                // non-deterministic so blob_hash differs each attempt). Bind
                // chunk_index -> final_hash at drain time, not here.
                bool recovered = false;
                for (int attempt = 0; attempt < RETRY_ATTEMPTS; ++attempt) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(RETRY_BACKOFF_MS[attempt]));
                    f.clear();
                    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
                    got = read_next_chunk(f, buf, chunk_size);
                    pt = std::span<const uint8_t>(buf.data(), got);
                    flatbuf = build_cdat_blob_flatbuf(id, ns, pt,
                                                      recipient_spans,
                                                      ttl, timestamp);
                    if (conn.send_async(MsgType::Data, flatbuf, this_rid)) {
                        recovered = true;
                        break;
                    }
                }
                if (!recovered) {
                    std::fprintf(stderr,
                        "Error: chunk %u send failed after %d attempts\n",
                        next_chunk, RETRY_ATTEMPTS);
                    return 1;
                }
            }
            rid_to_chunk_index[this_rid] = next_chunk;
            ++next_chunk;
            continue;  // keep filling before draining
        }

        // Phase B: drain one WriteAck.
        auto resp = conn.recv();
        if (!resp) {
            std::fprintf(stderr,
                "Error: connection lost during chunked upload of %s\n",
                path.c_str());
            return 1;
        }
        auto it = rid_to_chunk_index.find(resp->request_id);
        if (it == rid_to_chunk_index.end()) {
            spdlog::debug("put_chunked: discarding reply for unknown rid {}",
                          resp->request_id);
            continue;
        }
        const uint32_t chunk_idx = it->second;
        rid_to_chunk_index.erase(it);
        ++completed;

        if (resp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
            resp->payload.size() < 41) {
            std::fprintf(stderr,
                "Error: bad WriteAck for chunk %u of %s\n",
                chunk_idx, path.c_str());
            return 1;
        }
        // Bind chunk_index -> final blob_hash (post-WriteAck, D-15).
        std::memcpy(chunk_hashes.data() + static_cast<size_t>(chunk_idx) * 32,
                    resp->payload.data(), 32);

        if (!opts.quiet) {
            std::fprintf(stderr, "chunk %u/%u saved\n", completed, num_chunks);
        }
    }

    // All chunk hashes are bound. Build manifest.
    ManifestData m;
    m.version               = MANIFEST_VERSION_V1;
    m.chunk_size_bytes      = chunk_size;
    m.segment_count         = num_chunks;
    m.total_plaintext_bytes = end_pos;
    m.plaintext_sha3        = hasher.finalize();
    m.chunk_hashes          = std::move(chunk_hashes);
    m.filename              = filename;

    auto manifest_payload = encode_manifest_payload(m);     // [CPAR magic][FB]
    auto manifest_cenv    = envelope::encrypt(manifest_payload, recipient_spans);

    std::vector<uint8_t> manifest_blob_data;
    manifest_blob_data.reserve(4 + manifest_cenv.size());
    manifest_blob_data.insert(manifest_blob_data.end(),
                              CPAR_MAGIC.begin(), CPAR_MAGIC.end());
    manifest_blob_data.insert(manifest_blob_data.end(),
                              manifest_cenv.begin(), manifest_cenv.end());

    auto msi = build_signing_input(ns, manifest_blob_data, ttl, timestamp);
    auto msig = id.sign(msi);

    BlobData mb{};
    std::memcpy(mb.namespace_id.data(), ns.data(), 32);
    mb.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
    mb.data      = std::move(manifest_blob_data);
    mb.ttl       = ttl;
    mb.timestamp = timestamp;
    mb.signature = std::move(msig);

    auto mfb = encode_blob(mb);
    const uint32_t mrid = rid++;
    if (!conn.send_async(MsgType::Data, mfb, mrid)) {
        std::fprintf(stderr, "Error: failed to send manifest for %s\n",
                     path.c_str());
        return 1;
    }
    auto mresp = conn.recv_for(mrid);
    if (!mresp || mresp->type != static_cast<uint8_t>(MsgType::WriteAck) ||
        mresp->payload.size() < 41) {
        std::fprintf(stderr, "Error: bad manifest WriteAck for %s\n",
                     path.c_str());
        return 1;
    }
    auto mhash = std::span<const uint8_t>(mresp->payload.data(), 32);
    auto mhex  = to_hex(mhash);
    if (!filename.empty()) {
        std::printf("%s  %s\n", mhex.c_str(), filename.c_str());
    } else {
        std::printf("%s\n", mhex.c_str());
    }
    if (!opts.quiet) {
        std::fprintf(stderr, "saved: %s (%llu bytes, %u chunks)\n",
                     filename.empty() ? "<stdin>" : filename.c_str(),
                     static_cast<unsigned long long>(end_pos), num_chunks);
    }
    return 0;
}

// =============================================================================
// rm_chunked — cascade delete (D-09 chunks-first-manifest-last, D-10 idempotent)
// =============================================================================

int rm_chunked(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    const ManifestData& manifest,
    std::span<const uint8_t, 32> manifest_hash,
    Connection& conn,
    const cmd::ConnectOpts& opts) {

    const uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

    // Plan the full tombstone order up-front. rm_chunked guarantees D-09 by
    // construction: chunk_hashes[0..N) first, manifest_hash last. The plan
    // function is also unit-tested without a socket.
    auto targets = plan_tombstone_targets(manifest, manifest_hash);
    // Split into chunk-phase targets and the single manifest target.
    // The last element in targets is manifest_hash (D-09).
    const size_t N = targets.size() - 1;

    // Phase 1: pipelined chunk tombstones (D-09).
    uint32_t rid = 1;
    std::unordered_map<uint32_t, size_t> rid_to_chunk_index;
    size_t next = 0;
    size_t completed = 0;
    int errors = 0;

    while (completed < N) {
        if (next < N &&
            rid_to_chunk_index.size() < Connection::kPipelineDepth) {
            std::span<const uint8_t, 32> t(targets[next].data(), 32);
            auto fb = build_tombstone_flatbuf(id, ns, t, timestamp);
            const uint32_t r = rid++;
            if (!conn.send_async(MsgType::Delete, fb, r)) {
                std::fprintf(stderr,
                    "Error: failed to send tombstone for chunk %zu\n", next);
                ++errors;
                ++completed;
                ++next;
                continue;
            }
            rid_to_chunk_index[r] = next;
            ++next;
            continue;
        }

        auto resp = conn.recv();
        if (!resp) {
            std::fprintf(stderr,
                "Error: connection lost during chunk cascade-delete\n");
            return 1;
        }
        auto it = rid_to_chunk_index.find(resp->request_id);
        if (it == rid_to_chunk_index.end()) {
            spdlog::debug("rm_chunked: discarding reply for unknown rid {}",
                          resp->request_id);
            continue;
        }
        const size_t idx = it->second;
        rid_to_chunk_index.erase(it);
        ++completed;

        if (resp->type != static_cast<uint8_t>(MsgType::DeleteAck) ||
            resp->payload.size() < 41) {
            std::fprintf(stderr,
                "Error: bad DeleteAck for chunk %zu\n", idx);
            ++errors;
        } else if (!opts.quiet) {
            std::fprintf(stderr, "chunk %zu/%zu tombstoned\n", completed, N);
        }
    }

    if (errors != 0) {
        std::fprintf(stderr,
            "Error: %d chunk tombstone(s) failed; manifest NOT tombstoned "
            "(idempotent retry safe)\n", errors);
        return 1;
    }

    // Phase 2: tombstone the manifest LAST (D-09).
    std::span<const uint8_t, 32> mhs(targets.back().data(), 32);
    auto mfb = build_tombstone_flatbuf(id, ns, mhs, timestamp);
    const uint32_t mrid = rid++;
    if (!conn.send_async(MsgType::Delete, mfb, mrid)) {
        std::fprintf(stderr, "Error: failed to send manifest tombstone\n");
        return 1;
    }
    auto mresp = conn.recv_for(mrid);
    if (!mresp || mresp->type != static_cast<uint8_t>(MsgType::DeleteAck) ||
        mresp->payload.size() < 41) {
        std::fprintf(stderr, "Error: bad DeleteAck for manifest\n");
        return 1;
    }
    if (!opts.quiet) std::fprintf(stderr, "manifest tombstoned\n");
    return 0;
}

} // namespace chromatindb::cli::chunked
