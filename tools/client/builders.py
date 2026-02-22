"""Binary record builders for SET_PROFILE, REGISTER_NAME, and GROUP_META."""

import struct

from crypto_utils import sign


def build_profile_record(
    seckey: bytes,
    fingerprint: bytes,
    pubkey: bytes,
    kem_pubkey: bytes,
    bio: str,
    avatar: bytes,
    social_links: list[tuple[str, str]],
    sequence: int,
) -> bytes:
    """Build a signed profile binary record.

    Format:
        fingerprint(32) ||
        pubkey_len(2 BE) || pubkey ||
        kem_pubkey_len(2 BE) || kem_pubkey ||
        bio_len(2 BE) || bio(UTF-8) ||
        avatar_len(4 BE) || avatar ||
        social_links_count(1) ||
          [platform_len(1) || platform || handle_len(1) || handle] * count ||
        sequence(8 BE) ||
        sig_len(2 BE) || signature
    """
    bio_bytes = bio.encode("utf-8")
    body = bytearray()
    body += fingerprint
    body += struct.pack(">H", len(pubkey))
    body += pubkey
    body += struct.pack(">H", len(kem_pubkey))
    body += kem_pubkey
    body += struct.pack(">H", len(bio_bytes))
    body += bio_bytes
    body += struct.pack(">I", len(avatar))
    body += avatar
    body += struct.pack("B", len(social_links))
    for platform, handle in social_links:
        p = platform.encode("utf-8")
        h = handle.encode("utf-8")
        body += struct.pack("B", len(p))
        body += p
        body += struct.pack("B", len(h))
        body += h
    body += struct.pack(">Q", sequence)

    sig = sign(seckey, bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)


def build_name_record(
    seckey: bytes,
    name: str,
    fingerprint: bytes,
    pow_nonce: int,
    sequence: int,
) -> bytes:
    """Build a signed name registration record.

    Format:
        name_len(1) || name(UTF-8) ||
        fingerprint(32) ||
        pow_nonce(8 BE) ||
        sequence(8 BE) ||
        sig_len(2 BE) || signature
    """
    name_bytes = name.encode("utf-8")
    body = bytearray()
    body += struct.pack("B", len(name_bytes))
    body += name_bytes
    body += fingerprint
    body += struct.pack(">Q", pow_nonce)
    body += struct.pack(">Q", sequence)

    sig = sign(seckey, bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)


def build_group_meta(
    seckey: bytes,
    group_id: bytes,
    owner_fingerprint: bytes,
    version: int,
    members: list[tuple[bytes, int, bytes]],
) -> bytes:
    """Build a signed GROUP_META binary record.

    Format:
        group_id(32) ||
        owner_fingerprint(32) ||
        version(4 BE) ||
        member_count(2 BE) ||
          [member_fp(32) || role(1) || kem_ciphertext(1568)] * count ||
        sig_len(2 BE) || signature
    """
    body = bytearray()
    body += group_id
    body += owner_fingerprint
    body += struct.pack(">I", version)
    body += struct.pack(">H", len(members))
    for fp, role, kem_ct in members:
        body += fp
        body += struct.pack("B", role)
        body += kem_ct

    sig = sign(seckey, bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)
