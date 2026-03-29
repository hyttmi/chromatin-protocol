"""chromatindb -- Python SDK for chromatindb PQ-secure decentralized database."""

__version__ = "0.1.0"

from chromatindb.exceptions import (
    ChromatinError,
    CryptoError,
    DecodeError,
    DecryptionError,
    IdentityError,
    KeyDerivationError,
    KeyFileError,
    NamespaceError,
    ProtocolError,
    SignatureError,
    WireError,
)

__all__ = [
    "__version__",
    "ChromatinError",
    "CryptoError",
    "SignatureError",
    "DecryptionError",
    "KeyDerivationError",
    "IdentityError",
    "KeyFileError",
    "NamespaceError",
    "WireError",
    "DecodeError",
    "ProtocolError",
]
