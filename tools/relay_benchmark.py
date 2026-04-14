#!/usr/bin/env python3
"""
Relay Performance Benchmark Tool

Measures HTTP relay performance across 4 workload types:
  PERF-01: Throughput (concurrent clients, small blobs)
  PERF-02: Latency overhead (per-operation latency breakdown)
  PERF-03: Large blob transfer (1, 10, 50, 100 MiB)
  PERF-04: Mixed workload (small queries + large blob transfers)

Usage:
    python3 tools/relay_benchmark.py --relay-url http://127.0.0.1:4201

Prerequisites:
    pip install httpx flatbuffers
    System: liboqs-python (oqs)

    Relay config should have:
        rate_limit_messages_per_sec = 0
        request_timeout_seconds = 0
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import os
import platform
import statistics
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ---- Dependency check --------------------------------------------------------

_MISSING: list[str] = []
try:
    import httpx
except ImportError:
    _MISSING.append("httpx  (pip install httpx)")

try:
    import flatbuffers
    from flatbuffers import builder as fb_builder
except ImportError:
    _MISSING.append("flatbuffers  (pip install flatbuffers)")

try:
    import oqs  # type: ignore[import-untyped]
except ImportError:
    _MISSING.append("liboqs-python  (system package: python-oqs or pip install liboqs-python)")

if _MISSING:
    print("ERROR: Missing required dependencies:", file=sys.stderr)
    for dep in _MISSING:
        print(f"  - {dep}", file=sys.stderr)
    sys.exit(1)

# ---- ML-DSA-87 Identity -----------------------------------------------------


class Identity:
    """Ephemeral ML-DSA-87 identity for benchmark authentication."""

    def __init__(self) -> None:
        sig = oqs.Signature("ML-DSA-87")
        self.public_key: bytes = sig.generate_keypair()
        self._secret_key: bytes = sig.export_secret_key()
        self.namespace: bytes = hashlib.sha3_256(self.public_key).digest()
        self.namespace_hex: str = self.namespace.hex()
        self.pubkey_hex: str = self.public_key.hex()
        self._sig = sig

    def sign(self, message: bytes) -> bytes:
        """Sign message bytes with ML-DSA-87."""
        return self._sig.sign(message)


# ---- Auth helper -------------------------------------------------------------


async def authenticate(
    client: httpx.AsyncClient, base_url: str, identity: Identity
) -> str:
    """Authenticate with the relay and return a bearer token.

    Flow: POST /auth/challenge -> sign nonce -> POST /auth/verify -> token
    """
    # Step 1: Get challenge nonce
    resp = await client.post(f"{base_url}/auth/challenge")
    resp.raise_for_status()
    nonce_hex: str = resp.json()["nonce"]

    # Step 2: Sign the raw nonce bytes (decoded from hex)
    nonce_bytes = bytes.fromhex(nonce_hex)
    signature = identity.sign(nonce_bytes)

    # Step 3: Verify
    resp = await client.post(
        f"{base_url}/auth/verify",
        json={
            "nonce": nonce_hex,
            "public_key": identity.pubkey_hex,
            "signature": signature.hex(),
        },
    )
    resp.raise_for_status()
    return resp.json()["token"]


# ---- FlatBuffer Blob builder -------------------------------------------------

# Blob table VTable layout (from blob.fbs field order):
#   field 0: namespace_id  ([ubyte], offset slot 4)
#   field 1: pubkey        ([ubyte], offset slot 5)
#   field 2: data          ([ubyte], offset slot 6)
#   field 3: ttl           (uint32,  slot 7)
#   field 4: timestamp     (uint64,  slot 8)
#   field 5: signature     ([ubyte], offset slot 9)

_BLOB_VTABLE_SLOT_NS = 4
_BLOB_VTABLE_SLOT_PUBKEY = 5
_BLOB_VTABLE_SLOT_DATA = 6
_BLOB_VTABLE_SLOT_TTL = 7
_BLOB_VTABLE_SLOT_TIMESTAMP = 8
_BLOB_VTABLE_SLOT_SIGNATURE = 9


def build_blob(identity: Identity, data: bytes, ttl: int = 3600) -> bytes:
    """Build a FlatBuffer-encoded Blob with ML-DSA-87 signature.

    Returns the complete FlatBuffer bytes ready for POST /blob.
    """
    timestamp = int(time.time())

    # Build signing input: SHA3-256(namespace_id || data || ttl_be32 || timestamp_be64)
    signing_input = (
        identity.namespace
        + data
        + struct.pack(">I", ttl)
        + struct.pack(">Q", timestamp)
    )
    digest = hashlib.sha3_256(signing_input).digest()
    signature = identity.sign(digest)

    # Build FlatBuffer
    builder = fb_builder.Builder(len(data) + 8192)

    # Create byte vectors (must be created before table start)
    sig_vec = builder.CreateByteVector(signature)
    data_vec = builder.CreateByteVector(data)
    pubkey_vec = builder.CreateByteVector(identity.public_key)
    ns_vec = builder.CreateByteVector(identity.namespace)

    # Start Blob table (6 fields)
    builder.StartObject(6)
    builder.PrependUOffsetTRelativeSlot(
        _BLOB_VTABLE_SLOT_NS - 4, flatbuffers.number_types.UOffsetTFlags.py_type(ns_vec), 0
    )
    builder.PrependUOffsetTRelativeSlot(
        _BLOB_VTABLE_SLOT_PUBKEY - 4, flatbuffers.number_types.UOffsetTFlags.py_type(pubkey_vec), 0
    )
    builder.PrependUOffsetTRelativeSlot(
        _BLOB_VTABLE_SLOT_DATA - 4, flatbuffers.number_types.UOffsetTFlags.py_type(data_vec), 0
    )
    builder.PrependUint32Slot(
        _BLOB_VTABLE_SLOT_TTL - 4, ttl, 0
    )
    builder.PrependUint64Slot(
        _BLOB_VTABLE_SLOT_TIMESTAMP - 4, timestamp, 0
    )
    builder.PrependUOffsetTRelativeSlot(
        _BLOB_VTABLE_SLOT_SIGNATURE - 4, flatbuffers.number_types.UOffsetTFlags.py_type(sig_vec), 0
    )
    blob = builder.EndObject()
    builder.Finish(blob)

    return bytes(builder.Output())


# ---- Latency helpers ---------------------------------------------------------


def compute_percentiles(latencies_ms: list[float]) -> dict[str, float]:
    """Compute p50, p95, p99 from a list of latencies in ms."""
    if not latencies_ms:
        return {"p50": 0.0, "p95": 0.0, "p99": 0.0}
    s = sorted(latencies_ms)
    n = len(s)

    def pct(p: float) -> float:
        idx = int(p / 100.0 * n)
        idx = min(idx, n - 1)
        return s[idx]

    return {"p50": pct(50), "p95": pct(95), "p99": pct(99)}


def ns_to_ms(ns: int) -> float:
    """Convert nanoseconds to milliseconds."""
    return ns / 1_000_000


# ---- Benchmark results -------------------------------------------------------


@dataclass
class ThroughputResult:
    concurrency: int
    total_ops: int
    wall_time_s: float
    blobs_per_sec: float
    per_client_ops_sec: float
    latencies_ms: list[float] = field(default_factory=list)
    percentiles: dict[str, float] = field(default_factory=dict)
    errors: int = 0


@dataclass
class LatencyResult:
    operation: str
    iterations: int
    latencies_ms: list[float] = field(default_factory=list)
    percentiles: dict[str, float] = field(default_factory=dict)
    errors: int = 0


@dataclass
class LargeBlobResult:
    size_mib: float
    write_time_s: float
    read_time_s: float
    write_mib_sec: float
    read_mib_sec: float
    sign_time_s: float = 0.0
    build_time_s: float = 0.0
    errors: int = 0


@dataclass
class MixedWorkloadResult:
    small_query_count: int
    large_blob_count: int
    duration_s: float
    small_latencies_ms: list[float] = field(default_factory=list)
    small_percentiles: dict[str, float] = field(default_factory=dict)
    large_latencies_ms: list[float] = field(default_factory=list)
    large_percentiles: dict[str, float] = field(default_factory=dict)
    errors: int = 0


@dataclass
class BenchmarkReport:
    date: str
    hardware: str
    relay_url: str
    throughput_results: list[ThroughputResult] = field(default_factory=list)
    latency_results: list[LatencyResult] = field(default_factory=list)
    large_blob_results: list[LargeBlobResult] = field(default_factory=list)
    mixed_baseline: Optional[MixedWorkloadResult] = None
    mixed_under_load: Optional[MixedWorkloadResult] = None


# ---- PERF-01: Throughput benchmark -------------------------------------------


async def _throughput_worker(
    client: httpx.AsyncClient,
    base_url: str,
    token: str,
    identity: Identity,
    iterations: int,
    latencies: list[float],
) -> int:
    """Single worker: sends iterations POST /blob requests, records latencies."""
    errors = 0
    headers = {"Authorization": f"Bearer {token}", "Content-Type": "application/octet-stream"}
    blob_data = os.urandom(1024)  # 1 KB payload

    for _ in range(iterations):
        blob_bytes = build_blob(identity, blob_data)
        t0 = time.perf_counter_ns()
        try:
            resp = await client.post(
                f"{base_url}/blob", content=blob_bytes, headers=headers
            )
            t1 = time.perf_counter_ns()
            if resp.status_code == 200:
                latencies.append(ns_to_ms(t1 - t0))
            else:
                errors += 1
        except Exception:
            errors += 1
    return errors


async def run_throughput_benchmark(
    base_url: str,
    identity: Identity,
    concurrency_levels: list[int],
    iterations: int,
    warmup: int,
) -> list[ThroughputResult]:
    """PERF-01: Throughput at varying concurrency levels."""
    results: list[ThroughputResult] = []

    for concurrency in concurrency_levels:
        print(f"  PERF-01: concurrency={concurrency}, iterations={iterations} per client")

        # Create clients and authenticate each (staggered to avoid thundering herd)
        clients: list[httpx.AsyncClient] = []
        tokens: list[str] = []
        for i in range(concurrency):
            try:
                c = httpx.AsyncClient(http2=False, timeout=httpx.Timeout(120.0))
                tok = await authenticate(c, base_url, identity)
                clients.append(c)
                tokens.append(tok)
            except Exception as e:
                print(f"    WARNING: client {i} auth failed: {e}")
                try:
                    await c.aclose()
                except Exception:
                    pass
        if not clients:
            print(f"    SKIP: no clients could authenticate at concurrency={concurrency}")
            continue
        concurrency = len(clients)  # Adjust for failed clients

        # Warmup (single client)
        if warmup > 0:
            warmup_lats: list[float] = []
            await _throughput_worker(
                clients[0], base_url, tokens[0], identity, warmup, warmup_lats
            )

        # Run benchmark
        all_latencies: list[list[float]] = [[] for _ in range(concurrency)]
        t_start = time.perf_counter()

        tasks = [
            _throughput_worker(
                clients[i], base_url, tokens[i], identity, iterations, all_latencies[i]
            )
            for i in range(concurrency)
        ]
        error_counts = await asyncio.gather(*tasks)

        t_end = time.perf_counter()
        wall_time = t_end - t_start

        # Aggregate
        flat_latencies = [lat for lats in all_latencies for lat in lats]
        total_ops = len(flat_latencies)
        total_errors = sum(error_counts)

        result = ThroughputResult(
            concurrency=concurrency,
            total_ops=total_ops,
            wall_time_s=wall_time,
            blobs_per_sec=total_ops / wall_time if wall_time > 0 else 0,
            per_client_ops_sec=(total_ops / concurrency / wall_time) if wall_time > 0 else 0,
            latencies_ms=flat_latencies,
            percentiles=compute_percentiles(flat_latencies),
            errors=total_errors,
        )
        results.append(result)

        print(
            f"    -> {result.blobs_per_sec:.1f} blobs/sec, "
            f"p50={result.percentiles['p50']:.1f}ms, "
            f"p99={result.percentiles['p99']:.1f}ms, "
            f"errors={total_errors}"
        )

        # Cleanup
        for c in clients:
            await c.aclose()

    return results


# ---- PERF-02: Latency benchmark ----------------------------------------------


async def run_latency_benchmark(
    base_url: str,
    identity: Identity,
    iterations: int,
    warmup: int,
    skip_uds: bool,
) -> list[LatencyResult]:
    """PERF-02: Per-operation latency measurement."""
    results: list[LatencyResult] = []

    if skip_uds:
        print("  PERF-02: UDS baseline skipped (--skip-uds). Measuring relay-only latency.")

    async with httpx.AsyncClient(http2=False, timeout=httpx.Timeout(120.0)) as client:
        token = await authenticate(client, base_url, identity)
        headers_json = {"Authorization": f"Bearer {token}"}
        headers_bin = {
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/octet-stream",
        }

        # -- Write latency (POST /blob) --
        print("  PERF-02: Measuring write latency (POST /blob)...")
        write_lats: list[float] = []
        blob_data = os.urandom(1024)
        blob_hashes: list[str] = []

        for i in range(warmup + iterations):
            blob_bytes = build_blob(identity, blob_data)
            t0 = time.perf_counter_ns()
            resp = await client.post(
                f"{base_url}/blob", content=blob_bytes, headers=headers_bin
            )
            t1 = time.perf_counter_ns()
            if resp.status_code == 200:
                if i >= warmup:
                    write_lats.append(ns_to_ms(t1 - t0))
                blob_hash = resp.json().get("blob_hash", "")
                if blob_hash:
                    blob_hashes.append(blob_hash)

        write_result = LatencyResult(
            operation="write (POST /blob)",
            iterations=len(write_lats),
            latencies_ms=write_lats,
            percentiles=compute_percentiles(write_lats),
        )
        results.append(write_result)
        print(
            f"    -> p50={write_result.percentiles['p50']:.2f}ms, "
            f"p99={write_result.percentiles['p99']:.2f}ms"
        )

        # -- Read latency (GET /blob/{ns}/{hash}) --
        if blob_hashes:
            print("  PERF-02: Measuring read latency (GET /blob/{ns}/{hash})...")
            read_lats: list[float] = []
            test_hash = blob_hashes[0]

            for i in range(warmup + iterations):
                t0 = time.perf_counter_ns()
                resp = await client.get(
                    f"{base_url}/blob/{identity.namespace_hex}/{test_hash}",
                    headers=headers_json,
                )
                t1 = time.perf_counter_ns()
                if resp.status_code == 200 and i >= warmup:
                    read_lats.append(ns_to_ms(t1 - t0))

            read_result = LatencyResult(
                operation="read (GET /blob/{ns}/{hash})",
                iterations=len(read_lats),
                latencies_ms=read_lats,
                percentiles=compute_percentiles(read_lats),
            )
            results.append(read_result)
            print(
                f"    -> p50={read_result.percentiles['p50']:.2f}ms, "
                f"p99={read_result.percentiles['p99']:.2f}ms"
            )

        # -- Exists latency (GET /exists/{ns}/{hash}) --
        if blob_hashes:
            print("  PERF-02: Measuring exists latency (GET /exists/{ns}/{hash})...")
            exists_lats: list[float] = []
            test_hash = blob_hashes[0]

            for i in range(warmup + iterations):
                t0 = time.perf_counter_ns()
                resp = await client.get(
                    f"{base_url}/exists/{identity.namespace_hex}/{test_hash}",
                    headers=headers_json,
                )
                t1 = time.perf_counter_ns()
                if resp.status_code == 200 and i >= warmup:
                    exists_lats.append(ns_to_ms(t1 - t0))

            exists_result = LatencyResult(
                operation="exists (GET /exists/{ns}/{hash})",
                iterations=len(exists_lats),
                latencies_ms=exists_lats,
                percentiles=compute_percentiles(exists_lats),
            )
            results.append(exists_result)
            print(
                f"    -> p50={exists_result.percentiles['p50']:.2f}ms, "
                f"p99={exists_result.percentiles['p99']:.2f}ms"
            )

        # -- Stats latency (GET /stats/{ns}) --
        print("  PERF-02: Measuring stats latency (GET /stats/{ns})...")
        stats_lats: list[float] = []

        for i in range(warmup + iterations):
            t0 = time.perf_counter_ns()
            resp = await client.get(
                f"{base_url}/stats/{identity.namespace_hex}",
                headers=headers_json,
            )
            t1 = time.perf_counter_ns()
            if resp.status_code == 200 and i >= warmup:
                stats_lats.append(ns_to_ms(t1 - t0))

        stats_result = LatencyResult(
            operation="stats (GET /stats/{ns})",
            iterations=len(stats_lats),
            latencies_ms=stats_lats,
            percentiles=compute_percentiles(stats_lats),
        )
        results.append(stats_result)
        print(
            f"    -> p50={stats_result.percentiles['p50']:.2f}ms, "
            f"p99={stats_result.percentiles['p99']:.2f}ms"
        )

    return results


# ---- PERF-03: Large blob benchmark -------------------------------------------


async def run_large_blob_benchmark(
    base_url: str,
    identity: Identity,
    blob_sizes: list[int],
) -> list[LargeBlobResult]:
    """PERF-03: Large blob write and read performance."""
    results: list[LargeBlobResult] = []

    async with httpx.AsyncClient(http2=False, timeout=httpx.Timeout(600.0)) as client:
        token = await authenticate(client, base_url, identity)
        headers = {
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/octet-stream",
        }
        headers_read = {"Authorization": f"Bearer {token}"}

        for size_bytes in blob_sizes:
            size_mib = size_bytes / (1024 * 1024)
            print(f"  PERF-03: {size_mib:.0f} MiB blob...")

            # Generate random data
            data = os.urandom(size_bytes)

            # Build blob (includes signing time)
            t_build_start = time.perf_counter()
            t_sign_start = time.perf_counter()
            signing_input = (
                identity.namespace
                + data
                + struct.pack(">I", 3600)
                + struct.pack(">Q", int(time.time()))
            )
            _digest = hashlib.sha3_256(signing_input).digest()
            _sig = identity.sign(_digest)
            t_sign_end = time.perf_counter()
            sign_time = t_sign_end - t_sign_start

            blob_bytes = build_blob(identity, data)
            t_build_end = time.perf_counter()
            build_time = t_build_end - t_build_start

            print(f"    Sign time: {sign_time:.2f}s, Build time: {build_time:.2f}s")

            # Write
            t0 = time.perf_counter()
            try:
                resp = await client.post(
                    f"{base_url}/blob", content=blob_bytes, headers=headers
                )
                t1 = time.perf_counter()
                write_time = t1 - t0

                if resp.status_code != 200:
                    print(f"    Write FAILED: status={resp.status_code} body={resp.text[:200]}")
                    results.append(
                        LargeBlobResult(
                            size_mib=size_mib,
                            write_time_s=write_time,
                            read_time_s=0,
                            write_mib_sec=0,
                            read_mib_sec=0,
                            sign_time_s=sign_time,
                            build_time_s=build_time,
                            errors=1,
                        )
                    )
                    continue

                blob_hash = resp.json().get("blob_hash", "")
                write_mib_sec = size_mib / write_time if write_time > 0 else 0
                print(f"    Write: {write_time:.2f}s ({write_mib_sec:.1f} MiB/s)")
            except Exception as exc:
                print(f"    Write EXCEPTION: {exc}")
                results.append(
                    LargeBlobResult(
                        size_mib=size_mib,
                        write_time_s=0,
                        read_time_s=0,
                        write_mib_sec=0,
                        read_mib_sec=0,
                        sign_time_s=sign_time,
                        build_time_s=build_time,
                        errors=1,
                    )
                )
                continue

            # Read
            t0 = time.perf_counter()
            try:
                resp = await client.get(
                    f"{base_url}/blob/{identity.namespace_hex}/{blob_hash}",
                    headers=headers_read,
                )
                t1 = time.perf_counter()
                read_time = t1 - t0

                if resp.status_code != 200:
                    print(f"    Read FAILED: status={resp.status_code}")
                    read_mib_sec = 0
                    errors = 1
                else:
                    read_mib_sec = size_mib / read_time if read_time > 0 else 0
                    print(f"    Read: {read_time:.2f}s ({read_mib_sec:.1f} MiB/s), got {len(resp.content)} bytes")
                    errors = 0
            except Exception as exc:
                print(f"    Read EXCEPTION: {exc}")
                read_time = 0
                read_mib_sec = 0
                errors = 1

            results.append(
                LargeBlobResult(
                    size_mib=size_mib,
                    write_time_s=write_time,
                    read_time_s=read_time,
                    write_mib_sec=write_mib_sec,
                    read_mib_sec=read_mib_sec,
                    sign_time_s=sign_time,
                    build_time_s=build_time,
                    errors=errors,
                )
            )

    return results


# ---- PERF-04: Mixed workload benchmark --------------------------------------


async def _mixed_small_worker(
    client: httpx.AsyncClient,
    base_url: str,
    token: str,
    namespace_hex: str,
    blob_hash: str,
    stop_event: asyncio.Event,
    latencies: list[float],
) -> int:
    """Small-query worker for mixed workload: alternates stats and exists."""
    headers = {"Authorization": f"Bearer {token}"}
    errors = 0
    ops = 0
    while not stop_event.is_set():
        try:
            # Alternate between stats and exists
            if ops % 2 == 0:
                url = f"{base_url}/stats/{namespace_hex}"
            else:
                url = f"{base_url}/exists/{namespace_hex}/{blob_hash}"
            t0 = time.perf_counter_ns()
            resp = await client.get(url, headers=headers)
            t1 = time.perf_counter_ns()
            if resp.status_code == 200:
                latencies.append(ns_to_ms(t1 - t0))
            else:
                errors += 1
            ops += 1
        except Exception:
            errors += 1
            await asyncio.sleep(0.01)
    return errors


async def _mixed_large_worker(
    client: httpx.AsyncClient,
    base_url: str,
    token: str,
    identity: Identity,
    blob_size: int,
    stop_event: asyncio.Event,
    latencies: list[float],
) -> int:
    """Large-blob worker for mixed workload: writes large blobs."""
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/octet-stream",
    }
    errors = 0
    data = os.urandom(blob_size)
    while not stop_event.is_set():
        try:
            blob_bytes = build_blob(identity, data)
            t0 = time.perf_counter_ns()
            resp = await client.post(
                f"{base_url}/blob", content=blob_bytes, headers=headers
            )
            t1 = time.perf_counter_ns()
            if resp.status_code == 200:
                latencies.append(ns_to_ms(t1 - t0))
            else:
                errors += 1
        except Exception:
            errors += 1
            await asyncio.sleep(0.1)
    return errors


async def run_mixed_workload_benchmark(
    base_url: str,
    identity: Identity,
    duration_s: int = 30,
    small_clients: int = 10,
    large_clients: int = 2,
    large_blob_size: int = 10 * 1024 * 1024,
) -> tuple[MixedWorkloadResult, MixedWorkloadResult]:
    """PERF-04: Mixed workload -- small queries + large blob transfers.

    Returns (baseline_without_load, under_load).
    Baseline: small queries only. Under load: small queries + large blob writes.
    """
    # First, create a blob we can query against
    async with httpx.AsyncClient(http2=False, timeout=httpx.Timeout(120.0)) as setup_client:
        tok = await authenticate(setup_client, base_url, identity)
        blob_data = os.urandom(1024)
        blob_bytes = build_blob(identity, blob_data)
        resp = await setup_client.post(
            f"{base_url}/blob",
            content=blob_bytes,
            headers={
                "Authorization": f"Bearer {tok}",
                "Content-Type": "application/octet-stream",
            },
        )
        blob_hash = resp.json().get("blob_hash", "0" * 64)

    # -- Baseline: small queries only --
    print(f"  PERF-04: Baseline (small queries only, {small_clients} clients, {duration_s}s)...")
    small_lats_baseline: list[list[float]] = [[] for _ in range(small_clients)]
    baseline_clients: list[httpx.AsyncClient] = []
    baseline_tokens: list[str] = []

    for _ in range(small_clients):
        c = httpx.AsyncClient(http2=False, timeout=httpx.Timeout(30.0))
        baseline_clients.append(c)
        baseline_tokens.append(await authenticate(c, base_url, identity))

    stop = asyncio.Event()
    baseline_tasks = [
        _mixed_small_worker(
            baseline_clients[i], base_url, baseline_tokens[i],
            identity.namespace_hex, blob_hash, stop, small_lats_baseline[i]
        )
        for i in range(small_clients)
    ]

    gathered = asyncio.gather(*baseline_tasks)
    await asyncio.sleep(duration_s)
    stop.set()
    baseline_errors_list = await gathered

    flat_baseline = [lat for lats in small_lats_baseline for lat in lats]
    baseline_result = MixedWorkloadResult(
        small_query_count=len(flat_baseline),
        large_blob_count=0,
        duration_s=duration_s,
        small_latencies_ms=flat_baseline,
        small_percentiles=compute_percentiles(flat_baseline),
        errors=sum(baseline_errors_list),
    )
    print(
        f"    Baseline: {baseline_result.small_query_count} queries, "
        f"p50={baseline_result.small_percentiles['p50']:.2f}ms, "
        f"p99={baseline_result.small_percentiles['p99']:.2f}ms"
    )

    for c in baseline_clients:
        await c.aclose()

    # -- Under load: small queries + large blob writes --
    print(
        f"  PERF-04: Under load ({small_clients} small + {large_clients} large, "
        f"{duration_s}s)..."
    )
    small_lats_load: list[list[float]] = [[] for _ in range(small_clients)]
    large_lats_load: list[list[float]] = [[] for _ in range(large_clients)]
    all_clients: list[httpx.AsyncClient] = []
    all_tokens: list[str] = []

    for _ in range(small_clients + large_clients):
        c = httpx.AsyncClient(http2=False, timeout=httpx.Timeout(120.0))
        all_clients.append(c)
        all_tokens.append(await authenticate(c, base_url, identity))

    stop2 = asyncio.Event()
    load_tasks = []

    # Small query workers
    for i in range(small_clients):
        load_tasks.append(
            _mixed_small_worker(
                all_clients[i], base_url, all_tokens[i],
                identity.namespace_hex, blob_hash, stop2, small_lats_load[i]
            )
        )
    # Large blob workers
    for i in range(large_clients):
        idx = small_clients + i
        load_tasks.append(
            _mixed_large_worker(
                all_clients[idx], base_url, all_tokens[idx],
                identity, large_blob_size, stop2, large_lats_load[i]
            )
        )

    gathered2 = asyncio.gather(*load_tasks)
    await asyncio.sleep(duration_s)
    stop2.set()
    load_errors_list = await gathered2

    flat_small_load = [lat for lats in small_lats_load for lat in lats]
    flat_large_load = [lat for lats in large_lats_load for lat in lats]

    load_result = MixedWorkloadResult(
        small_query_count=len(flat_small_load),
        large_blob_count=len(flat_large_load),
        duration_s=duration_s,
        small_latencies_ms=flat_small_load,
        small_percentiles=compute_percentiles(flat_small_load),
        large_latencies_ms=flat_large_load,
        large_percentiles=compute_percentiles(flat_large_load),
        errors=sum(load_errors_list),
    )

    # Degradation
    if baseline_result.small_percentiles["p99"] > 0:
        degradation = (
            (load_result.small_percentiles["p99"] - baseline_result.small_percentiles["p99"])
            / baseline_result.small_percentiles["p99"]
            * 100
        )
    else:
        degradation = 0

    print(
        f"    Under load: {load_result.small_query_count} small queries, "
        f"{load_result.large_blob_count} large writes, "
        f"p50={load_result.small_percentiles['p50']:.2f}ms, "
        f"p99={load_result.small_percentiles['p99']:.2f}ms, "
        f"degradation={degradation:+.1f}%"
    )

    for c in all_clients:
        await c.aclose()

    return baseline_result, load_result


# ---- Report generation -------------------------------------------------------


def generate_report(report: BenchmarkReport, output_path: str) -> str:
    """Generate a markdown benchmark report."""
    lines: list[str] = []

    lines.append("# Relay Performance Benchmark Report")
    lines.append("")
    lines.append(f"**Date:** {report.date}")
    lines.append(f"**Hardware:** {report.hardware}")
    lines.append(f"**CPUs:** {os.cpu_count()}")
    lines.append(f"**Relay URL:** {report.relay_url}")
    lines.append("")
    lines.append("**Relay config notes:** Benchmarks should run with `rate_limit_messages_per_sec=0` "
                  "and `request_timeout_seconds=0` to avoid artificial limits.")
    lines.append("")

    # PERF-01: Throughput
    lines.append("## PERF-01: Throughput Benchmark")
    lines.append("")
    lines.append("1 KB blobs, POST /blob, per concurrency level.")
    lines.append("")
    lines.append("| Concurrency | Total Ops | Wall Time (s) | Blobs/sec | Per-Client ops/sec | p50 (ms) | p95 (ms) | p99 (ms) | Errors |")
    lines.append("|-------------|-----------|---------------|-----------|-------------------|----------|----------|----------|--------|")
    for r in report.throughput_results:
        lines.append(
            f"| {r.concurrency} | {r.total_ops} | {r.wall_time_s:.2f} | "
            f"{r.blobs_per_sec:.1f} | {r.per_client_ops_sec:.1f} | "
            f"{r.percentiles.get('p50', 0):.2f} | {r.percentiles.get('p95', 0):.2f} | "
            f"{r.percentiles.get('p99', 0):.2f} | {r.errors} |"
        )
    lines.append("")

    # PERF-02: Latency
    lines.append("## PERF-02: Per-Operation Latency")
    lines.append("")
    lines.append("Single client, HTTP keep-alive, relay-only (UDS baseline deferred).")
    lines.append("")
    lines.append("| Operation | Iterations | p50 (ms) | p95 (ms) | p99 (ms) | Errors |")
    lines.append("|-----------|-----------|----------|----------|----------|--------|")
    for r in report.latency_results:
        lines.append(
            f"| {r.operation} | {r.iterations} | "
            f"{r.percentiles.get('p50', 0):.2f} | {r.percentiles.get('p95', 0):.2f} | "
            f"{r.percentiles.get('p99', 0):.2f} | {r.errors} |"
        )
    lines.append("")

    # PERF-03: Large blobs
    lines.append("## PERF-03: Large Blob Transfer")
    lines.append("")
    lines.append("Raw binary POST /blob and GET /blob, single client, HTTP keep-alive.")
    lines.append("")
    lines.append("| Size (MiB) | Sign (s) | Build (s) | Write (s) | Write MiB/s | Read (s) | Read MiB/s | Errors |")
    lines.append("|-----------|----------|-----------|-----------|-------------|----------|------------|--------|")
    for r in report.large_blob_results:
        lines.append(
            f"| {r.size_mib:.0f} | {r.sign_time_s:.2f} | {r.build_time_s:.2f} | "
            f"{r.write_time_s:.2f} | {r.write_mib_sec:.1f} | "
            f"{r.read_time_s:.2f} | {r.read_mib_sec:.1f} | {r.errors} |"
        )
    lines.append("")

    # PERF-04: Mixed workload
    lines.append("## PERF-04: Mixed Workload")
    lines.append("")
    if report.mixed_baseline and report.mixed_under_load:
        bl = report.mixed_baseline
        ld = report.mixed_under_load

        lines.append(f"Baseline: {bl.small_query_count} small queries over {bl.duration_s}s (no large blobs).")
        lines.append(f"Under load: {ld.small_query_count} small queries + {ld.large_blob_count} large blob writes over {ld.duration_s}s.")
        lines.append("")
        lines.append("| Condition | Small Queries | Large Writes | p50 (ms) | p95 (ms) | p99 (ms) | Errors |")
        lines.append("|-----------|--------------|-------------|----------|----------|----------|--------|")
        lines.append(
            f"| Baseline | {bl.small_query_count} | 0 | "
            f"{bl.small_percentiles.get('p50', 0):.2f} | {bl.small_percentiles.get('p95', 0):.2f} | "
            f"{bl.small_percentiles.get('p99', 0):.2f} | {bl.errors} |"
        )
        lines.append(
            f"| Under load | {ld.small_query_count} | {ld.large_blob_count} | "
            f"{ld.small_percentiles.get('p50', 0):.2f} | {ld.small_percentiles.get('p95', 0):.2f} | "
            f"{ld.small_percentiles.get('p99', 0):.2f} | {ld.errors} |"
        )
        lines.append("")

        # Degradation analysis
        if bl.small_percentiles.get("p99", 0) > 0:
            deg_p50 = (
                (ld.small_percentiles["p50"] - bl.small_percentiles["p50"])
                / bl.small_percentiles["p50"]
                * 100
            ) if bl.small_percentiles["p50"] > 0 else 0
            deg_p99 = (
                (ld.small_percentiles["p99"] - bl.small_percentiles["p99"])
                / bl.small_percentiles["p99"]
                * 100
            )
            lines.append(f"**p50 degradation:** {deg_p50:+.1f}%")
            lines.append(f"**p99 degradation:** {deg_p99:+.1f}%")
        else:
            lines.append("*No baseline data to compute degradation.*")
    else:
        lines.append("*Mixed workload not executed.*")
    lines.append("")

    content = "\n".join(lines)

    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    Path(output_path).write_text(content, encoding="utf-8")

    return content


# ---- Main --------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Relay Performance Benchmark Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Run all benchmarks against local relay
    python3 tools/relay_benchmark.py --relay-url http://127.0.0.1:4201

    # Quick test: fewer iterations, skip large blobs
    python3 tools/relay_benchmark.py --iterations 10 --blob-sizes ""

    # Only measure throughput at high concurrency
    python3 tools/relay_benchmark.py --concurrency-levels "100" --iterations 500

Prerequisites:
    Relay config should have:
        rate_limit_messages_per_sec = 0
        request_timeout_seconds = 0
        """,
    )
    parser.add_argument(
        "--relay-url",
        default="http://localhost:4201",
        help="Relay HTTP base URL (default: http://localhost:4201)",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=5,
        help="Warmup iterations before measuring (default: 5)",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=100,
        help="Iterations per benchmark/client (default: 100)",
    )
    parser.add_argument(
        "--concurrency-levels",
        default="1,10,100",
        help="Comma-separated concurrency levels for PERF-01 (default: 1,10,100)",
    )
    parser.add_argument(
        "--blob-sizes",
        default="1048576,10485760,52428800,104857600",
        help=(
            "Comma-separated blob sizes in bytes for PERF-03 "
            "(default: 1048576,10485760,52428800,104857600 = 1,10,50,100 MiB)"
        ),
    )
    parser.add_argument(
        "--output",
        default="tools/benchmark_report.md",
        help="Output report path (default: tools/benchmark_report.md)",
    )
    parser.add_argument(
        "--skip-uds",
        action="store_true",
        default=True,
        help="Skip PERF-02 UDS baseline (default: true, relay-only latency measured)",
    )
    parser.add_argument(
        "--mixed-duration",
        type=int,
        default=30,
        help="Duration in seconds for PERF-04 mixed workload (default: 30)",
    )
    return parser.parse_args()


