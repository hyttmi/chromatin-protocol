"""Binary payload encode/decode for chromatindb wire protocol.

All multi-byte integers in wire payloads are big-endian (``>`` in struct format).
FlatBuffer blob encoding uses ForceDefaults(True) to match C++ deterministic encoding.
"""

from __future__ import annotations

import struct

import flatbuffers

from chromatindb.exceptions import ProtocolError
from chromatindb.types import (
    BatchReadResult,
    DelegationEntry,
    DelegationList,
    MetadataResult,
    NamespaceEntry,
    NamespaceListResult,
    NamespaceStats,
    NodeInfo,
    Notification,
    PeerDetail,
    PeerInfo,
    ReadResult,
    StorageStatus,
    TimeRangeEntry,
    TimeRangeResult,
)
from chromatindb.generated.blob_generated import (
    Blob,
    BlobAddData,
    BlobAddNamespaceId,
    BlobAddPubkey,
    BlobAddSignature,
    BlobAddTimestamp,
    BlobAddTtl,
    BlobEnd,
    BlobStart,
)

# Tombstone magic bytes matching C++ TOMBSTONE_MAGIC in db/wire/codec.h
TOMBSTONE_MAGIC: bytes = b"\xDE\xAD\xBE\xEF"


def make_tombstone_data(target_hash: bytes) -> bytes:
    """Build tombstone data: 4-byte magic + 32-byte target blob hash.

    Args:
        target_hash: 32-byte SHA3-256 hash of the blob to delete.

    Returns:
        36-byte tombstone data.

    Raises:
        ValueError: If target_hash is not 32 bytes.
    """
    if len(target_hash) != 32:
        raise ValueError(
            f"target_hash must be 32 bytes, got {len(target_hash)}"
        )
    return TOMBSTONE_MAGIC + target_hash


def encode_blob_payload(
    namespace_id: bytes,
    pubkey: bytes,
    data: bytes,
    ttl: int,
    timestamp: int,
    signature: bytes,
) -> bytes:
    """Encode a Blob FlatBuffer matching C++ encode_blob() with ForceDefaults.

    Args:
        namespace_id: 32-byte namespace identifier.
        pubkey: Public key bytes (ML-DSA-87: 2592 bytes).
        data: Blob payload data.
        ttl: Time-to-live in seconds (uint32). 0 for permanent/tombstone.
        timestamp: Unix timestamp in seconds (uint64).
        signature: ML-DSA-87 signature bytes.

    Returns:
        FlatBuffer-encoded blob bytes.
    """
    builder = flatbuffers.Builder(len(data) + 8192)
    builder.ForceDefaults(True)

    # Create byte vectors BEFORE BlobStart (FlatBuffers requirement)
    ns_vec = builder.CreateByteVector(namespace_id)
    pk_vec = builder.CreateByteVector(pubkey)
    dt_vec = builder.CreateByteVector(data)
    sg_vec = builder.CreateByteVector(signature)

    BlobStart(builder)
    BlobAddNamespaceId(builder, ns_vec)
    BlobAddPubkey(builder, pk_vec)
    BlobAddData(builder, dt_vec)
    BlobAddTtl(builder, ttl)
    BlobAddTimestamp(builder, timestamp)
    BlobAddSignature(builder, sg_vec)
    blob = BlobEnd(builder)
    builder.Finish(blob)
    return bytes(builder.Output())


def decode_write_ack(payload: bytes) -> tuple[bytes, int, bool]:
    """Decode WriteAck payload: [blob_hash:32][seq_num_be:8][status:1].

    Args:
        payload: 41-byte WriteAck payload.

    Returns:
        Tuple of (blob_hash, seq_num, duplicate).

    Raises:
        ProtocolError: If payload is not exactly 41 bytes.
    """
    if len(payload) != 41:
        raise ProtocolError(
            f"WriteAck must be 41 bytes, got {len(payload)}"
        )
    blob_hash = payload[:32]
    seq_num = struct.unpack(">Q", payload[32:40])[0]
    duplicate = payload[40] == 1
    return blob_hash, seq_num, duplicate


