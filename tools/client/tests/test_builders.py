import os
import sys
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crypto_utils import generate_keypair, fingerprint_of
from builders import build_profile_record, build_name_record, build_group_meta


def test_build_profile_record():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    record = build_profile_record(
        seckey=seckey,
        fingerprint=fp,
        pubkey=pubkey,
        kem_pubkey=b"",
        bio="Hello world",
        avatar=b"",
        social_links=[],
        sequence=1,
    )
    assert record[:32] == fp
    offset = 32
    pk_len = struct.unpack(">H", record[offset : offset + 2])[0]
    assert pk_len == len(pubkey)


def test_build_name_record():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    record = build_name_record(
        seckey=seckey,
        name="alice",
        fingerprint=fp,
        pubkey=pubkey,
        pow_nonce=0,
        sequence=1,
    )
    assert record[0] == 5
    assert record[1:6] == b"alice"
    assert record[6:38] == fp
    # After fingerprint(32) + pow_nonce(8) + sequence(8) = offset 38+16=54
    pk_len = struct.unpack(">H", record[54:56])[0]
    assert pk_len == len(pubkey)
    assert record[56 : 56 + pk_len] == pubkey


def test_build_group_meta():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    group_id = os.urandom(32)
    kem_dummy = b"\x00" * 1568
    members = [(fp, 0x02, kem_dummy)]
    meta = build_group_meta(seckey, group_id, fp, 1, members)
    assert meta[:32] == group_id
    assert meta[32:64] == fp
    version = struct.unpack(">I", meta[64:68])[0]
    assert version == 1
    member_count = struct.unpack(">H", meta[68:70])[0]
    assert member_count == 1
