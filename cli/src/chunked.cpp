#include "cli/src/chunked.h"
#include "cli/src/commands.h"
#include "cli/src/connection.h"
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/pubk_presence.h"
#include "cli/src/wire.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

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

// RAII guard that unlinks `path` on destruction unless release() was called.
// WR-03 fix: any throw between ::close(fd) and verify_plaintext_sha3
// in get_chunked (e.g. std::bad_alloc in the Sha3Hasher constructor) must not
// leave a partial output file on disk (D-12). release() is called ONLY on the
// successful-verify exit of get_chunked.
struct UnlinkGuard {
    std::string path;
    bool armed = true;
    explicit UnlinkGuard(std::string p) : path(std::move(p)) {}
    ~UnlinkGuard() {
        if (armed && !path.empty()) {
            ::unlink(path.c_str());
        }
    }
    UnlinkGuard(const UnlinkGuard&) = delete;
    UnlinkGuard& operator=(const UnlinkGuard&) = delete;
    void release() noexcept { armed = false; }
};

// Build one CDAT chunk blob as a BlobWriteBody envelope ready to ship under
// MsgType::BlobWrite=64. The encoded blob.data layout (D-13) is:
//     [CDAT magic:4][CENV envelope(plaintext_chunk)]
// The ML-DSA-87 signature covers (ns || blob.data || ttl || timestamp).
// Post-124: returns BlobWriteBody envelope bytes (not bare Blob bytes).
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

    auto ns_blob = build_owned_blob(id, ns, blob_data, ttl, timestamp);
    return encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);
}

// Build one tombstone blob as a BlobWriteBody envelope targeting `target`.
// Post-124: returns BlobWriteBody envelope bytes (not bare Blob bytes); send
// under MsgType::Delete=17 so node still emits DeleteAck (RESEARCH Q3 KEEP).
std::vector<uint8_t> build_tombstone_flatbuf(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target,
    uint64_t timestamp) {

    auto td = make_tombstone_data(target);
    auto ns_blob = build_owned_blob(id, ns, td, 0, timestamp);
    return encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);
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

    // D-01: auto-PUBK probe+emit before entering the Phase-A greedy fill loop.
    // Typically this is a cache hit (cmd::put already probed on this Connection
    // via the invocation-scoped cache), but we call ensure_pubk for path
    // independence: put_chunked is a public helper callable from other flows.
    // A dedicated rid (0x2000 range) keeps the probe separate from put_chunked's
    // local `rid` counter which starts at 1.
    {
        uint32_t pubk_rid = 0x2000;
        if (!ensure_pubk(id, conn, ns, pubk_rid)) {
            std::fprintf(stderr,
                "Error: failed to ensure namespace is published on node %s\n",
                opts.host.c_str());
            return 1;
        }
    }

    // Per P-119-05: one timestamp + one ttl, shared across every CDAT and
    // the manifest, so the manifest never outlives its chunks.
    const uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

    std::vector<uint8_t> buf;           // reusable plaintext read buffer
    buf.reserve(chunk_size);
    Sha3Hasher hasher;                  // D-04 whole-file SHA3

    std::vector<uint8_t> chunk_hashes(static_cast<size_t>(num_chunks) * 32, 0);

    // two-phase pump.
    // Phase A: greedy-fill up to Connection::kPipelineDepth.
    // Phase B: drain one WriteAck via conn.recv_next() in arrival order
    //          (CR-01: decrements in_flight_), map
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
            bool sent = conn.send_async(MsgType::BlobWrite, flatbuf, this_rid);
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
                    if (got == 0) {
                        // File truncated between initial read and retry.
                        // Fail loudly rather than sign a zero-length CDAT.
                        std::fprintf(stderr,
                            "Error: chunk %u re-read returned 0 bytes "
                            "(file truncated under us?); aborting\n",
                            next_chunk);
                        return 1;
                    }
                    pt = std::span<const uint8_t>(buf.data(), got);
                    flatbuf = build_cdat_blob_flatbuf(id, ns, pt,
                                                      recipient_spans,
                                                      ttl, timestamp);
                    if (conn.send_async(MsgType::BlobWrite, flatbuf, this_rid)) {
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

        // Phase B: drain one WriteAck. recv_next() decrements in_flight_
        // (CR-01 fix; plain recv() leaks the counter and stalls at 8
        // in-flight). See 119-REVIEW.md §CR-01.
        auto resp = conn.recv_next();
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

    auto ns_blob = build_owned_blob(id, ns, manifest_blob_data, ttl, timestamp);
    auto mfb     = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

    const uint32_t mrid = rid++;
    if (!conn.send_async(MsgType::BlobWrite, mfb, mrid)) {
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

    // pipelined chunk tombstones (D-09).
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

        // recv_next() decrements in_flight_ (CR-01 fix). Each DeleteAck drained
        // here is ONE tombstone the node has committed; releasing the pipeline
        // slot lets Phase A fire the next one. See 119-REVIEW.md §CR-01.
        auto resp = conn.recv_next();
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

    // tombstone the manifest LAST (D-09).
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

// =============================================================================
// Read side (Plan 02 — CHUNK-03 + CHUNK-05)
// =============================================================================

bool refuse_if_exists(const std::string& out_path, bool force_overwrite) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!force_overwrite && fs::exists(out_path, ec)) {
        std::fprintf(stderr,
                     "Error: %s already exists (use --force to overwrite)\n",
                     out_path.c_str());
        return false;
    }
    return true;
}