def decode_delete_ack(payload: bytes) -> tuple[bytes, int, bool]:
    """Decode DeleteAck payload: [blob_hash:32][seq_num_be:8][status:1].

    Identical binary format to WriteAck.

    Args:
        payload: 41-byte DeleteAck payload.

    Returns:
        Tuple of (blob_hash, seq_num, duplicate).

    Raises:
        ProtocolError: If payload is not exactly 41 bytes.
    """
    if len(payload) != 41:
        raise ProtocolError(
            f"DeleteAck must be 41 bytes, got {len(payload)}"
        )
    blob_hash = payload[:32]
    seq_num = struct.unpack(">Q", payload[32:40])[0]
    duplicate = payload[40] == 1
    return blob_hash, seq_num, duplicate


def encode_read_request(namespace: bytes, blob_hash: bytes) -> bytes:
    """Encode ReadRequest payload: [namespace:32][blob_hash:32].

    Args:
        namespace: 32-byte namespace identifier.
        blob_hash: 32-byte blob hash.

    Returns:
        64-byte ReadRequest payload.

    Raises:
        ValueError: If namespace or blob_hash is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    if len(blob_hash) != 32:
        raise ValueError(
            f"blob_hash must be 32 bytes, got {len(blob_hash)}"
        )
    return namespace + blob_hash


def decode_read_response(
    payload: bytes,
) -> tuple[bytes, int, int, bytes] | None:
    """Decode ReadResponse payload.

    Format: [found:1][FlatBuffer Blob...] or [not_found:0x00] (1 byte).

    Args:
        payload: ReadResponse payload bytes.

    Returns:
        Tuple of (data, ttl, timestamp, signature) if found, None if not found.

    Raises:
        ProtocolError: If payload is empty or has unexpected found flag.
    """
    if len(payload) < 1:
        raise ProtocolError("empty ReadResponse")
    if payload[0] == 0x00:
        return None
    if payload[0] != 0x01:
        raise ProtocolError(
            f"unexpected ReadResponse flag: {payload[0]:#x}"
        )

    fb_bytes = payload[1:]
    blob = Blob.GetRootAs(fb_bytes, 0)

    data = bytes(blob.Data(j) for j in range(blob.DataLength()))
    ttl = blob.Ttl()
    timestamp = blob.Timestamp()
    signature = bytes(blob.Signature(j) for j in range(blob.SignatureLength()))

    return data, ttl, timestamp, signature


def encode_list_request(
    namespace: bytes, since_seq: int, limit: int
) -> bytes:
    """Encode ListRequest payload: [namespace:32][since_seq_be:8][limit_be:4].

    Args:
        namespace: 32-byte namespace identifier.
        since_seq: Sequence number cursor (exclusive). 0 for first page.
        limit: Maximum entries to return.

    Returns:
        44-byte ListRequest payload.

    Raises:
        ValueError: If namespace is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    return namespace + struct.pack(">Q", since_seq) + struct.pack(">I", limit)


def decode_list_response(
    payload: bytes,
) -> tuple[list[tuple[bytes, int]], bool]:
    """Decode ListResponse payload.

    Format: [count_be:4][ [hash:32][seq_be:8] * count ][has_more:1]

    Args:
        payload: ListResponse payload bytes.

    Returns:
        Tuple of (entries, has_more) where entries is list of (blob_hash, seq_num).

    Raises:
        ProtocolError: If payload is too short or size mismatches.
    """
    if len(payload) < 5:
        raise ProtocolError(
            f"ListResponse too short: {len(payload)} bytes"
        )
    count = struct.unpack(">I", payload[:4])[0]
    expected_len = 4 + count * 40 + 1
    if len(payload) != expected_len:
        raise ProtocolError(
            f"ListResponse size mismatch: expected {expected_len}, got {len(payload)}"
        )
    entries: list[tuple[bytes, int]] = []
    for i in range(count):
        off = 4 + i * 40
        blob_hash = payload[off : off + 32]
        seq_num = struct.unpack(">Q", payload[off + 32 : off + 40])[0]
        entries.append((blob_hash, seq_num))
    has_more = payload[-1] == 1
    return entries, has_more


