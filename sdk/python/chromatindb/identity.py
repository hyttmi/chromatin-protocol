"""Client identity management for chromatindb SDK.

Manages ML-DSA-87 keypairs for client authentication.
Key files use raw binary format matching the C++ node:
- .key file: 4896 bytes (ML-DSA-87 secret key)
- .pub file: 2592 bytes (ML-DSA-87 public key)
"""

from __future__ import annotations

from pathlib import Path

import oqs

from chromatindb.crypto import sha3_256
from chromatindb.exceptions import KeyFileError, SignatureError

# ML-DSA-87 key sizes (match db/crypto/signing.h)
PUBLIC_KEY_SIZE: int = 2592
SECRET_KEY_SIZE: int = 4896
MAX_SIGNATURE_SIZE: int = 4627


class Identity:
    """ML-DSA-87 client identity with namespace derivation.

    An identity owns a keypair and the derived namespace (SHA3-256 of pubkey).
    Supports generation, file I/O, signing, and verification.

    Args are internal -- use classmethods to create instances.
    """

    def __init__(
        self,
        public_key: bytes,
        namespace: bytes,
        signer: oqs.Signature | None = None,
    ) -> None:
        self._public_key = public_key
        self._namespace = namespace
        self._signer = signer  # None for verify-only identities

    @classmethod
    def generate(cls) -> Identity:
        """Generate a new ephemeral ML-DSA-87 identity.

        Returns:
            Identity with fresh keypair (not saved to disk).
        """
        signer = oqs.Signature("ML-DSA-87")
        public_key = signer.generate_keypair()
        # public_key is returned as bytes by liboqs-python
        public_key = bytes(public_key)
        namespace = sha3_256(public_key)
        return cls(public_key, namespace, signer)

    @classmethod
    def generate_and_save(cls, key_path: str | Path) -> Identity:
        """Generate a new identity and save to disk.

        Args:
            key_path: Path for the .key file. The .pub sibling
                is derived automatically (per D-02 SSH-style convention).

        Returns:
            Identity with saved keypair.

        Raises:
            KeyFileError: If files cannot be written.
        """
        identity = cls.generate()
        identity.save(key_path)
        return identity

    @classmethod
    def load(cls, key_path: str | Path) -> Identity:
        """Load identity from key files on disk.

        Args:
            key_path: Path to the .key file. The .pub sibling
                is derived automatically.

        Returns:
            Identity reconstructed from saved keys.

        Raises:
            KeyFileError: If files are missing, unreadable, or wrong size.
        """
        key_path = Path(key_path)
        pub_path = key_path.with_suffix(".pub")

        # Read and validate secret key
        try:
            secret_key = key_path.read_bytes()
        except FileNotFoundError:
            raise KeyFileError(f"Secret key file not found: {key_path}") from None
        except OSError as e:
            raise KeyFileError(f"Cannot read secret key file: {e}") from e

        if len(secret_key) != SECRET_KEY_SIZE:
            raise KeyFileError(
                f"Invalid secret key size: expected {SECRET_KEY_SIZE}, "
                f"got {len(secret_key)}"
            )

        # Read and validate public key
        try:
            public_key = pub_path.read_bytes()
        except FileNotFoundError:
            raise KeyFileError(f"Public key file not found: {pub_path}") from None
        except OSError as e:
            raise KeyFileError(f"Cannot read public key file: {e}") from e

        if len(public_key) != PUBLIC_KEY_SIZE:
            raise KeyFileError(
                f"Invalid public key size: expected {PUBLIC_KEY_SIZE}, "
                f"got {len(public_key)}"
            )

        # Reconstruct signer from secret key
        signer = oqs.Signature("ML-DSA-87", secret_key=secret_key)
        namespace = sha3_256(public_key)
        return cls(public_key, namespace, signer)

    @classmethod
    def from_public_key(cls, public_key: bytes) -> Identity:
        """Create a verify-only identity from a public key.

        Args:
            public_key: 2592-byte ML-DSA-87 public key.

        Returns:
            Identity that can verify but not sign.

        Raises:
            KeyFileError: If public key size is wrong.
        """
        if len(public_key) != PUBLIC_KEY_SIZE:
            raise KeyFileError(
                f"Invalid public key size: expected {PUBLIC_KEY_SIZE}, "
                f"got {len(public_key)}"
            )
        namespace = sha3_256(public_key)
        return cls(public_key, namespace, None)

    def save(self, key_path: str | Path) -> None:
        """Save identity key files to disk.

        Args:
            key_path: Path for the .key file.

        Raises:
            KeyFileError: If identity has no secret key or files cannot be written.
        """
        if self._signer is None:
            raise KeyFileError("Cannot save verify-only identity (no secret key)")

        key_path = Path(key_path)
        pub_path = key_path.with_suffix(".pub")

        try:
            key_path.parent.mkdir(parents=True, exist_ok=True)
            key_path.write_bytes(self._signer.export_secret_key())
            pub_path.write_bytes(self._public_key)
        except OSError as e:
            raise KeyFileError(f"Cannot write key files: {e}") from e

    def sign(self, message: bytes) -> bytes:
        """Sign a message with ML-DSA-87.

        Args:
            message: Data to sign (typically a 32-byte signing digest).

        Returns:
            Signature bytes (variable length, up to 4627 bytes).

        Raises:
            SignatureError: If identity has no secret key.
        """
        if self._signer is None:
            raise SignatureError("Cannot sign with verify-only identity")
        return bytes(self._signer.sign(message))

    @staticmethod
    def verify(message: bytes, signature: bytes, public_key: bytes) -> bool:
        """Verify an ML-DSA-87 signature.

        Args:
            message: Original signed data.
            signature: Signature to verify.
            public_key: 2592-byte signer's public key.

        Returns:
            True if signature is valid, False otherwise.
        """
        verifier = oqs.Signature("ML-DSA-87")
        result: bool = verifier.verify(message, signature, public_key)
        return result

    @property
    def public_key(self) -> bytes:
        """The 2592-byte ML-DSA-87 public key."""
        return self._public_key

    @property
    def namespace(self) -> bytes:
        """The 32-byte namespace ID (SHA3-256 of public key)."""
        return self._namespace

    @property
    def can_sign(self) -> bool:
        """Whether this identity can sign (has secret key)."""
        return self._signer is not None
