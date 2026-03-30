"""Binary payload encode/decode for chromatindb wire protocol.

All multi-byte integers in wire payloads are big-endian (``>`` in struct format).
FlatBuffer blob encoding uses ForceDefaults(True) to match C++ deterministic encoding.
"""

from __future__ import annotations

import struct

import flatbuffers

from chromatindb.exceptions import ProtocolError
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