async def main() -> None:
    args = parse_args()

    concurrency_levels = [
        int(x.strip()) for x in args.concurrency_levels.split(",") if x.strip()
    ]
    blob_sizes = [
        int(x.strip()) for x in args.blob_sizes.split(",") if x.strip()
    ]

    print("=" * 60)
    print("  Relay Performance Benchmark")
    print("=" * 60)
    print(f"  Relay URL:    {args.relay_url}")
    print(f"  Warmup:       {args.warmup}")
    print(f"  Iterations:   {args.iterations}")
    print(f"  Concurrency:  {concurrency_levels}")
    print(f"  Blob sizes:   {[f'{s / 1024 / 1024:.0f} MiB' for s in blob_sizes]}")
    print(f"  Output:       {args.output}")
    print(f"  Skip UDS:     {args.skip_uds}")
    print("=" * 60)

    # Generate ephemeral identity
    print("\nGenerating ML-DSA-87 identity...")
    identity = Identity()
    print(f"  Namespace: {identity.namespace_hex}")

    report = BenchmarkReport(
        date=time.strftime("%Y-%m-%d %H:%M:%S %Z"),
        hardware=f"{platform.processor() or platform.machine()} / {platform.platform()}",
        relay_url=args.relay_url,
    )

    # PERF-01: Throughput
    print("\n--- PERF-01: Throughput Benchmark ---")
    report.throughput_results = await run_throughput_benchmark(
        args.relay_url, identity, concurrency_levels, args.iterations, args.warmup
    )

    # PERF-02: Latency
    print("\n--- PERF-02: Latency Benchmark ---")
    report.latency_results = await run_latency_benchmark(
        args.relay_url, identity, args.iterations, args.warmup, args.skip_uds
    )

    # PERF-03: Large blobs
    if blob_sizes:
        print("\n--- PERF-03: Large Blob Benchmark ---")
        report.large_blob_results = await run_large_blob_benchmark(
            args.relay_url, identity, blob_sizes
        )
    else:
        print("\n--- PERF-03: Skipped (no blob sizes) ---")

    # PERF-04: Mixed workload
    print("\n--- PERF-04: Mixed Workload Benchmark ---")
    baseline, under_load = await run_mixed_workload_benchmark(
        args.relay_url, identity, duration_s=args.mixed_duration
    )
    report.mixed_baseline = baseline
    report.mixed_under_load = under_load

    # Generate report
    print(f"\n--- Generating report: {args.output} ---")
    content = generate_report(report, args.output)
    print(content)
    print(f"\nReport saved to {args.output}")


if __name__ == "__main__":
    asyncio.run(main())
