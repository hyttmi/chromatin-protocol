"""chromatindb -- Python SDK for chromatindb PQ-secure decentralized database."""

__version__ = "0.1.0"

from chromatindb._hkdf import hkdf_derive, hkdf_expand, hkdf_extract
from chromatindb.crypto import (
    AEAD_KEY_SIZE,
    AEAD_NONCE_SIZE,
    AEAD_TAG_SIZE,
    SHA3_256_SIZE,
    aead_decrypt,
    aead_encrypt,
    build_signing_input,
    sha3_256,
)
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
from chromatindb.identity import Identity
from chromatindb.wire import (
    TransportMsgType,
    decode_transport_message,
    encode_transport_message,
)

__all__ = [
    "AEAD_KEY_SIZE",
    "AEAD_NONCE_SIZE",
    "AEAD_TAG_SIZE",
    "SHA3_256_SIZE",
    "ChromatinError",
    "CryptoError",
    "DecodeError",
    "DecryptionError",
    "Identity",
    "IdentityError",
    "KeyDerivationError",
    "KeyFileError",
    "NamespaceError",
    "ProtocolError",
    "SignatureError",
    "TransportMsgType",
    "WireError",
    "__version__",
    "aead_decrypt",
    "aead_encrypt",
    "build_signing_input",
    "decode_transport_message",
    "encode_transport_message",
    "hkdf_derive",
    "hkdf_expand",
    "hkdf_extract",
    "sha3_256",
]
