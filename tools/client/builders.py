"""Binary record builders for SET_PROFILE, REGISTER_NAME, and GROUP_META."""

import struct

from crypto_utils import sign

# Profile sub-field limits (must match config::protocol:: in C++)
MAX_BIO_SIZE = 2048
MAX_AVATAR_SIZE = 256 * 1024
MAX_SOCIAL_LINKS = 16
MAX_SOCIAL_PLATFORM_LENGTH = 64
MAX_SOCIAL_HANDLE_LENGTH = 128


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
    if len(bio_bytes) > MAX_BIO_SIZE:
        raise ValueError(f"bio exceeds {MAX_BIO_SIZE} byte limit ({len(bio_bytes)} bytes)")
    if len(avatar) > MAX_AVATAR_SIZE:
        raise ValueError(f"avatar exceeds {MAX_AVATAR_SIZE} byte limit ({len(avatar)} bytes)")
    if len(social_links) > MAX_SOCIAL_LINKS:
        raise ValueError(f"social_links count exceeds {MAX_SOCIAL_LINKS} limit ({len(social_links)})")
    for platform, handle in social_links:
        p = platform.encode("utf-8")
        h = handle.encode("utf-8")
        if len(p) > MAX_SOCIAL_PLATFORM_LENGTH:
            raise ValueError(f"platform string exceeds {MAX_SOCIAL_PLATFORM_LENGTH} byte limit ({len(p)} bytes)")
        if len(h) > MAX_SOCIAL_HANDLE_LENGTH:
            raise ValueError(f"handle string exceeds {MAX_SOCIAL_HANDLE_LENGTH} byte limit ({len(h)} bytes)")
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

    sig = sign(seckey, b"chromatin:profile:" + bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)


def build_name_record(
    seckey: bytes,
    name: str,
    fingerprint: bytes,
    pubkey: bytes,
    pow_nonce: int,
    sequence: int,
) -> bytes:
    """Build a signed name registration record.

    Format:
        name_len(1) || name(UTF-8) ||
        fingerprint(32) ||
        pow_nonce(8 BE) ||
        sequence(8 BE) ||
        pubkey_len(2 BE) || pubkey ||
        sig_len(2 BE) || signature
    """
    name_bytes = name.encode("utf-8")
    body = bytearray()
    body += struct.pack("B", len(name_bytes))
    body += name_bytes
    body += fingerprint
    body += struct.pack(">Q", pow_nonce)
    body += struct.pack(">Q", sequence)
    body += struct.pack(">H", len(pubkey))
    body += pubkey

    sig = sign(seckey, b"chromatin:name:" + bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)


def build_group_meta(
    seckey: bytes,
    group_id: bytes,
    owner_fingerprint: bytes,
    signer_fingerprint: bytes,
    version: int,
    members: list[tuple[bytes, int, bytes, bytes]],
    signer_pubkey: bytes,
) -> bytes:
    """Build a signed GROUP_META binary record.

    Format:
        group_id(32) ||
        owner_fingerprint(32) ||
        signer_fingerprint(32) ||
        version(4 BE) ||
        member_count(2 BE) ||
          [member_fp(32) || role(1) || kem_ciphertext(1568) || wrapped_gek(48)] * count ||
        signer_pubkey_len(2 BE) || signer_pubkey ||
        sig_len(2 BE) || signature
    """
    body = bytearray()
    body += group_id
    body += owner_fingerprint
    body += signer_fingerprint
    body += struct.pack(">I", version)
    body += struct.pack(">H", len(members))
    for fp, role, kem_ct, wrapped_gek in members:
        body += fp
        body += struct.pack("B", role)
        body += kem_ct
        body += wrapped_gek

    # Signer pubkey (self-verifiable — SHA3(pubkey) == signer_fingerprint)
    body += struct.pack(">H", len(signer_pubkey))
    body += signer_pubkey

    sig = sign(seckey, bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)
