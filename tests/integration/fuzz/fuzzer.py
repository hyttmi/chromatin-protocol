#!/usr/bin/env python3
"""
chromatindb Protocol & Handshake Fuzzer

Self-contained Python fuzzer using raw TCP sockets. Sends crafted malformed
payloads to a chromatindb node to verify graceful handling of invalid input.

Modes:
  protocol  -- 13 payload categories covering core, crypto, and semantic malformations
  handshake -- 7 payloads targeting each PQ handshake stage independently

Exit code 0 if all payloads were sent successfully (regardless of server response).
The calling test script verifies that the node survived.
"""

import argparse
import os
import random
import socket
import struct
import sys
import time


def log(msg: str) -> None:
    print(f"[fuzzer] {msg}", flush=True)


def send_payload(host: str, port: int, timeout: float,
                 payload: bytes, name: str, idx: int,
                 recv_first: bool = False) -> bool:
    """
    Open a fresh TCP connection, optionally read first, send payload,
    wait for response/disconnect, close.

    Returns True if the payload was sent (connection may or may not succeed).
    """
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((host, port))
    except (ConnectionRefusedError, OSError) as e:
        log(f"PAYLOAD {idx} ({name}): connection refused/failed: {e}")
        return True  # Server alive but rejected -- that's fine

    first_response = b""
    if recv_first:
        try:
            first_response = sock.recv(8192)
        except socket.timeout:
            pass
        except (ConnectionResetError, BrokenPipeError, OSError):
            pass

    try:
        sock.sendall(payload)
    except (ConnectionResetError, BrokenPipeError, OSError) as e:
        log(f"PAYLOAD {idx} ({name}): sent 0 bytes (send failed: {e}), "
            f"result: disconnected")
        sock.close()
        return True

    # Try to read response
    try:
        response = sock.recv(8192)
        if len(response) == 0:
            log(f"PAYLOAD {idx} ({name}): sent {len(payload)} bytes, "
                f"result: disconnected")
        else:
            log(f"PAYLOAD {idx} ({name}): sent {len(payload)} bytes, "
                f"result: response({len(response)} bytes)")
    except socket.timeout:
        log(f"PAYLOAD {idx} ({name}): sent {len(payload)} bytes, "
            f"result: timeout")
    except (ConnectionResetError, BrokenPipeError, OSError):
        log(f"PAYLOAD {idx} ({name}): sent {len(payload)} bytes, "
            f"result: disconnected")

    sock.close()
    return True


def make_length_prefix(length: int) -> bytes:
    """4-byte big-endian uint32 length prefix (chromatindb framing)."""
    return struct.pack(">I", length)


def random_bytes(n: int) -> bytes:
    return os.urandom(n)


# =============================================================================
# Protocol Mode Payloads (SAN-04)
# =============================================================================

