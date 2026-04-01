"""Exception hierarchy for chromatindb SDK.

All SDK exceptions inherit from ChromatinError.
Hierarchy matches C++ error conditions:
  ChromatinError
  +-- CryptoError
  |   +-- SignatureError
  |   +-- DecryptionError
  |   +-- NotARecipientError
  |   +-- MalformedEnvelopeError
  |   +-- KeyDerivationError
  +-- IdentityError
  |   +-- KeyFileError
  |   +-- NamespaceError
  +-- WireError
  |   +-- DecodeError
  +-- ProtocolError
      +-- HandshakeError
      +-- ConnectionError
"""


class ChromatinError(Exception):
    """Base exception for all chromatindb SDK errors."""


class CryptoError(ChromatinError):
    """Base for cryptographic operation errors."""


class SignatureError(CryptoError):
    """ML-DSA-87 signing or verification failed."""


class DecryptionError(CryptoError):
    """AEAD decryption or authentication failed."""


class NotARecipientError(CryptoError):
    """Identity not found in envelope recipient list."""


class MalformedEnvelopeError(CryptoError):
    """Envelope has invalid magic, version, or is truncated."""


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
    """Base for protocol-level errors."""


class HandshakeError(ProtocolError):
    """PQ handshake failed (timeout, auth verification, protocol mismatch)."""


class ConnectionError(ProtocolError):
    """Connection lost or transport-level failure.

    Named ConnectionError to match C++ node convention. Does NOT shadow
    the builtin ConnectionError because SDK code uses fully-qualified
    chromatindb.exceptions.ConnectionError or imports explicitly.
    """