def encode_exists_request(namespace: bytes, blob_hash: bytes) -> bytes:
    """Encode ExistsRequest payload: [namespace:32][blob_hash:32].

    Args:
        namespace: 32-byte namespace identifier.
        blob_hash: 32-byte blob hash.

    Returns:
        64-byte ExistsRequest payload.

    Raises:
        ValueError: If namespace or blob_hash is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    if len(blob_hash) != 32:
        raise ValueError(
            f"blob_hash must be 32 bytes, got {len(blob_hash)}"
        )
    return namespace + blob_hash


def decode_exists_response(payload: bytes) -> tuple[bool, bytes]:
    """Decode ExistsResponse payload: [exists:1][blob_hash:32].

    Args:
        payload: 33-byte ExistsResponse payload.

    Returns:
        Tuple of (exists, blob_hash).

    Raises:
        ProtocolError: If payload is not exactly 33 bytes.
    """
    if len(payload) != 33:
        raise ProtocolError(
            f"ExistsResponse must be 33 bytes, got {len(payload)}"
        )
    exists = payload[0] == 1
    blob_hash = payload[1:33]
    return exists, blob_hash


# --- Phase 73: Extended query & pub/sub codecs ---


def encode_metadata_request(namespace: bytes, blob_hash: bytes) -> bytes:
    """Encode MetadataRequest payload: [namespace:32][blob_hash:32].

    Args:
        namespace: 32-byte namespace identifier.
        blob_hash: 32-byte blob hash.

    Returns:
        64-byte MetadataRequest payload.

    Raises:
        ValueError: If namespace or blob_hash is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    if len(blob_hash) != 32:
        raise ValueError(
            f"blob_hash must be 32 bytes, got {len(blob_hash)}"
        )
    return namespace + blob_hash


def decode_metadata_response(payload: bytes) -> MetadataResult | None:
    """Decode MetadataResponse payload.

    Format: [not_found:0x00] or [found:0x01][hash:32][ts:8BE][ttl:4BE]
            [size:8BE][seq:8BE][pk_len:2BE][pk:N].

    Args:
        payload: MetadataResponse payload bytes.

    Returns:
        MetadataResult if found, None if not found.

    Raises:
        ProtocolError: If payload is invalid.
    """
    if len(payload) < 1:
        raise ProtocolError("empty MetadataResponse")
    if payload[0] == 0x00:
        return None
    if payload[0] != 0x01:
        raise ProtocolError(
            f"unexpected MetadataResponse flag: {payload[0]:#x}"
        )
    if len(payload) < 63:
        raise ProtocolError(
            f"MetadataResponse too short: {len(payload)} bytes"
        )
    blob_hash = payload[1:33]
    timestamp = struct.unpack(">Q", payload[33:41])[0]
    ttl = struct.unpack(">I", payload[41:45])[0]
    data_size = struct.unpack(">Q", payload[45:53])[0]
    seq_num = struct.unpack(">Q", payload[53:61])[0]
    pubkey_len = struct.unpack(">H", payload[61:63])[0]
    if len(payload) != 63 + pubkey_len:
        raise ProtocolError(
            f"MetadataResponse size mismatch: expected {63 + pubkey_len}, "
            f"got {len(payload)}"
        )
    pubkey = payload[63 : 63 + pubkey_len]
    return MetadataResult(
        blob_hash=blob_hash,
        timestamp=timestamp,
        ttl=ttl,
        data_size=data_size,
        seq_num=seq_num,
        pubkey=pubkey,
    )


def encode_batch_exists_request(
    namespace: bytes, hashes: list[bytes]
) -> bytes:
    """Encode BatchExistsRequest: [ns:32][count:4BE][hash:32*N].

    Args:
        namespace: 32-byte namespace identifier.
        hashes: List of 32-byte blob hashes.

    Returns:
        Encoded request payload.

    Raises:
        ValueError: If namespace or any hash is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    for h in hashes:
        if len(h) != 32:
            raise ValueError(
                f"each hash must be 32 bytes, got {len(h)}"
            )
    return namespace + struct.pack(">I", len(hashes)) + b"".join(hashes)


def decode_batch_exists_response(
    payload: bytes, hashes: list[bytes]
) -> dict[bytes, bool]:
    """Decode BatchExistsResponse: [exists:1*count].

    Args:
        payload: Response payload (one byte per hash).
        hashes: Original hashes list for key mapping.

    Returns:
        Dict mapping each hash to existence boolean.

    Raises:
        ProtocolError: If payload length doesn't match hash count.
    """
    if len(payload) != len(hashes):
        raise ProtocolError(
            f"BatchExistsResponse length mismatch: "
            f"expected {len(hashes)} bytes, got {len(payload)}"
        )
    return {hashes[i]: payload[i] == 1 for i in range(len(hashes))}


def encode_batch_read_request(
    namespace: bytes, hashes: list[bytes], cap_bytes: int = 0
) -> bytes:
    """Encode BatchReadRequest: [ns:32][cap:4BE][count:4BE][hash:32*N].

    Args:
        namespace: 32-byte namespace identifier.
        hashes: List of 32-byte blob hashes.
        cap_bytes: Max response size in bytes. 0 = server default (4 MiB).

    Returns:
        Encoded request payload.

    Raises:
        ValueError: If namespace or any hash is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    for h in hashes:
        if len(h) != 32:
            raise ValueError(
                f"each hash must be 32 bytes, got {len(h)}"
            )
    return (
        namespace
        + struct.pack(">I", cap_bytes)
        + struct.pack(">I", len(hashes))
        + b"".join(hashes)
    )