def protocol_payloads() -> list:
    """
    Return list of (name, payload_bytes, recv_first) tuples.
    13 payload categories covering core protocol, crypto, and semantic.
    """
    payloads = []

    # --- Core Protocol ---

    # 1. Truncated length prefix (1 byte instead of 4)
    payloads.append((
        "truncated length prefix",
        b"\x00",
        False
    ))

    # 2. Oversized length prefix (0xFFFFFFFF followed by 16 bytes)
    payloads.append((
        "oversized length prefix",
        struct.pack(">I", 0xFFFFFFFF) + random_bytes(16),
        False
    ))

    # 3. Zero-length frame (4 bytes of 0x00)
    payloads.append((
        "zero-length frame",
        b"\x00\x00\x00\x00",
        False
    ))

    # 4. Pure garbage bytes (256 random bytes)
    payloads.append((
        "pure garbage bytes",
        random_bytes(256),
        False
    ))

    # 5. Valid 4-byte length prefix + garbage payload
    #    Length says 100, send 100 random bytes
    payloads.append((
        "valid length + garbage payload",
        make_length_prefix(100) + random_bytes(100),
        False
    ))

    # 6. Truncated FlatBuffer mid-field
    #    Valid length prefix, 50 bytes of partial FlatBuffer-like data
    #    starting with valid root offset but truncated
    fb_partial = struct.pack("<I", 16)  # root offset (little-endian, FlatBuffer style)
    fb_partial += random_bytes(46)      # rest of partial data
    payloads.append((
        "truncated FlatBuffer mid-field",
        make_length_prefix(50) + fb_partial,
        False
    ))

    # --- Crypto-specific ---

    # 7. Invalid AEAD tag (24-byte nonce + 64 bytes data + 16 bytes 0xFF tag)
    #    Note: chromatindb uses 12-byte nonces, but we send 24 to test wrong sizes too
    payloads.append((
        "invalid AEAD tag",
        make_length_prefix(104) + random_bytes(24) + random_bytes(64) + (b"\xFF" * 16),
        False
    ))

    # 8. Wrong nonce length (12 bytes instead of 24, followed by 64 bytes)
    #    The framing expects ciphertext, not nonce+data, but this tests
    #    how the node handles unexpected byte patterns
    payloads.append((
        "wrong nonce length",
        make_length_prefix(76) + random_bytes(12) + random_bytes(64),
        False
    ))

    # 9. Truncated ciphertext (24-byte nonce + 8 bytes, then close)
    payloads.append((
        "truncated ciphertext",
        make_length_prefix(32) + random_bytes(24) + random_bytes(8),
        False
    ))

    # 10. Replay with modified bytes
    #     Connect, capture first server response, replay with 1 bit flipped
    payloads.append((
        "replay modified bytes",
        b"",  # Special handling below
        True  # recv_first=True
    ))

    # --- Semantic ---

    # 11. Valid FlatBuffer-ish structure with impossible message type enum
    #     type byte = 0xFF at expected FlatBuffer enum offset
    fb_semantic = bytearray(64)
    struct.pack_into("<I", fb_semantic, 0, 4)  # root offset
    struct.pack_into("<I", fb_semantic, 4, 0)  # vtable offset
    fb_semantic[8] = 0xFF                       # impossible type enum
    payloads.append((
        "impossible message type enum",
        make_length_prefix(64) + bytes(fb_semantic),
        False
    ))

    # 12. Valid structure with negative-looking sizes (0xFFFFFFFF as size field)
    fb_neg = bytearray(64)
    struct.pack_into("<I", fb_neg, 0, 4)
    struct.pack_into("<I", fb_neg, 4, 0xFFFFFFFF)  # impossibly large size
    payloads.append((
        "negative-looking size fields",
        make_length_prefix(64) + bytes(fb_neg),
        False
    ))

    # 13. Future version byte (version = 0xFF at offset 0)
    fb_ver = bytearray(64)
    fb_ver[0] = 0xFF  # future version byte
    payloads.append((
        "future version byte",
        make_length_prefix(64) + bytes(fb_ver),
        False
    ))

    return payloads


def run_protocol_mode(host: str, port: int, timeout: float) -> bool:
    """Run all protocol fuzzing payloads. Returns True if all sent."""
    log(f"=== Protocol Fuzzing Mode ===")
    log(f"Target: {host}:{port}, timeout: {timeout}s")

    payloads = protocol_payloads()
    all_sent = True

    for idx, (name, payload, recv_first) in enumerate(payloads, 1):
        # Special case: replay with modified bytes
        if name == "replay modified bytes":
            all_sent &= _send_replay_payload(host, port, timeout, idx)
            continue

        if not send_payload(host, port, timeout, payload, name, idx,
                            recv_first=recv_first):
            all_sent = False

        # Small delay between payloads to avoid overwhelming
        time.sleep(0.1)

    log(f"=== Protocol Fuzzing Complete: {len(payloads)} payloads sent ===")
    return all_sent


def _send_replay_payload(host: str, port: int, timeout: float,
                         idx: int) -> bool:
    """
    Connect, capture first server response bytes (if any),
    replay with 1 bit flipped.
    """
    name = "replay modified bytes"
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((host, port))
    except (ConnectionRefusedError, OSError) as e:
        log(f"PAYLOAD {idx} ({name}): connection failed: {e}")
        return True

    # Read whatever the server sends first (likely nothing for chromatindb
    # since the initiator speaks first, but we try anyway)
    captured = b""
    try:
        captured = sock.recv(4096)
    except socket.timeout:
        pass
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass
    sock.close()

    # If we got data, flip a bit and replay it
    if len(captured) > 0:
        modified = bytearray(captured)
        # Flip bit 0 of the first byte
        modified[0] ^= 0x01
        replay_data = bytes(modified)
    else:
        # No server data captured -- send random bytes as "replay"
        replay_data = random_bytes(64)

    return send_payload(host, port, timeout, replay_data, name, idx)


# =============================================================================
# Handshake Mode Payloads (SAN-05)
# =============================================================================

