"""ML-DSA-87 key management, ML-KEM-1024 encapsulation, AES-256-GCM,
SHA3-256 hashing, proof-of-work mining, and E2E message encryption."""

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


# ---------------------------------------------------------------------------
# E2E encryption helpers
# ---------------------------------------------------------------------------


def sha3_256(data: bytes) -> bytes:
    """SHA3-256 hash."""
    return hashlib.sha3_256(data).digest()


def kem_generate_keypair() -> tuple[bytes, bytes]:
    """Generate ML-KEM-1024 keypair. Returns (pubkey, seckey)."""
    kem = oqs.KeyEncapsulation("ML-KEM-1024")
    pk = kem.generate_keypair()
    sk = kem.export_secret_key()
    return bytes(pk), bytes(sk)


def kem_encapsulate(pubkey: bytes) -> tuple[bytes, bytes]:
    """Encapsulate to pubkey. Returns (ciphertext, shared_secret)."""
    kem = oqs.KeyEncapsulation("ML-KEM-1024")
    ct, ss = kem.encap_secret(pubkey)
    return bytes(ct), bytes(ss)


def kem_decapsulate(ciphertext: bytes, seckey: bytes) -> bytes:
    """Decapsulate with secret key. Returns shared_secret."""
    kem = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=seckey)
    return bytes(kem.decap_secret(ciphertext))


def aes_gcm_encrypt(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes) -> bytes:
    """AES-256-GCM encrypt. Returns ciphertext || tag (16 bytes)."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    aesgcm = AESGCM(key)
    return aesgcm.encrypt(nonce, plaintext, aad)


def aes_gcm_decrypt(key: bytes, nonce: bytes, ciphertext_and_tag: bytes, aad: bytes) -> bytes:
    """AES-256-GCM decrypt. Returns plaintext."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    aesgcm = AESGCM(key)
    return aesgcm.decrypt(nonce, ciphertext_and_tag, aad)


def encrypt_1to1_message(
    sender_sk: bytes, sender_fp: bytes, recipient_fp: bytes,
    recipient_kem_pk: bytes, content: bytes
) -> bytes:
    """Encrypt a 1:1 message blob (sign-then-encrypt).

    Returns: kem_ciphertext(1568) || nonce(12) || aes_ciphertext || tag(16)
    """
    # Sign
    sig_data = b"chromatin:msg:" + sender_fp + recipient_fp + content
    sig = sign(sender_sk, sig_data)

    # Build plaintext: sender_fp(32) || content || sig_len(2 BE) || sig
    plaintext = sender_fp + content + struct.pack(">H", len(sig)) + sig

    # KEM encapsulate
    kem_ct, ss = kem_encapsulate(recipient_kem_pk)

    # Derive message key
    message_key = sha3_256(b"chromatin:msg:1:1:" + ss)

    # Encrypt
    nonce = os.urandom(12)
    ct_and_tag = aes_gcm_encrypt(message_key, nonce, plaintext, kem_ct)

    return kem_ct + nonce + ct_and_tag


def decrypt_1to1_message(
    recipient_kem_sk: bytes, recipient_fp: bytes, blob: bytes
) -> tuple[bytes, bytes, bytes]:
    """Decrypt a 1:1 message blob.

    Returns: (sender_fp, content, signature)
    """
    kem_ct = blob[:1568]
    nonce = blob[1568:1580]
    ct_and_tag = blob[1580:]

    ss = kem_decapsulate(kem_ct, recipient_kem_sk)
    message_key = sha3_256(b"chromatin:msg:1:1:" + ss)
    plaintext = aes_gcm_decrypt(message_key, nonce, ct_and_tag, kem_ct)

    sender_fp = plaintext[:32]
    # ML-DSA-87 signature is fixed 4627 bytes
    sig = plaintext[-(4627):]
    sig_len_bytes = plaintext[-(4627 + 2):-(4627)]
    sig_len = struct.unpack(">H", sig_len_bytes)[0]
    assert sig_len == 4627, f"unexpected sig_len: {sig_len}"
    content = plaintext[32:-(4627 + 2)]

    return sender_fp, content, sig


# ---------------------------------------------------------------------------
# Group E2E encryption helpers
# ---------------------------------------------------------------------------


def wrap_gek_for_member(member_kem_pk: bytes, gek: bytes) -> tuple[bytes, bytes]:
    """Wrap a GEK for one member via ML-KEM-1024.
    Returns (kem_ciphertext(1568), wrapped_gek(48)).
    wrapped_gek = AES-256-GCM(wrap_key, zeros(12), gek, aad="chromatin:gek")
    """
    kem_ct, ss = kem_encapsulate(member_kem_pk)
    wrap_key = sha3_256(b"chromatin:gek:wrap:" + ss)
    nonce = b"\x00" * 12
    wrapped = aes_gcm_encrypt(wrap_key, nonce, gek, b"chromatin:gek")
    return kem_ct, wrapped


def unwrap_gek(kem_ct: bytes, kem_sk: bytes, wrapped_gek: bytes) -> bytes:
    """Unwrap a GEK using ML-KEM-1024 decapsulation.
    Returns the 32-byte GEK.
    """
    ss = kem_decapsulate(kem_ct, kem_sk)
    wrap_key = sha3_256(b"chromatin:gek:wrap:" + ss)
    nonce = b"\x00" * 12
    return aes_gcm_decrypt(wrap_key, nonce, wrapped_gek, b"chromatin:gek")


def encrypt_group_message(
    sender_sk: bytes, sender_fp: bytes, group_id: bytes, gek_version: int,
    gek: bytes, content: bytes
) -> bytes:
    """Encrypt a group message blob (sign-then-encrypt with GEK).
    Returns: nonce(12) || aes_ciphertext || tag(16)
    """
    sig_data = b"chromatin:grp:" + sender_fp + group_id + struct.pack(">I", gek_version) + content
    sig = sign(sender_sk, sig_data)
    plaintext = sender_fp + content + struct.pack(">H", len(sig)) + sig
    message_key = sha3_256(b"chromatin:msg:grp:" + gek)
    nonce = os.urandom(12)
    # AAD: group_id(32) || gek_version(4 BE) per spec
    aad = group_id + struct.pack(">I", gek_version)
    ct_and_tag = aes_gcm_encrypt(message_key, nonce, plaintext, aad)
    return nonce + ct_and_tag


def decrypt_group_message(
    gek: bytes, group_id: bytes, gek_version: int, blob: bytes
) -> tuple[bytes, bytes]:
    """Decrypt a group message blob.
    Returns: (sender_fp, content)
    """
    nonce = blob[:12]
    ct_and_tag = blob[12:]
    message_key = sha3_256(b"chromatin:msg:grp:" + gek)
    aad = group_id + struct.pack(">I", gek_version)
    plaintext = aes_gcm_decrypt(message_key, nonce, ct_and_tag, aad)
    sender_fp = plaintext[:32]
    # ML-DSA-87 signature is fixed 4627 bytes
    sig = plaintext[-4627:]
    sig_len = struct.unpack(">H", plaintext[-(4627 + 2):-4627])[0]
    assert sig_len == 4627, f"unexpected sig_len: {sig_len}"
    content = plaintext[32:-(4627 + 2)]
    return sender_fp, content