def decode_batch_read_response(payload: bytes) -> BatchReadResult:
    """Decode BatchReadResponse.

    Format: [trunc:1][count:4BE][entries...].
    Found entry: [0x01][hash:32][size:8BE][FlatBuffer_blob:size].
    Not-found entry: [0x00][hash:32].

    Args:
        payload: Response payload bytes.

    Returns:
        BatchReadResult with blobs dict and truncation flag.

    Raises:
        ProtocolError: If payload is too short or malformed.
    """
    if len(payload) < 5:
        raise ProtocolError(
            f"BatchReadResponse too short: {len(payload)} bytes"
        )
    truncated = payload[0] == 1
    count = struct.unpack(">I", payload[1:5])[0]
    blobs: dict[bytes, ReadResult | None] = {}
    off = 5
    for _ in range(count):
        if off >= len(payload):
            raise ProtocolError("BatchReadResponse truncated mid-entry")
        status = payload[off]
        if status == 0x01:
            # Found: [0x01][hash:32][size:8BE][fb_data:size]
            blob_hash = payload[off + 1 : off + 33]
            size = struct.unpack(">Q", payload[off + 33 : off + 41])[0]
            fb_data = payload[off + 41 : off + 41 + size]
            # Decode FlatBuffer blob reusing existing decode logic
            result = decode_read_response(bytes([0x01]) + fb_data)
            if result is not None:
                data, ttl, timestamp, signature = result
                blobs[blob_hash] = ReadResult(
                    data=data, ttl=ttl, timestamp=timestamp,
                    signature=signature,
                )
            else:
                blobs[blob_hash] = None
            off += 41 + size
        else:
            # Not found: [0x00][hash:32]
            blob_hash = payload[off + 1 : off + 33]
            blobs[blob_hash] = None
            off += 33
    return BatchReadResult(blobs=blobs, truncated=truncated)


