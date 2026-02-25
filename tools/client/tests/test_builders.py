import os
import sys
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pytest

from crypto_utils import generate_keypair, fingerprint_of
from builders import (
    build_profile_record, build_name_record, build_group_meta,
    MAX_BIO_SIZE, MAX_AVATAR_SIZE, MAX_SOCIAL_LINKS,
    MAX_SOCIAL_PLATFORM_LENGTH, MAX_SOCIAL_HANDLE_LENGTH,
)


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


def test_profile_bio_exceeds_limit():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    with pytest.raises(ValueError, match="bio exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="A" * (MAX_BIO_SIZE + 1), avatar=b"", social_links=[], sequence=1,
        )


def test_profile_avatar_exceeds_limit():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    with pytest.raises(ValueError, match="avatar exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"\xff" * (MAX_AVATAR_SIZE + 1), social_links=[], sequence=1,
        )


def test_profile_too_many_social_links():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    links = [("x", "y")] * (MAX_SOCIAL_LINKS + 1)
    with pytest.raises(ValueError, match="social_links count exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"", social_links=links, sequence=1,
        )


def test_profile_platform_too_long():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    links = [("p" * (MAX_SOCIAL_PLATFORM_LENGTH + 1), "h")]
    with pytest.raises(ValueError, match="platform string exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"", social_links=links, sequence=1,
        )


def test_profile_handle_too_long():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    links = [("p", "h" * (MAX_SOCIAL_HANDLE_LENGTH + 1))]
    with pytest.raises(ValueError, match="handle string exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"", social_links=links, sequence=1,
        )


def test_profile_at_field_limits():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    record = build_profile_record(
        seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
        bio="B" * MAX_BIO_SIZE, avatar=b"",
        social_links=[("p" * MAX_SOCIAL_PLATFORM_LENGTH, "h" * MAX_SOCIAL_HANDLE_LENGTH)],
        sequence=1,
    )
    assert record[:32] == fp