def run_handshake_mode(host: str, port: int, timeout: float) -> bool:
    """Run all handshake fuzzing payloads. Returns True if all sent."""
    log(f"=== Handshake Fuzzing Mode ===")
    log(f"Target: {host}:{port}, timeout: {timeout}s")

    all_sent = True
    idx = 0

    # --- Stage 1: Malformed ClientHello ---
    # In chromatindb, the initiator sends the first message (KemPubkey).
    # The raw handshake messages are length-prefixed FlatBuffers.

    # 1a. Truncated hello (4 bytes length + 10 bytes, close)
    idx += 1
    name = "stage1: truncated hello"
    payload = make_length_prefix(10) + random_bytes(10)
    all_sent &= send_payload(host, port, timeout, payload, name, idx)
    time.sleep(0.1)

    # 1b. Oversized hello (length prefix claims 10MB, send 100 bytes, close)
    idx += 1
    name = "stage1: oversized hello"
    payload = make_length_prefix(10 * 1024 * 1024) + random_bytes(100)
    all_sent &= send_payload(host, port, timeout, payload, name, idx)
    time.sleep(0.1)

    # 1c. Wrong magic/version byte at offset 0
    idx += 1
    name = "stage1: wrong magic/version"
    bad_hello = bytearray(200)
    bad_hello[0] = 0xFF  # invalid version/magic
    payload = bytes(bad_hello)
    all_sent &= send_payload(host, port, timeout, payload, name, idx)
    time.sleep(0.1)

    # --- Stage 2: Malformed KEM ciphertext response ---
    # Send a valid-length ClientHello-sized frame first, then garbage

    # 2a. Valid-length ClientHello + garbage KEM ciphertext
    idx += 1
    name = "stage2: garbage KEM ciphertext"
    all_sent &= _send_stage2_payload(host, port, timeout, name, idx,
                                     kem_response=random_bytes(200))
    time.sleep(0.1)

    # 2b. Valid-length ClientHello + truncated ciphertext (50 bytes)
    idx += 1
    name = "stage2: truncated KEM ciphertext"
    all_sent &= _send_stage2_payload(host, port, timeout, name, idx,
                                     kem_response=random_bytes(50))
    time.sleep(0.1)

    # --- Stage 3: Malformed session confirmation ---
    # Send two valid-length frames, then garbage as session confirmation

    # 3a. Two frames + garbage session confirmation
    idx += 1
    name = "stage3: garbage session confirmation"
    all_sent &= _send_stage3_payload(host, port, timeout, name, idx,
                                     confirm=random_bytes(200))
    time.sleep(0.1)

    # 3b. Two frames + just close (zero bytes as confirmation)
    idx += 1
    name = "stage3: close without confirmation"
    all_sent &= _send_stage3_payload(host, port, timeout, name, idx,
                                     confirm=b"")
    time.sleep(0.1)

    log(f"=== Handshake Fuzzing Complete: {idx} payloads sent ===")
    return all_sent


def _send_stage2_payload(host: str, port: int, timeout: float,
                         name: str, idx: int,
                         kem_response: bytes) -> bool:
    """
    Send a valid-length ClientHello-sized frame (simulating KemPubkey),
    wait for server response, then send malformed KEM ciphertext.
    """
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((host, port))
    except (ConnectionRefusedError, OSError) as e:
        log(f"PAYLOAD {idx} ({name}): connection failed: {e}")
        return True

    # Send a ClientHello-sized frame (200 random bytes with length prefix)
    # This simulates a KemPubkey message (normally 1568 bytes, but the node
    # will try to parse it regardless of size)
    hello = random_bytes(200)
    try:
        sock.sendall(hello)
    except (ConnectionResetError, BrokenPipeError, OSError) as e:
        log(f"PAYLOAD {idx} ({name}): send hello failed: {e}, "
            f"result: disconnected")
        sock.close()
        return True

    # Wait for server response (KemCiphertext)
    try:
        response = sock.recv(8192)
        if len(response) == 0:
            log(f"PAYLOAD {idx} ({name}): server disconnected after hello")
            sock.close()
            return True
    except socket.timeout:
        log(f"PAYLOAD {idx} ({name}): no server response after hello (timeout)")
        sock.close()
        return True
    except (ConnectionResetError, BrokenPipeError, OSError):
        log(f"PAYLOAD {idx} ({name}): server disconnected after hello")
        sock.close()
        return True

    # Send malformed KEM ciphertext response
    try:
        sock.sendall(kem_response)
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass

    # Check for server disconnect
    try:
        final = sock.recv(4096)
        if len(final) == 0:
            log(f"PAYLOAD {idx} ({name}): sent {len(kem_response)} bytes, "
                f"result: disconnected")
        else:
            log(f"PAYLOAD {idx} ({name}): sent {len(kem_response)} bytes, "
                f"result: response({len(final)} bytes)")
    except socket.timeout:
        log(f"PAYLOAD {idx} ({name}): sent {len(kem_response)} bytes, "
            f"result: timeout")
    except (ConnectionResetError, BrokenPipeError, OSError):
        log(f"PAYLOAD {idx} ({name}): sent {len(kem_response)} bytes, "
            f"result: disconnected")

    sock.close()
    return True