std::vector<std::array<uint8_t, 32>> plan_chunk_read_targets(
    const ManifestData& manifest) {
    std::vector<std::array<uint8_t, 32>> out;
    out.reserve(manifest.segment_count);
    for (uint32_t i = 0; i < manifest.segment_count; ++i) {
        std::array<uint8_t, 32> h{};
        std::memcpy(h.data(),
                    manifest.chunk_hashes.data() + static_cast<size_t>(i) * 32,
                    32);
        out.push_back(h);
    }
    return out;
}

bool verify_plaintext_sha3(const std::string& path,
                           std::span<const uint8_t, 32> expected) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    Sha3Hasher hasher;
    std::vector<uint8_t> buf(CHUNK_SIZE_BYTES_DEFAULT);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(buf.size()));
        auto got = static_cast<std::size_t>(f.gcount());
        if (got == 0) break;
        hasher.absorb(std::span<const uint8_t>(buf.data(), got));
    }
    if (f.bad()) return false;
    auto actual = hasher.finalize();
    return std::memcmp(actual.data(), expected.data(), 32) == 0;
}

GetDispatch classify_blob_data(std::span<const uint8_t> blob_data) {
    if (blob_data.size() >= 4 &&
        std::memcmp(blob_data.data(), CPAR_MAGIC.data(), 4) == 0) {
        return GetDispatch::CPAR;
    }
    if (blob_data.size() >= 4 &&
        std::memcmp(blob_data.data(), CDAT_MAGIC.data(), 4) == 0) {
        return GetDispatch::CDAT;
    }
    return GetDispatch::Other;
}

// =============================================================================
// get_chunked — pipelined chunked download + post-reassembly SHA3 verify
// =============================================================================
//
// D-06 cdb get auto-reassemble. D-08 arrival-order drain. D-12 unlink on
// any failure. D-15 per-chunk retry (3 attempts, 250/1000/4000 ms backoff).
// CHUNK-05 defense-in-depth plaintext_sha3 check.

