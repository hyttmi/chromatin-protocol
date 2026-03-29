"""Exception hierarchy for chromatindb SDK.

All SDK exceptions inherit from ChromatinError.
Hierarchy matches C++ error conditions:
  ChromatinError
  +-- CryptoError
  |   +-- SignatureError
  |   +-- DecryptionError
  |   +-- KeyDerivationError
  +-- IdentityError
  |   +-- KeyFileError
  |   +-- NamespaceError
  +-- WireError
  |   +-- DecodeError
  +-- ProtocolError (Phase 71+: HandshakeError, ConnectionError)
"""


class ChromatinError(Exception):
    """Base exception for all chromatindb SDK errors."""


class CryptoError(ChromatinError):
    """Base for cryptographic operation errors."""


class SignatureError(CryptoError):
    """ML-DSA-87 signing or verification failed."""


class DecryptionError(CryptoError):
    """AEAD decryption or authentication failed."""


class KeyDerivationError(CryptoError):
    """HKDF key derivation failed."""


class IdentityError(ChromatinError):
    """Base for identity management errors."""


class KeyFileError(IdentityError):
    """Key file missing, corrupt, or wrong size."""


class NamespaceError(IdentityError):
    """Namespace derivation or validation failed."""


class WireError(ChromatinError):
    """Base for wire format errors."""


class DecodeError(WireError):
    """FlatBuffer decode or verification failed."""


class ProtocolError(ChromatinError):
    """Base for protocol-level errors (Phase 71+: HandshakeError, ConnectionError)."""
