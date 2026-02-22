import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crypto_utils import (
    generate_keypair,
    load_keypair,
    save_keypair,
    fingerprint_of,
    sign,
    mine_pow,
    count_leading_zero_bits,
)


def test_generate_keypair():
    pubkey, seckey = generate_keypair()
    assert len(pubkey) == 2592
    assert len(seckey) == 4896


def test_fingerprint():
    pubkey, _ = generate_keypair()
    fp = fingerprint_of(pubkey)
    assert len(fp) == 32
    assert fingerprint_of(pubkey) == fp


def test_sign():
    pubkey, seckey = generate_keypair()
    message = b"chromatin-auth:" + os.urandom(32)
    sig = sign(seckey, message)
    assert len(sig) == 4627


def test_save_and_load_keypair():
    pubkey, seckey = generate_keypair()
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "test.key")
        save_keypair(path, pubkey, seckey)
        loaded_pub, loaded_sec = load_keypair(path)
        assert loaded_pub == pubkey
        assert loaded_sec == seckey


def test_count_leading_zero_bits():
    assert count_leading_zero_bits(b"\x00\x00\x00\xff") == 24
    assert count_leading_zero_bits(b"\x00\x00\xff\xff") == 16
    assert count_leading_zero_bits(b"\x00\x01\xff\xff") == 15
    assert count_leading_zero_bits(b"\xff\xff\xff\xff") == 0
    assert count_leading_zero_bits(b"\x00\x00\x00\x00") == 32


def test_mine_pow():
    prefix = b"test-prefix"
    nonce = mine_pow(prefix, difficulty=8)
    import hashlib
    h = hashlib.sha3_256(prefix + nonce.to_bytes(8, "big")).digest()
    assert count_leading_zero_bits(h) >= 8