int get_chunked(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    const ManifestData& manifest,
    const std::string& out_path,
    bool force_overwrite,
    Connection& conn,
    const cmd::ConnectOpts& opts) {

    // Pre-flight: overwrite guard via the factored helper (D-12 semantics).
    // MUST be the first statement — no Connection or fd access before this.
    if (!refuse_if_exists(out_path, force_overwrite)) {
        return 1;
    }

    // Open + preallocate. Focus Area 3: ftruncate up front surfaces ENOSPC
    // before we start fetching, so no bandwidth is wasted on a download that
    // cannot be persisted. O_EXCL for non-force mode belts-and-braces the
    // refuse_if_exists check above against a TOCTOU race.
    int flags = O_WRONLY | O_CREAT | (force_overwrite ? O_TRUNC : O_EXCL);
    int fd = ::open(out_path.c_str(), flags, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "Error: cannot open %s for write: %s\n",
                     out_path.c_str(), std::strerror(errno));
        return 1;
    }

    // WR-03: RAII so a throw between ::close(fd) and verify_plaintext_sha3
    // still unlinks the partial output (D-12). Only released on successful
    // verify at the end of the function.
    UnlinkGuard guard(out_path);

    if (::ftruncate(fd, static_cast<off_t>(manifest.total_plaintext_bytes)) != 0) {
        std::fprintf(stderr,
            "Error: cannot preallocate %s (%llu bytes): %s\n",
            out_path.c_str(),
            static_cast<unsigned long long>(manifest.total_plaintext_bytes),
            std::strerror(errno));
        ::close(fd);
        // Partial output unlink is handled by UnlinkGuard on scope exit.
        return 1;
    }

    auto fail_unlink = [&](const char* where, const std::string& detail = {}) -> int {
        if (detail.empty()) {
            std::fprintf(stderr, "Error: %s\n", where);
        } else {
            std::fprintf(stderr, "Error: %s: %s\n", where, detail.c_str());
        }
        ::close(fd);
        // Partial output unlink is handled by UnlinkGuard on scope exit.
        return 1;
    };

    const auto targets = plan_chunk_read_targets(manifest);
    const uint32_t N   = manifest.segment_count;

    // Two-phase pipeline (Phase 120-02 shape). rid_to_chunk_index binds rid
    // to chunk_index at send time; retries allocate a fresh rid and resend
    // the same chunk_hash — ML-DSA-87 non-determinism on the server side is
    // not an issue for reads (we are not re-signing; we are re-requesting).
    uint32_t rid = 1;
    std::unordered_map<uint32_t, uint32_t> rid_to_chunk_index;
    std::vector<int> attempts(N, 0);
    uint32_t next = 0;
    uint32_t completed = 0;

    auto send_chunk_read = [&](uint32_t chunk_idx) -> std::optional<uint32_t> {
        std::vector<uint8_t> payload(64);
        std::memcpy(payload.data(), ns.data(), 32);
        std::memcpy(payload.data() + 32, targets[chunk_idx].data(), 32);
        uint32_t this_rid = rid++;
        if (conn.send_async(MsgType::ReadRequest, payload, this_rid)) {
            return this_rid;
        }
        return std::nullopt;
    };

    // Retry a chunk that failed validation / decryption / transport.
    // Returns true if the retry was successfully queued; false if the per-
    // chunk budget is exhausted (caller must unlink + error).
    auto retry_chunk = [&](uint32_t chunk_idx) -> bool {
        // D-15: up to RETRY_ATTEMPTS (=3) retries per chunk with backoff
        // 250/1000/4000 ms. Post-index (not pre-increment) so the first
        // retry uses RETRY_BACKOFF_MS[0]=250ms — matches put_chunked
        // (cli/src/chunked.cpp:215-217). See 119-REVIEW.md §WR-02.
        if (attempts[chunk_idx] >= RETRY_ATTEMPTS) return false;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(RETRY_BACKOFF_MS[attempts[chunk_idx]]));
        ++attempts[chunk_idx];
        auto r = send_chunk_read(chunk_idx);
        if (!r) return false;
        rid_to_chunk_index[*r] = chunk_idx;
        return true;
    };

    while (completed < N) {
        // Phase A: greedy fill up to kPipelineDepth.
        if (next < N &&
            rid_to_chunk_index.size() < Connection::kPipelineDepth) {
            auto r = send_chunk_read(next);
            if (!r) {
                return fail_unlink("initial ReadRequest send failed",
                                    std::to_string(next));
            }
            rid_to_chunk_index[*r] = next;
            ++next;
            continue;
        }

        // Phase B: drain one reply in arrival order (D-08). recv_next()
        // decrements in_flight_ (CR-01 fix — plain recv() leaks the counter
        // and stalls the download at 8 in-flight ReadRequests). See
        // 119-REVIEW.md §CR-01.
        auto resp = conn.recv_next();
        if (!resp) {
            return fail_unlink("connection lost during chunked download");
        }
        auto it = rid_to_chunk_index.find(resp->request_id);
        if (it == rid_to_chunk_index.end()) {
            spdlog::debug("get_chunked: discarding reply for unknown rid {}",
                          resp->request_id);
            continue;
        }
        const uint32_t chunk_idx = it->second;
        rid_to_chunk_index.erase(it);

        // Response validation: must be a ReadResponse with payload[0] == 0x01.
        if (resp->type != static_cast<uint8_t>(MsgType::ReadResponse) ||
            resp->payload.empty() || resp->payload[0] != 0x01) {
            if (retry_chunk(chunk_idx)) continue;
            return fail_unlink("chunk read failed after retries",
                               std::to_string(chunk_idx));
        }
        auto blob_bytes = std::span<const uint8_t>(
            resp->payload.data() + 1, resp->payload.size() - 1);
        auto blob = decode_blob(blob_bytes);
        if (!blob) {
            if (retry_chunk(chunk_idx)) continue;
            return fail_unlink("chunk blob decode failed",
                               std::to_string(chunk_idx));
        }

        // D-13 outer-magic defense-in-depth: blob.data starts with CDAT_MAGIC.
        // If the node returned the wrong blob_type we are not going to retry —
        // this is a corruption / substitution indicator, not a transient error.
        if (blob->data.size() < 4 ||
            std::memcmp(blob->data.data(), CDAT_MAGIC.data(), 4) != 0) {
            return fail_unlink("chunk not tagged CDAT",
                               std::to_string(chunk_idx));
        }
        auto cenv_bytes = std::span<const uint8_t>(
            blob->data.data() + 4, blob->data.size() - 4);
        auto pt = envelope::decrypt(cenv_bytes, id.kem_seckey(), id.kem_pubkey());
        if (!pt) {
            // Decrypt failure is not a transient transport error — the
            // recipient set is fixed at put time. Fail fast.
            return fail_unlink("chunk decrypt failed",
                               std::to_string(chunk_idx));
        }

        // Bounds sanity: plaintext length must match chunk_size_bytes except
        // for the last chunk, which may be short.
        const uint64_t expected_off = static_cast<uint64_t>(chunk_idx) *
                                       manifest.chunk_size_bytes;
        uint64_t expected_len = manifest.chunk_size_bytes;
        if (chunk_idx + 1 == N) {
            expected_len = manifest.total_plaintext_bytes - expected_off;
        }
        if (pt->size() != expected_len) {
            return fail_unlink("chunk plaintext length mismatch",
                               std::to_string(chunk_idx));
        }

        // D-12: pwrite at offset — out-of-order arrival safe.
        ssize_t w = ::pwrite(fd, pt->data(), pt->size(),
                             static_cast<off_t>(expected_off));
        if (w < 0 || static_cast<uint64_t>(w) != pt->size()) {
            return fail_unlink("pwrite failed", std::strerror(errno));
        }
        ++completed;
        if (!opts.quiet) {
            std::fprintf(stderr, "chunk %u/%u saved\n", completed, N);
        }
    }

    // Flush + close before re-reading for the integrity check.
    if (::fsync(fd) != 0) {
        // fsync failure is unusual; fall through to the re-read anyway — if
        // the data is genuinely wrong the sha3 check will catch it. We do
        // NOT fail here because the data may still be in page cache.
        spdlog::warn("get_chunked: fsync({}) returned errno={}",
                     out_path, errno);
    }
    ::close(fd);

    // CHUNK-05 / D-04 defense-in-depth: recompute SHA3-256 over the output
    // file and compare to manifest.plaintext_sha3. Mismatch => unlink.
    if (!verify_plaintext_sha3(
            out_path,
            std::span<const uint8_t, 32>(manifest.plaintext_sha3.data(), 32))) {
        std::fprintf(stderr,
            "Error: plaintext_sha3 mismatch — aborting and removing %s\n",
            out_path.c_str());
        // Partial output unlink is handled by UnlinkGuard on scope exit.
        return 1;
    }

    if (!opts.quiet) {
        std::fprintf(stderr, "saved: %s (%llu bytes, %u chunks)\n",
            out_path.c_str(),
            static_cast<unsigned long long>(manifest.total_plaintext_bytes), N);
    }
    guard.release();
    return 0;
}

} // namespace chromatindb::cli::chunked
