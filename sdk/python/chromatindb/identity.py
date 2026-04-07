"""Client identity management for chromatindb SDK.

Manages ML-DSA-87 signing keypairs and ML-KEM-1024 encryption keypairs.
Key files use raw binary format matching the C++ node:
- .key file: 4896 bytes (ML-DSA-87 secret key)
- .pub file: 2592 bytes (ML-DSA-87 public key)
- .kem file: 3168 bytes (ML-KEM-1024 secret key)
- .kpub file: 1568 bytes (ML-KEM-1024 public key)

KEM key ring files (after rotation):
- .kem.{N} files: 3168 bytes each (versioned KEM secret keys)
- .kpub.{N} files: 1568 bytes each (versioned KEM public keys)
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

# ML-KEM-1024 key sizes (match db/crypto/kem.h)
KEM_PUBLIC_KEY_SIZE: int = 1568
KEM_SECRET_KEY_SIZE: int = 3168


class Identity:
    """ML-DSA-87 + ML-KEM-1024 client identity with namespace derivation.

    An identity owns a signing keypair (ML-DSA-87), an optional encryption
    keypair (ML-KEM-1024), and the derived namespace (SHA3-256 of signing
    pubkey). Supports generation, file I/O, signing, verification, and
    encryption recipient capabilities.

    Args are internal -- use classmethods to create instances.
    """

    def __init__(
        self,
        public_key: bytes,
        namespace: bytes,
        signer: oqs.Signature | None = None,
        kem_public_key: bytes | None = None,
        kem: oqs.KeyEncapsulation | None = None,
        *,
        kem_ring: list[tuple[int, bytes, oqs.KeyEncapsulation | None]] | None = None,
    ) -> None:
        self._public_key = public_key
        self._namespace = namespace
        self._signer = signer  # None for verify-only identities

        # Build the KEM key ring.
        # If kem_ring is provided, use it directly (load path with versioned keys).
        # Otherwise, build a single-entry ring from the legacy kem_public_key/kem args.
        if kem_ring is not None:
            self._kem_ring = kem_ring
        elif kem_public_key is not None:
            self._kem_ring: list[tuple[int, bytes, oqs.KeyEncapsulation | None]] = [
                (0, kem_public_key, kem)
            ]
        else:
            self._kem_ring = []

        # Derive convenience attrs from ring for backward compat.
        if self._kem_ring:
            self._kem_public_key = self._kem_ring[-1][1]
            self._kem = self._kem_ring[-1][2]
        else:
            self._kem_public_key = None
            self._kem = None

    @classmethod
    def generate(cls) -> Identity:
        """Generate a new ephemeral ML-DSA-87 + ML-KEM-1024 identity.

        Returns:
            Identity with fresh signing and encryption keypairs (not saved to disk).
        """
        signer = oqs.Signature("ML-DSA-87")
        public_key = bytes(signer.generate_keypair())
        namespace = sha3_256(public_key)
        kem = oqs.KeyEncapsulation("ML-KEM-1024")
        kem_public_key = bytes(kem.generate_keypair())
        return cls(public_key, namespace, signer, kem_public_key, kem)

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

        # Read and validate KEM secret key
        kem_path = key_path.with_suffix(".kem")
        kpub_path = key_path.with_suffix(".kpub")

        try:
            kem_secret = kem_path.read_bytes()
        except FileNotFoundError:
            raise KeyFileError(
                f"KEM secret key file not found: {kem_path}"
            ) from None
        except OSError as e:
            raise KeyFileError(f"Cannot read KEM secret key file: {e}") from e

        if len(kem_secret) != KEM_SECRET_KEY_SIZE:
            raise KeyFileError(
                f"Invalid KEM secret key size: expected {KEM_SECRET_KEY_SIZE}, "
                f"got {len(kem_secret)}"
            )

        # Read and validate KEM public key
        try:
            kem_public = kpub_path.read_bytes()
        except FileNotFoundError:
            raise KeyFileError(
                f"KEM public key file not found: {kpub_path}"
            ) from None
        except OSError as e:
            raise KeyFileError(f"Cannot read KEM public key file: {e}") from e

        if len(kem_public) != KEM_PUBLIC_KEY_SIZE:
            raise KeyFileError(
                f"Invalid KEM public key size: expected {KEM_PUBLIC_KEY_SIZE}, "
                f"got {len(kem_public)}"
            )

        # Reconstruct signer and KEM from secret keys
        signer = oqs.Signature("ML-DSA-87", secret_key=secret_key)
        namespace = sha3_256(public_key)

        # Discover numbered KEM files for key ring.
        parent = key_path.parent
        stem = key_path.stem
        numbered_sec = sorted(
            (
                p for p in parent.glob(f"{stem}.kem.[0-9]*")
                if p.suffix.lstrip(".").isdigit()
            ),
            key=lambda p: int(p.suffix.lstrip(".")),
        )

        if numbered_sec:
            # Build full ring from numbered files.
            kem_ring: list[tuple[int, bytes, oqs.KeyEncapsulation | None]] = []
            for sec_path in numbered_sec:
                version = int(sec_path.suffix.lstrip("."))
                pub_path = parent / f"{stem}.kpub.{version}"

                try:
                    sk_bytes = sec_path.read_bytes()
                except FileNotFoundError:
                    raise KeyFileError(
                        f"KEM secret key file not found: {sec_path}"
                    ) from None
                except OSError as e:
                    raise KeyFileError(
                        f"Cannot read KEM secret key file: {e}"
                    ) from e

                if len(sk_bytes) != KEM_SECRET_KEY_SIZE:
                    raise KeyFileError(
                        f"Invalid KEM secret key size in {sec_path}: "
                        f"expected {KEM_SECRET_KEY_SIZE}, got {len(sk_bytes)}"
                    )

                try:
                    pk_bytes = pub_path.read_bytes()
                except FileNotFoundError:
                    raise KeyFileError(
                        f"KEM public key file not found: {pub_path}"
                    ) from None
                except OSError as e:
                    raise KeyFileError(
                        f"Cannot read KEM public key file: {e}"
                    ) from e

                if len(pk_bytes) != KEM_PUBLIC_KEY_SIZE:
                    raise KeyFileError(
                        f"Invalid KEM public key size in {pub_path}: "
                        f"expected {KEM_PUBLIC_KEY_SIZE}, got {len(pk_bytes)}"
                    )

                kem_obj = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=sk_bytes)
                kem_ring.append((version, pk_bytes, kem_obj))

            return cls(public_key, namespace, signer, kem_ring=kem_ring)

        # Pre-rotation identity: no numbered files. Single-entry ring.
        kem = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=kem_secret)
        return cls(public_key, namespace, signer, kem_public, kem)

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

    @classmethod
    def from_public_keys(cls, signing_pk: bytes, kem_pk: bytes) -> Identity:
        """Create a verify + encrypt-capable identity from public keys.

        Can verify signatures and be used as an encryption recipient.
        Cannot sign or decapsulate (no secret keys).

        Args:
            signing_pk: 2592-byte ML-DSA-87 public key.
            kem_pk: 1568-byte ML-KEM-1024 public key.

        Returns:
            Identity that can verify and be encrypted-to, but not sign or decrypt.

        Raises:
            KeyFileError: If key sizes are wrong.
        """
        if len(signing_pk) != PUBLIC_KEY_SIZE:
            raise KeyFileError(
                f"Invalid signing public key size: expected {PUBLIC_KEY_SIZE}, "
                f"got {len(signing_pk)}"
            )
        if len(kem_pk) != KEM_PUBLIC_KEY_SIZE:
            raise KeyFileError(
                f"Invalid KEM public key size: expected {KEM_PUBLIC_KEY_SIZE}, "
                f"got {len(kem_pk)}"
            )
        namespace = sha3_256(signing_pk)
        return cls(signing_pk, namespace, None, kem_pk, None)

    def save(self, key_path: str | Path) -> None:
        """Save identity key files to disk (4 files: .key, .pub, .kem, .kpub).

        Args:
            key_path: Path for the .key file.

        Raises:
            KeyFileError: If identity has no secret key, no KEM keypair,
                or files cannot be written.
        """
        if self._signer is None:
            raise KeyFileError("Cannot save verify-only identity (no secret key)")
        if self._kem is None:
            raise KeyFileError("Cannot save identity without KEM keypair")

        key_path = Path(key_path)
        parent = key_path.parent
        stem = key_path.stem
        pub_path = key_path.with_suffix(".pub")
        kem_path = key_path.with_suffix(".kem")
        kpub_path = key_path.with_suffix(".kpub")

        try:
            parent.mkdir(parents=True, exist_ok=True)
            key_path.write_bytes(self._signer.export_secret_key())
            pub_path.write_bytes(self._public_key)
            # Canonical .kem/.kpub always contain the latest key.
            kem_path.write_bytes(self._kem.export_secret_key())
            kpub_path.write_bytes(self._kem_public_key)

            # Write numbered files when ring has multiple entries or version > 0.
            # D-03: pre-rotation (ring length 1, version 0) does NOT write numbered files.
            if len(self._kem_ring) > 1 or (
                self._kem_ring and self._kem_ring[-1][0] > 0
            ):
                for version, pk, kem_obj in self._kem_ring:
                    if kem_obj is not None:
                        (parent / f"{stem}.kem.{version}").write_bytes(
                            kem_obj.export_secret_key()
                        )
                        (parent / f"{stem}.kpub.{version}").write_bytes(pk)
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

    @property
    def has_kem(self) -> bool:
        """Whether this identity has a KEM public key for encryption."""
        return len(self._kem_ring) > 0

    @property
    def key_version(self) -> int:
        """Current (highest) KEM key version. 0 for fresh/pre-rotation identities."""
        if self._kem_ring:
            return self._kem_ring[-1][0]
        return 0

    @property
    def kem_public_key(self) -> bytes:
        """The 1568-byte ML-KEM-1024 public key (latest version).

        Raises:
            KeyFileError: If identity has no KEM public key.
        """
        if self._kem_public_key is None:
            raise KeyFileError("Identity has no KEM public key")
        return self._kem_public_key

    def rotate_kem(self, key_path: str | Path) -> None:
        """Generate a new ML-KEM-1024 keypair, retain old in ring, write numbered files.

        Offline-only operation (no network). After rotation, call
        directory.register() to publish the new key.

        On first rotation of a pre-rotation identity (no numbered files on disk),
        writes kem.0.* with the original key before writing kem.1.* with the new key
        (lazy migration per D-03).

        Args:
            key_path: Path to the .key file (same as save/load convention).

        Raises:
            KeyFileError: If identity cannot rotate (verify-only or no KEM).
        """
        if self._signer is None:
            raise KeyFileError("Cannot rotate KEM on verify-only identity (no secret key)")
        if self._kem is None:
            raise KeyFileError("Cannot rotate KEM on identity without KEM keypair")

        key_path = Path(key_path)
        parent = key_path.parent
        stem = key_path.stem

        current_version = self._kem_ring[-1][0]

        # Lazy migration: if first rotation and no version-0 files on disk,
        # write the current key as version 0.
        v0_sec_path = parent / f"{stem}.kem.0"
        if current_version == 0 and not v0_sec_path.exists():
            v0_pub_path = parent / f"{stem}.kpub.0"
            try:
                v0_sec_path.write_bytes(self._kem_ring[-1][2].export_secret_key())
                v0_pub_path.write_bytes(self._kem_ring[-1][1])
            except OSError as e:
                raise KeyFileError(f"Cannot write version 0 KEM files: {e}") from e

        # Generate new KEM keypair.
        new_kem = oqs.KeyEncapsulation("ML-KEM-1024")
        new_pk = bytes(new_kem.generate_keypair())
        new_version = current_version + 1

        # Write numbered files for new version.
        try:
            (parent / f"{stem}.kem.{new_version}").write_bytes(
                new_kem.export_secret_key()
            )
            (parent / f"{stem}.kpub.{new_version}").write_bytes(new_pk)
        except OSError as e:
            raise KeyFileError(
                f"Cannot write version {new_version} KEM files: {e}"
            ) from e

        # Update canonical .kem / .kpub to latest.
        try:
            key_path.with_suffix(".kem").write_bytes(new_kem.export_secret_key())
            key_path.with_suffix(".kpub").write_bytes(new_pk)
        except OSError as e:
            raise KeyFileError(f"Cannot update canonical KEM files: {e}") from e

        # Update in-memory state.
        self._kem_ring.append((new_version, new_pk, new_kem))
        self._kem_public_key = new_pk
        self._kem = new_kem

    def _build_kem_ring_map(self) -> dict[bytes, oqs.KeyEncapsulation]:
        """Build a pk_hash -> KEM object map from all ring entries with secret keys.

        Used by envelope_decrypt to find the correct decapsulation key by
        matching sha3_256(kem_public_key) against envelope stanza kem_pk_hash.

        Returns:
            Dict mapping sha3_256(pk) -> KEM object for entries with secret keys.
            Empty dict for public-only identities.
        """
        return {
            sha3_256(pk): kem_obj
            for _, pk, kem_obj in self._kem_ring
            if kem_obj is not None
        }