def encode_time_range_request(
    namespace: bytes,
    start_ts: int,
    end_ts: int,
    limit: int = 100,
) -> bytes:
    """Encode TimeRangeRequest: [ns:32][start:8BE][end:8BE][limit:4BE].

    Args:
        namespace: 32-byte namespace identifier.
        start_ts: Start timestamp (seconds, inclusive).
        end_ts: End timestamp (seconds, inclusive).
        limit: Max results (default 100, server clamps to [1, 100]).

    Returns:
        52-byte TimeRangeRequest payload.

    Raises:
        ValueError: If namespace is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    return (
        namespace
        + struct.pack(">Q", start_ts)
        + struct.pack(">Q", end_ts)
        + struct.pack(">I", limit)
    )


def decode_time_range_response(payload: bytes) -> TimeRangeResult:
    """Decode TimeRangeResponse.

    Format: [trunc:1][count:4BE][entries...48*N].
    Per entry: [hash:32][seq:8BE][ts:8BE] = 48 bytes.

    Args:
        payload: Response payload bytes.

    Returns:
        TimeRangeResult with entries and truncation flag.

    Raises:
        ProtocolError: If payload is too short or malformed.
    """
    if len(payload) < 5:
        raise ProtocolError(
            f"TimeRangeResponse too short: {len(payload)} bytes"
        )
    truncated = payload[0] == 1
    count = struct.unpack(">I", payload[1:5])[0]
    expected = 5 + count * 48
    if len(payload) < expected:
        raise ProtocolError(
            f"TimeRangeResponse size mismatch: "
            f"expected {expected}, got {len(payload)}"
        )
    entries: list[TimeRangeEntry] = []
    for i in range(count):
        off = 5 + i * 48
        blob_hash = payload[off : off + 32]
        seq_num = struct.unpack(">Q", payload[off + 32 : off + 40])[0]
        timestamp = struct.unpack(">Q", payload[off + 40 : off + 48])[0]
        entries.append(
            TimeRangeEntry(
                blob_hash=blob_hash, seq_num=seq_num, timestamp=timestamp
            )
        )
    return TimeRangeResult(entries=entries, truncated=truncated)


def encode_namespace_list_request(
    after_ns: bytes, limit: int = 100
) -> bytes:
    """Encode NamespaceListRequest: [after_ns:32][limit:4BE].

    Args:
        after_ns: 32-byte namespace cursor. All zeros for first page.
        limit: Max namespaces to return (default 100).

    Returns:
        36-byte NamespaceListRequest payload.

    Raises:
        ValueError: If after_ns is not 32 bytes.
    """
    if len(after_ns) != 32:
        raise ValueError(
            f"after_ns must be 32 bytes, got {len(after_ns)}"
        )
    return after_ns + struct.pack(">I", limit)


def decode_namespace_list_response(payload: bytes) -> NamespaceListResult:
    """Decode NamespaceListResponse.

    Format: [count:4BE][has_more:1][entries...40*N].
    Per entry: [ns_id:32][blob_count:8BE] = 40 bytes.

    Args:
        payload: Response payload bytes.

    Returns:
        NamespaceListResult with namespaces and cursor.

    Raises:
        ProtocolError: If payload is too short or malformed.
    """
    if len(payload) < 5:
        raise ProtocolError(
            f"NamespaceListResponse too short: {len(payload)} bytes"
        )
    count = struct.unpack(">I", payload[0:4])[0]
    has_more = payload[4] == 1
    expected = 5 + count * 40
    if len(payload) < expected:
        raise ProtocolError(
            f"NamespaceListResponse size mismatch: "
            f"expected {expected}, got {len(payload)}"
        )
    namespaces: list[NamespaceEntry] = []
    for i in range(count):
        off = 5 + i * 40
        namespace_id = payload[off : off + 32]
        blob_count = struct.unpack(">Q", payload[off + 32 : off + 40])[0]
        namespaces.append(
            NamespaceEntry(namespace_id=namespace_id, blob_count=blob_count)
        )
    cursor = namespaces[-1].namespace_id if has_more and namespaces else None
    return NamespaceListResult(namespaces=namespaces, cursor=cursor)


def encode_namespace_stats_request(namespace: bytes) -> bytes:
    """Encode NamespaceStatsRequest: [ns:32].

    Args:
        namespace: 32-byte namespace identifier.

    Returns:
        32-byte NamespaceStatsRequest payload.

    Raises:
        ValueError: If namespace is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    return namespace


def decode_namespace_stats_response(payload: bytes) -> NamespaceStats:
    """Decode NamespaceStatsResponse.

    Format: [found:1][blob_count:8BE][total_bytes:8BE][delegation_count:8BE]
            [quota_bytes:8BE][quota_count:8BE] = 41 bytes.

    Args:
        payload: 41-byte response payload.

    Returns:
        NamespaceStats result.

    Raises:
        ProtocolError: If payload is not exactly 41 bytes.
    """
    if len(payload) != 41:
        raise ProtocolError(
            f"NamespaceStatsResponse must be 41 bytes, got {len(payload)}"
        )
    found = payload[0] == 1
    blob_count = struct.unpack(">Q", payload[1:9])[0]
    total_bytes = struct.unpack(">Q", payload[9:17])[0]
    delegation_count = struct.unpack(">Q", payload[17:25])[0]
    quota_bytes_limit = struct.unpack(">Q", payload[25:33])[0]
    quota_count_limit = struct.unpack(">Q", payload[33:41])[0]
    return NamespaceStats(
        found=found,
        blob_count=blob_count,
        total_bytes=total_bytes,
        delegation_count=delegation_count,
        quota_bytes_limit=quota_bytes_limit,
        quota_count_limit=quota_count_limit,
    )


