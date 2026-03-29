"""Unit tests for PQ handshake initiator module.

Tests cover:
- Auth payload encode/decode with LE endianness
- Session key derivation with empty HKDF salt
- Full 4-step handshake protocol with mock relay
"""

from __future__ import annotations

import asyncio
import os
import struct

import oqs
import pytest

from chromatindb._hkdf import hkdf_expand, hkdf_extract
from chromatindb.crypto import sha3_256
from chromatindb.exceptions import HandshakeError, ProtocolError
from chromatindb.identity import Identity
from chromatindb.wire import (
    TransportMsgType,
    decode_transport_message,
    encode_transport_message,
)

from chromatindb._handshake import (
    KEM_CT_SIZE,
    KEM_PK_SIZE,
    KEM_SS_SIZE,
    SIG_PK_SIZE,
    decode_auth_payload,
    derive_session_keys,
    encode_auth_payload,
    perform_handshake,
)


# ---------------------------------------------------------------------------
# encode_auth_payload / decode_auth_payload
# ---------------------------------------------------------------------------


class TestAuthPayload:
    def test_encode_produces_le_pubkey_size_header(self) -> None:
        """Auth payload starts with 4-byte LE pubkey_size."""
        pubkey = os.urandom(2592)
        sig = os.urandom(100)
        encoded = encode_auth_payload(pubkey, sig)
        assert encoded[:4] == struct.pack("<I", 2592)
        assert encoded[4 : 4 + 2592] == pubkey
        assert encoded[4 + 2592 :] == sig

    def test_roundtrip(self) -> None:
        """decode_auth_payload reverses encode_auth_payload."""
        pubkey = os.urandom(2592)
        sig = os.urandom(4627)
        encoded = encode_auth_payload(pubkey, sig)
        dec_pk, dec_sig = decode_auth_payload(encoded)
        assert dec_pk == pubkey
        assert dec_sig == sig

    def test_decode_truncated_header_raises(self) -> None:
        """decode_auth_payload raises ProtocolError on < 4 bytes."""
        with pytest.raises(ProtocolError):
            decode_auth_payload(b"\x00\x01\x02")

    def test_decode_truncated_pubkey_raises(self) -> None:
        """decode_auth_payload raises ProtocolError when pubkey_size exceeds remaining data."""
        # Header says 100 bytes, but only 10 follow
        data = struct.pack("<I", 100) + os.urandom(10)
        with pytest.raises(ProtocolError):
            decode_auth_payload(data)


# ---------------------------------------------------------------------------
# derive_session_keys
# ---------------------------------------------------------------------------


class TestDeriveSessionKeys:
    def test_produces_three_32byte_values(self) -> None:
        """derive_session_keys returns (send_key, recv_key, fingerprint), all 32 bytes."""
        shared_secret = os.urandom(32)
        init_pk = os.urandom(2592)
        resp_pk = os.urandom(2592)
        send_key, recv_key, fingerprint = derive_session_keys(
            shared_secret, init_pk, resp_pk
        )
        assert len(send_key) == 32
        assert len(recv_key) == 32
        assert len(fingerprint) == 32

    def test_uses_empty_hkdf_salt(self) -> None:
        """derive_session_keys uses empty HKDF salt (not SHA3-256(pubkeys))."""
        shared_secret = os.urandom(32)
        init_pk = os.urandom(2592)
        resp_pk = os.urandom(2592)
        send_key, recv_key, fingerprint = derive_session_keys(
            shared_secret, init_pk, resp_pk
        )
        prk = hkdf_extract(b"", shared_secret)
        assert send_key == hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
        assert recv_key == hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)

    def test_correct_directional_contexts(self) -> None:
        """send_key uses init-to-resp, recv_key uses resp-to-init."""
        shared_secret = os.urandom(32)
        init_pk = os.urandom(2592)
        resp_pk = os.urandom(2592)
        send_key, recv_key, _ = derive_session_keys(shared_secret, init_pk, resp_pk)
        prk = hkdf_extract(b"", shared_secret)
        expected_send = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
        expected_recv = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)
        assert send_key == expected_send
        assert recv_key == expected_recv
        assert send_key != recv_key  # directional keys differ

    def test_fingerprint_computation(self) -> None:
        """fingerprint = SHA3-256(shared_secret || initiator_sig_pk || responder_sig_pk)."""
        shared_secret = os.urandom(32)
        init_pk = os.urandom(2592)
        resp_pk = os.urandom(2592)
        _, _, fingerprint = derive_session_keys(shared_secret, init_pk, resp_pk)
        expected = sha3_256(shared_secret + init_pk + resp_pk)
        assert fingerprint == expected


