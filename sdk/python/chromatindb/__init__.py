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
    "ChromatinError",
    "CryptoError",
    "DecodeError",
    "DecryptionError",
    "IdentityError",
    "KeyDerivationError",
    "KeyFileError",
    "NamespaceError",
    "ProtocolError",
    "SignatureError",
    "WireError",
    "__version__",
]