def decode_storage_status_response(payload: bytes) -> StorageStatus:
    """Decode StorageStatusResponse.

    Format: [used_data:8BE][max_storage:8BE][tombstone_count:8BE]
            [namespace_count:4BE][total_blobs:8BE][mmap_bytes:8BE] = 44 bytes.

    Args:
        payload: 44-byte response payload.

    Returns:
        StorageStatus result.

    Raises:
        ProtocolError: If payload is not exactly 44 bytes.
    """
    if len(payload) != 44:
        raise ProtocolError(
            f"StorageStatusResponse must be 44 bytes, got {len(payload)}"
        )
    used_data_bytes = struct.unpack(">Q", payload[0:8])[0]
    max_storage_bytes = struct.unpack(">Q", payload[8:16])[0]
    tombstone_count = struct.unpack(">Q", payload[16:24])[0]
    namespace_count = struct.unpack(">I", payload[24:28])[0]
    total_blobs = struct.unpack(">Q", payload[28:36])[0]
    mmap_bytes = struct.unpack(">Q", payload[36:44])[0]
    return StorageStatus(
        used_data_bytes=used_data_bytes,
        max_storage_bytes=max_storage_bytes,
        tombstone_count=tombstone_count,
        namespace_count=namespace_count,
        total_blobs=total_blobs,
        mmap_bytes=mmap_bytes,
    )


def decode_node_info_response(payload: bytes) -> NodeInfo:
    """Decode NodeInfoResponse (variable length).

    Format: [ver_len:1][ver:N][git_len:1][git:N][uptime:8BE][peers:4BE]
            [ns_count:4BE][blobs:8BE][used:8BE][max:8BE]
            [types_count:1][types:N].

    Args:
        payload: Response payload bytes.

    Returns:
        NodeInfo result.

    Raises:
        ProtocolError: If payload is truncated or malformed.
    """
    try:
        off = 0
        version_len = payload[off]
        off += 1
        version = payload[off : off + version_len].decode("utf-8")
        off += version_len

        git_hash_len = payload[off]
        off += 1
        git_hash = payload[off : off + git_hash_len].decode("utf-8")
        off += git_hash_len

        uptime_seconds = struct.unpack(">Q", payload[off : off + 8])[0]
        off += 8
        peer_count = struct.unpack(">I", payload[off : off + 4])[0]
        off += 4
        namespace_count = struct.unpack(">I", payload[off : off + 4])[0]
        off += 4
        total_blobs = struct.unpack(">Q", payload[off : off + 8])[0]
        off += 8
        storage_used_bytes = struct.unpack(">Q", payload[off : off + 8])[0]
        off += 8
        storage_max_bytes = struct.unpack(">Q", payload[off : off + 8])[0]
        off += 8

        types_count = payload[off]
        off += 1
        supported_types = list(payload[off : off + types_count])
    except (IndexError, struct.error) as e:
        raise ProtocolError(
            f"NodeInfoResponse truncated or malformed: {e}"
        ) from e

    return NodeInfo(
        version=version,
        git_hash=git_hash,
        uptime_seconds=uptime_seconds,
        peer_count=peer_count,
        namespace_count=namespace_count,
        total_blobs=total_blobs,
        storage_used_bytes=storage_used_bytes,
        storage_max_bytes=storage_max_bytes,
        supported_types=supported_types,
    )


def decode_peer_info_response(payload: bytes) -> PeerInfo:
    """Decode PeerInfoResponse (trust-gated).

    Untrusted (8 bytes): [peer_count:4BE][bootstrap_count:4BE].
    Trusted (>8 bytes): same header + per-peer entries:
        [addr_len:2BE][addr:N][is_bootstrap:1][syncing:1]
        [peer_is_full:1][duration_ms:8BE].

    Args:
        payload: Response payload bytes.

    Returns:
        PeerInfo with empty peers list if untrusted.

    Raises:
        ProtocolError: If payload is too short or malformed.
    """
    if len(payload) < 8:
        raise ProtocolError(
            f"PeerInfoResponse too short: {len(payload)} bytes"
        )
    peer_count = struct.unpack(">I", payload[0:4])[0]
    bootstrap_count = struct.unpack(">I", payload[4:8])[0]

    if len(payload) == 8:
        return PeerInfo(
            peer_count=peer_count,
            bootstrap_count=bootstrap_count,
            peers=[],
        )

    # Trusted: parse per-peer entries
    peers: list[PeerDetail] = []
    off = 8
    try:
        while off < len(payload):
            addr_len = struct.unpack(">H", payload[off : off + 2])[0]
            off += 2
            address = payload[off : off + addr_len].decode("utf-8")
            off += addr_len
            is_bootstrap = payload[off] == 1
            off += 1
            syncing = payload[off] == 1
            off += 1
            peer_is_full = payload[off] == 1
            off += 1
            connected_duration_ms = struct.unpack(
                ">Q", payload[off : off + 8]
            )[0]
            off += 8
            peers.append(
                PeerDetail(
                    address=address,
                    is_bootstrap=is_bootstrap,
                    syncing=syncing,
                    peer_is_full=peer_is_full,
                    connected_duration_ms=connected_duration_ms,
                )
            )
    except (IndexError, struct.error) as e:
        raise ProtocolError(
            f"PeerInfoResponse truncated or malformed: {e}"
        ) from e

    return PeerInfo(
        peer_count=peer_count,
        bootstrap_count=bootstrap_count,
        peers=peers,
    )


