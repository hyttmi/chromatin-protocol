"""PQ handshake initiator for chromatindb SDK.

Implements the client side of the ML-KEM-1024 key exchange and
ML-DSA-87 mutual authentication protocol. Matches the C++ Connection
class behavior in db/net/handshake.cpp.

Protocol (4 steps):
  1. Send KemPubkey([kem_pk:1568][sig_pk:2592]) as raw frame
  2. Recv KemCiphertext([ct:1568][sig_pk:2592]) as raw frame, decapsulate
  3. Derive session keys, send encrypted AuthSignature (counter=0)
  4. Recv encrypted AuthSignature (counter=0), verify peer

Post-handshake: send_counter=1, recv_counter=1.

Internal module -- not part of public API (per D-03).
"""

from __future__ import annotations

import asyncio
import struct

import oqs

from chromatindb._framing import recv_encrypted, recv_raw, send_encrypted, send_raw
from chromatindb._hkdf import hkdf_expand, hkdf_extract
from chromatindb.crypto import sha3_256
from chromatindb.exceptions import HandshakeError, ProtocolError
from chromatindb.identity import Identity
from chromatindb.wire import (
    TransportMsgType,
    decode_transport_message,
    encode_transport_message,
)

# ML-KEM-1024 sizes (match db/crypto/kem.h)
KEM_PK_SIZE: int = 1568
KEM_CT_SIZE: int = 1568
KEM_SS_SIZE: int = 32

# ML-DSA-87 public key size
SIG_PK_SIZE: int = 2592


def encode_auth_payload(signing_pubkey: bytes, signature: bytes) -> bytes:
    """Encode auth payload: [4-byte LE pubkey_size][pubkey][signature].

    Args:
        signing_pubkey: ML-DSA-87 public key bytes.
        signature: Signature bytes.

    Returns:
        Encoded auth payload.
    """
    return struct.pack("<I", len(signing_pubkey)) + signing_pubkey + signature


def decode_auth_payload(data: bytes) -> tuple[bytes, bytes]:
    """Decode auth payload. Returns (pubkey, signature).

    Args:
        data: Raw auth payload bytes.

    Returns:
        Tuple of (pubkey, signature).

    Raises:
        ProtocolError: On truncated or invalid data.
    """
    if len(data) < 4:
        raise ProtocolError("auth payload too short for pubkey_size header")
    pk_size = struct.unpack("<I", data[:4])[0]
    if len(data) < 4 + pk_size:
        raise ProtocolError(
            f"auth payload truncated: need {4 + pk_size}, got {len(data)}"
        )
    pubkey = data[4 : 4 + pk_size]
    signature = data[4 + pk_size :]
    return pubkey, signature


def derive_session_keys(
    shared_secret: bytes,
    initiator_sig_pk: bytes,
    responder_sig_pk: bytes,
) -> tuple[bytes, bytes, bytes]:
    """Derive (send_key, recv_key, fingerprint) for initiator.

    Uses empty HKDF salt per C++ implementation (NOT SHA3-256(pubkeys)
    as PROTOCOL.md incorrectly states -- fix deferred to Phase 74).

    Args:
        shared_secret: 32-byte KEM shared secret.
        initiator_sig_pk: Initiator's ML-DSA-87 public key.
        responder_sig_pk: Responder's ML-DSA-87 public key.

    Returns:
        (send_key, recv_key, session_fingerprint) -- all 32 bytes each.
    """
    prk = hkdf_extract(b"", shared_secret)
    send_key = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
    recv_key = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)
    fingerprint = sha3_256(shared_secret + initiator_sig_pk + responder_sig_pk)
    return send_key, recv_key, fingerprint


async def perform_handshake(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    identity: Identity,
) -> tuple[bytes, bytes, int, int, bytes]:
    """Execute PQ handshake as initiator.

    Args:
        reader: asyncio stream reader (TCP connection).
        writer: asyncio stream writer (TCP connection).
        identity: Client's ML-DSA-87 identity for signing.

    Returns:
        (send_key, recv_key, send_counter, recv_counter, responder_pubkey).
        Counters are 1 post-handshake (auth exchange consumed counter=0).

    Raises:
        HandshakeError: On protocol violation, auth failure, or
            unexpected message type.
    """
    # Step 1: Generate ephemeral KEM keypair, send KemPubkey
    kem = oqs.KeyEncapsulation("ML-KEM-1024")
    kem_pk = bytes(kem.generate_keypair())
    payload = kem_pk + identity.public_key  # [kem_pk:1568][sig_pk:2592]
    msg = encode_transport_message(TransportMsgType.KemPubkey, payload)
    await send_raw(writer, msg)

    # Step 2: Receive KemCiphertext, decapsulate
    raw = await recv_raw(reader)
    msg_type, resp_payload, _ = decode_transport_message(raw)
    if msg_type != TransportMsgType.KemCiphertext:
        raise HandshakeError(f"expected KemCiphertext (2), got type {msg_type}")
    expected_len = KEM_CT_SIZE + SIG_PK_SIZE
    if len(resp_payload) != expected_len:
        raise HandshakeError(
            f"KemCiphertext payload wrong size: expected {expected_len}, "
            f"got {len(resp_payload)}"
        )

    kem_ct = resp_payload[:KEM_CT_SIZE]
    responder_sig_pk = resp_payload[KEM_CT_SIZE:]
    shared_secret = bytes(kem.decap_secret(kem_ct))

    # Derive session keys
    send_key, recv_key, fingerprint = derive_session_keys(
        shared_secret,
        identity.public_key,
        responder_sig_pk,
    )

    # Step 3: Send encrypted auth (uses send_counter=0)
    signature = identity.sign(fingerprint)
    auth_payload = encode_auth_payload(identity.public_key, signature)
    auth_msg = encode_transport_message(TransportMsgType.AuthSignature, auth_payload)
    send_counter = await send_encrypted(writer, auth_msg, send_key, 0)
    # send_counter is now 1

    # Step 4: Receive and verify peer auth (uses recv_counter=0)
    resp_auth_plaintext, recv_counter = await recv_encrypted(reader, recv_key, 0)
    # recv_counter is now 1
    msg_type, resp_auth_payload, _ = decode_transport_message(resp_auth_plaintext)
    if msg_type != TransportMsgType.AuthSignature:
        raise HandshakeError(f"expected AuthSignature (3), got type {msg_type}")

    resp_pk, resp_sig = decode_auth_payload(resp_auth_payload)
    if resp_pk != responder_sig_pk:
        raise HandshakeError(
            "auth pubkey mismatch: KemCiphertext pubkey != AuthSignature pubkey"
        )
    if not Identity.verify(fingerprint, resp_sig, resp_pk):
        raise HandshakeError("peer auth signature invalid")

    return send_key, recv_key, send_counter, recv_counter, responder_sig_pk
