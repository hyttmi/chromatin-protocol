"""ML-DSA-87 key management, SHA3-256 hashing, and proof-of-work mining."""

import hashlib
import os
import struct

import oqs


def generate_keypair() -> tuple[bytes, bytes]:
    """Generate an ML-DSA-87 keypair. Returns (pubkey, seckey)."""
    signer = oqs.Signature("ML-DSA-87")
    pubkey = signer.generate_keypair()
    seckey = signer.export_secret_key()
    return bytes(pubkey), bytes(seckey)


def fingerprint_of(pubkey: bytes) -> bytes:
    """SHA3-256 hash of a public key."""
    return hashlib.sha3_256(pubkey).digest()


def sign(seckey: bytes, message: bytes) -> bytes:
    """Sign a message with ML-DSA-87."""
    signer = oqs.Signature("ML-DSA-87", secret_key=seckey)
    return bytes(signer.sign(message))


def save_keypair(path: str, pubkey: bytes, seckey: bytes) -> None:
    """Save keypair to file. Format: pubkey_len(4 BE) || pubkey || seckey."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack(">I", len(pubkey)))
        f.write(pubkey)
        f.write(seckey)


def load_keypair(path: str) -> tuple[bytes, bytes]:
    """Load keypair from file."""
    with open(path, "rb") as f:
        data = f.read()
    pubkey_len = struct.unpack(">I", data[:4])[0]
    pubkey = data[4 : 4 + pubkey_len]
    seckey = data[4 + pubkey_len :]
    return pubkey, seckey


def count_leading_zero_bits(data: bytes) -> int:
    """Count leading zero bits in a byte string."""
    bits = 0
    for byte in data:
        if byte == 0:
            bits += 8
        else:
            for i in range(7, -1, -1):
                if byte & (1 << i):
                    return bits
                bits += 1
            return bits
    return bits


def mine_pow(prefix: bytes, difficulty: int) -> int:
    """Mine a proof-of-work nonce. Returns nonce such that
    SHA3-256(prefix || nonce_BE(8)) has >= difficulty leading zero bits."""
    nonce = 0
    while True:
        h = hashlib.sha3_256(prefix + nonce.to_bytes(8, "big")).digest()
        if count_leading_zero_bits(h) >= difficulty:
            return nonce
        nonce += 1