def encode_delegation_list_request(namespace: bytes) -> bytes:
    """Encode DelegationListRequest: [ns:32].

    Args:
        namespace: 32-byte namespace identifier.

    Returns:
        32-byte DelegationListRequest payload.

    Raises:
        ValueError: If namespace is not 32 bytes.
    """
    if len(namespace) != 32:
        raise ValueError(
            f"namespace must be 32 bytes, got {len(namespace)}"
        )
    return namespace


def decode_delegation_list_response(payload: bytes) -> DelegationList:
    """Decode DelegationListResponse.

    Format: [count:4BE][entries...64*N].
    Per entry: [delegate_pk_hash:32][delegation_blob_hash:32] = 64 bytes.

    Args:
        payload: Response payload bytes.

    Returns:
        DelegationList result.

    Raises:
        ProtocolError: If payload is too short or size mismatches.
    """
    if len(payload) < 4:
        raise ProtocolError(
            f"DelegationListResponse too short: {len(payload)} bytes"
        )
    count = struct.unpack(">I", payload[0:4])[0]
    expected = 4 + count * 64
    if len(payload) != expected:
        raise ProtocolError(
            f"DelegationListResponse size mismatch: "
            f"expected {expected}, got {len(payload)}"
        )
    entries: list[DelegationEntry] = []
    for i in range(count):
        off = 4 + i * 64
        delegate_pk_hash = payload[off : off + 32]
        delegation_blob_hash = payload[off + 32 : off + 64]
        entries.append(
            DelegationEntry(
                delegate_pk_hash=delegate_pk_hash,
                delegation_blob_hash=delegation_blob_hash,
            )
        )
    return DelegationList(entries=entries)


def encode_subscribe(namespaces: list[bytes]) -> bytes:
    """Encode Subscribe payload: [count:2BE][ns:32*N].

    Args:
        namespaces: List of 32-byte namespace identifiers.

    Returns:
        Encoded Subscribe payload.

    Raises:
        ValueError: If any namespace is not 32 bytes.
    """
    for ns in namespaces:
        if len(ns) != 32:
            raise ValueError(
                f"each namespace must be 32 bytes, got {len(ns)}"
            )
    return struct.pack(">H", len(namespaces)) + b"".join(namespaces)


def encode_unsubscribe(namespaces: list[bytes]) -> bytes:
    """Encode Unsubscribe payload: [count:2BE][ns:32*N].

    Args:
        namespaces: List of 32-byte namespace identifiers.

    Returns:
        Encoded Unsubscribe payload.

    Raises:
        ValueError: If any namespace is not 32 bytes.
    """
    for ns in namespaces:
        if len(ns) != 32:
            raise ValueError(
                f"each namespace must be 32 bytes, got {len(ns)}"
            )
    return struct.pack(">H", len(namespaces)) + b"".join(namespaces)


def decode_notification(payload: bytes) -> Notification:
    """Decode Notification payload (77 bytes fixed).

    Format: [ns:32][hash:32][seq:8BE][size:4BE][tombstone:1].

    Args:
        payload: 77-byte Notification payload.

    Returns:
        Notification result.

    Raises:
        ProtocolError: If payload is not exactly 77 bytes.
    """
    if len(payload) != 77:
        raise ProtocolError(
            f"Notification must be 77 bytes, got {len(payload)}"
        )
    namespace = payload[0:32]
    blob_hash = payload[32:64]
    seq_num = struct.unpack(">Q", payload[64:72])[0]
    blob_size = struct.unpack(">I", payload[72:76])[0]
    is_tombstone = payload[76] == 1
    return Notification(
        namespace=namespace,
        blob_hash=blob_hash,
        seq_num=seq_num,
        blob_size=blob_size,
        is_tombstone=is_tombstone,
    )