def _send_stage3_payload(host: str, port: int, timeout: float,
                         name: str, idx: int,
                         confirm: bytes) -> bool:
    """
    Send two valid-length frames (simulating KemPubkey and AuthSignature),
    then send malformed session confirmation (or just close).
    """
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((host, port))
    except (ConnectionRefusedError, OSError) as e:
        log(f"PAYLOAD {idx} ({name}): connection failed: {e}")
        return True

    # Frame 1: simulated KemPubkey (200 random bytes, raw)
    hello = random_bytes(200)
    try:
        sock.sendall(hello)
    except (ConnectionResetError, BrokenPipeError, OSError) as e:
        log(f"PAYLOAD {idx} ({name}): send frame 1 failed: {e}")
        sock.close()
        return True

    # Wait for server KemCiphertext response
    try:
        resp1 = sock.recv(8192)
        if len(resp1) == 0:
            log(f"PAYLOAD {idx} ({name}): server disconnected after frame 1")
            sock.close()
            return True
    except socket.timeout:
        log(f"PAYLOAD {idx} ({name}): no response after frame 1 (timeout)")
        sock.close()
        return True
    except (ConnectionResetError, BrokenPipeError, OSError):
        log(f"PAYLOAD {idx} ({name}): server disconnected after frame 1")
        sock.close()
        return True

    # Frame 2: simulated AuthSignature (length-prefixed encrypted frame)
    auth_frame = make_length_prefix(200) + random_bytes(200)
    try:
        sock.sendall(auth_frame)
    except (ConnectionResetError, BrokenPipeError, OSError):
        log(f"PAYLOAD {idx} ({name}): send frame 2 failed, "
            f"result: disconnected")
        sock.close()
        return True

    # Wait for server response
    try:
        resp2 = sock.recv(8192)
    except socket.timeout:
        pass
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass

    # Frame 3: malformed session confirmation (or just close)
    if len(confirm) > 0:
        try:
            sock.sendall(confirm)
        except (ConnectionResetError, BrokenPipeError, OSError):
            pass

        # Check for disconnect
        try:
            final = sock.recv(4096)
            if len(final) == 0:
                log(f"PAYLOAD {idx} ({name}): sent {len(confirm)} bytes, "
                    f"result: disconnected")
            else:
                log(f"PAYLOAD {idx} ({name}): sent {len(confirm)} bytes, "
                    f"result: response({len(final)} bytes)")
        except socket.timeout:
            log(f"PAYLOAD {idx} ({name}): sent {len(confirm)} bytes, "
                f"result: timeout")
        except (ConnectionResetError, BrokenPipeError, OSError):
            log(f"PAYLOAD {idx} ({name}): sent {len(confirm)} bytes, "
                f"result: disconnected")
    else:
        log(f"PAYLOAD {idx} ({name}): closed without sending confirmation")

    sock.close()
    return True


# =============================================================================
# Main
# =============================================================================

def parse_target(target: str) -> tuple:
    """Parse HOST:PORT string."""
    parts = target.rsplit(":", 1)
    if len(parts) != 2:
        raise argparse.ArgumentTypeError(
            f"Invalid target format '{target}', expected HOST:PORT")
    host = parts[0]
    try:
        port = int(parts[1])
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"Invalid port '{parts[1]}' in target '{target}'")
    return host, port


def main() -> int:
    parser = argparse.ArgumentParser(
        description="chromatindb protocol and handshake fuzzer")
    parser.add_argument("--target", required=True,
                        help="Target HOST:PORT")
    parser.add_argument("--mode", required=True,
                        choices=["protocol", "handshake"],
                        help="Fuzzing mode: protocol or handshake")
    parser.add_argument("--timeout", type=float, default=2.0,
                        help="Per-connection timeout in seconds (default: 2)")

    args = parser.parse_args()
    host, port = parse_target(args.target)

    log(f"chromatindb fuzzer starting: mode={args.mode}, "
        f"target={host}:{port}, timeout={args.timeout}s")

    if args.mode == "protocol":
        success = run_protocol_mode(host, port, args.timeout)
    else:
        success = run_handshake_mode(host, port, args.timeout)

    if success:
        log("All payloads sent successfully")
        return 0
    else:
        log("Some payloads failed to send")
        return 1


if __name__ == "__main__":
    sys.exit(main())