# ---------------------------------------------------------------------------
# perform_handshake (full 4-step protocol with mock relay)
# ---------------------------------------------------------------------------


class MockStreamWriter:
    """Captures written data for assertion."""

    def __init__(self) -> None:
        self.data = bytearray()
        self._closed = False

    def write(self, data: bytes) -> None:
        self.data.extend(data)

    async def drain(self) -> None:
        pass

    def close(self) -> None:
        self._closed = True

    async def wait_closed(self) -> None:
        pass


class MockStreamReader:
    """Feeds pre-built frames sequentially."""

    def __init__(self, frames: list[bytes]) -> None:
        self._buffer = bytearray()
        for frame in frames:
            # Each frame is length-prefixed [4-byte BE len][data]
            self._buffer.extend(struct.pack(">I", len(frame)))
            self._buffer.extend(frame)
        self._pos = 0

    async def readexactly(self, n: int) -> bytes:
        if self._pos + n > len(self._buffer):
            raise asyncio.IncompleteReadError(
                bytes(self._buffer[self._pos :]), n
            )
        data = bytes(self._buffer[self._pos : self._pos + n])
        self._pos += n
        return data


def _build_relay_response(
    sdk_kem_pk: bytes, sdk_sig_pk: bytes, responder_identity: Identity
) -> tuple[bytes, bytes, bytes]:
    """Simulate relay side: encapsulate, derive keys, build auth message.

    Returns (kem_ciphertext_frame, encrypted_auth_frame, shared_secret).
    """
    # Relay encapsulates against SDK's KEM public key
    kem = oqs.KeyEncapsulation("ML-KEM-1024")
    kem_ct, shared_secret = kem.encap_secret(sdk_kem_pk)

    # Build KemCiphertext response payload: [ct:1568][sig_pk:2592]
    resp_payload = bytes(kem_ct) + responder_identity.public_key
    kem_ct_msg = encode_transport_message(TransportMsgType.KemCiphertext, resp_payload)

    # Derive session keys from responder side (keys are swapped)
    prk = hkdf_extract(b"", bytes(shared_secret))
    resp_send_key = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)
    resp_recv_key = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
    fingerprint = sha3_256(
        bytes(shared_secret) + sdk_sig_pk + responder_identity.public_key
    )

    # Build encrypted auth signature
    sig = responder_identity.sign(fingerprint)
    auth_payload = encode_auth_payload(responder_identity.public_key, sig)
    auth_msg = encode_transport_message(TransportMsgType.AuthSignature, auth_payload)

    # Encrypt with responder's send key (which is SDK's recv key), counter=0
    from chromatindb.crypto import aead_encrypt
    from chromatindb._framing import make_nonce

    nonce = make_nonce(0)
    encrypted_auth = aead_encrypt(auth_msg, b"", nonce, resp_send_key)

    return kem_ct_msg, encrypted_auth, bytes(shared_secret)


class TestPerformHandshake:
    async def test_completes_4step_exchange(self) -> None:
        """perform_handshake with mock relay completes and returns correct keys."""
        sdk_identity = Identity.generate()
        responder_identity = Identity.generate()

        # We need to intercept the SDK's KEM pubkey to build relay response.
        # Use a two-pass approach: first call to get KEM pk, then build mocks.
        # Actually, we need to mock the I/O, so we'll generate the KEM keypair
        # ourselves and inject it.

        # Generate KEM keypair on behalf of SDK
        kem = oqs.KeyEncapsulation("ML-KEM-1024")
        sdk_kem_pk = bytes(kem.generate_keypair())

        # Build relay response
        kem_ct_msg, encrypted_auth, shared_secret = _build_relay_response(
            sdk_kem_pk, sdk_identity.public_key, responder_identity
        )

        # But we can't directly inject the KEM keypair into perform_handshake.
        # Instead, we need to mock at a different level. Let's use a real
        # reader/writer pair via asyncio streams.

        # Alternative: use a captured-writer approach where we extract the
        # SDK's KEM pubkey from what it writes, then respond accordingly.
        # This requires building a responding reader that adapts.

        # Simpler approach: create connected stream pair
        server_reader, server_writer = None, None
        client_reader, client_writer = None, None

        async def relay_handler(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            nonlocal server_reader, server_writer
            server_reader = reader
            server_writer = writer

        server = await asyncio.start_server(relay_handler, "127.0.0.1", 0)
        addr = server.sockets[0].getsockname()
        client_reader, client_writer = await asyncio.open_connection(
            addr[0], addr[1]
        )
        # Wait for handler to be called
        await asyncio.sleep(0.01)

        # Run handshake in a task, simulate relay in parallel
        async def simulate_relay() -> None:
            from chromatindb._framing import (
                make_nonce,
                recv_raw,
                send_encrypted,
                send_raw,
            )
            from chromatindb.crypto import aead_encrypt

            # Step 1: Receive KemPubkey from SDK
            raw = await recv_raw(server_reader)
            msg_type, payload, _ = decode_transport_message(raw)
            assert msg_type == TransportMsgType.KemPubkey
            assert len(payload) == KEM_PK_SIZE + SIG_PK_SIZE

            client_kem_pk = payload[:KEM_PK_SIZE]
            client_sig_pk = payload[KEM_PK_SIZE:]

            # Step 2: Encapsulate and send KemCiphertext
            kem = oqs.KeyEncapsulation("ML-KEM-1024")
            ct, ss = kem.encap_secret(client_kem_pk)
            resp_payload = bytes(ct) + responder_identity.public_key
            resp_msg = encode_transport_message(
                TransportMsgType.KemCiphertext, resp_payload
            )
            await send_raw(server_writer, resp_msg)

            # Derive keys (responder side)
            prk = hkdf_extract(b"", bytes(ss))
            resp_send_key = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)
            resp_recv_key = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
            fp = sha3_256(
                bytes(ss) + client_sig_pk + responder_identity.public_key
            )

            # Step 3: Receive and verify SDK auth (counter=0)
            from chromatindb._framing import recv_encrypted

            auth_plaintext, _ = await recv_encrypted(
                server_reader, resp_recv_key, 0
            )
            auth_type, auth_data, _ = decode_transport_message(auth_plaintext)
            assert auth_type == TransportMsgType.AuthSignature
            pk, sig = decode_auth_payload(auth_data)
            assert pk == sdk_identity.public_key
            assert Identity.verify(fp, sig, pk)

            # Step 4: Send responder auth (counter=0)
            resp_sig = responder_identity.sign(fp)
            resp_auth_payload = encode_auth_payload(
                responder_identity.public_key, resp_sig
            )
            resp_auth_msg = encode_transport_message(
                TransportMsgType.AuthSignature, resp_auth_payload
            )
            await send_encrypted(server_writer, resp_auth_msg, resp_send_key, 0)

        relay_task = asyncio.create_task(simulate_relay())

        send_key, recv_key, send_ctr, recv_ctr, resp_pk = await perform_handshake(
            client_reader, client_writer, sdk_identity
        )

        await relay_task

        # Verify results
        assert send_ctr == 1
        assert recv_ctr == 1
        assert resp_pk == responder_identity.public_key
        assert len(send_key) == 32
        assert len(recv_key) == 32

        # Cleanup
        client_writer.close()
        await client_writer.wait_closed()
        server_writer.close()
        await server_writer.wait_closed()
        server.close()
        await server.wait_closed()

    async def test_sends_kem_pubkey_first(self) -> None:
        """perform_handshake sends KemPubkey([kem_pk:1568][sig_pk:2592]) as first message."""
        sdk_identity = Identity.generate()
        responder_identity = Identity.generate()

        server_received: list[bytes] = []

        async def relay_handler(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            from chromatindb._framing import recv_raw, send_raw, send_encrypted

            # Capture KemPubkey
            raw = await recv_raw(reader)
            server_received.append(raw)

            msg_type, payload, _ = decode_transport_message(raw)
            client_kem_pk = payload[:KEM_PK_SIZE]

            # Respond with KemCiphertext
            kem = oqs.KeyEncapsulation("ML-KEM-1024")
            ct, ss = kem.encap_secret(client_kem_pk)
            resp_payload = bytes(ct) + responder_identity.public_key
            await send_raw(
                writer,
                encode_transport_message(
                    TransportMsgType.KemCiphertext, resp_payload
                ),
            )

            # Derive responder keys
            prk = hkdf_extract(b"", bytes(ss))
            resp_send_key = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)
            resp_recv_key = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
            fp = sha3_256(
                bytes(ss)
                + payload[KEM_PK_SIZE:]
                + responder_identity.public_key
            )

            # Step 3: receive client auth
            from chromatindb._framing import recv_encrypted

            await recv_encrypted(reader, resp_recv_key, 0)

            # Step 4: send responder auth
            sig = responder_identity.sign(fp)
            auth_payload = encode_auth_payload(responder_identity.public_key, sig)
            auth_msg = encode_transport_message(
                TransportMsgType.AuthSignature, auth_payload
            )
            await send_encrypted(writer, auth_msg, resp_send_key, 0)

        server = await asyncio.start_server(relay_handler, "127.0.0.1", 0)
        addr = server.sockets[0].getsockname()
        reader, writer = await asyncio.open_connection(addr[0], addr[1])

        await perform_handshake(reader, writer, sdk_identity)

        # Verify the first message structure
        assert len(server_received) == 1
        msg_type, payload, _ = decode_transport_message(server_received[0])
        assert msg_type == TransportMsgType.KemPubkey
        assert len(payload) == KEM_PK_SIZE + SIG_PK_SIZE
        # Last 2592 bytes should be the SDK's signing pubkey
        assert payload[KEM_PK_SIZE:] == sdk_identity.public_key

        writer.close()
        await writer.wait_closed()
        server.close()
        await server.wait_closed()

    async def test_raises_on_invalid_peer_auth(self) -> None:
        """perform_handshake raises HandshakeError if peer auth signature is invalid."""
        sdk_identity = Identity.generate()
        responder_identity = Identity.generate()
        wrong_identity = Identity.generate()  # Will sign with wrong key

        async def relay_handler(
            reader: asyncio.StreamReader, writer: asyncio.StreamWriter
        ) -> None:
            from chromatindb._framing import recv_raw, send_raw, send_encrypted, recv_encrypted

            raw = await recv_raw(reader)
            msg_type, payload, _ = decode_transport_message(raw)
            client_kem_pk = payload[:KEM_PK_SIZE]

            kem = oqs.KeyEncapsulation("ML-KEM-1024")
            ct, ss = kem.encap_secret(client_kem_pk)
            resp_payload = bytes(ct) + responder_identity.public_key
            await send_raw(
                writer,
                encode_transport_message(
                    TransportMsgType.KemCiphertext, resp_payload
                ),
            )

            prk = hkdf_extract(b"", bytes(ss))
            resp_send_key = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)
            resp_recv_key = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
            fp = sha3_256(
                bytes(ss)
                + payload[KEM_PK_SIZE:]
                + responder_identity.public_key
            )

            await recv_encrypted(reader, resp_recv_key, 0)

            # Sign with WRONG identity (signature won't verify)
            bad_sig = wrong_identity.sign(fp)
            auth_payload = encode_auth_payload(
                responder_identity.public_key, bad_sig
            )
            auth_msg = encode_transport_message(
                TransportMsgType.AuthSignature, auth_payload
            )
            await send_encrypted(writer, auth_msg, resp_send_key, 0)

        server = await asyncio.start_server(relay_handler, "127.0.0.1", 0)
        addr = server.sockets[0].getsockname()
        reader, writer = await asyncio.open_connection(addr[0], addr[1])

        with pytest.raises(HandshakeError, match="peer auth signature invalid"):
            await perform_handshake(reader, writer, sdk_identity)

        writer.close()
        await writer.wait_closed()
        server.close()
        await server.wait_closed()
