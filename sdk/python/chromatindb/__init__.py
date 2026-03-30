"""chromatindb -- Python SDK for chromatindb PQ-secure decentralized database."""

__version__ = "0.1.0"

from chromatindb._hkdf import hkdf_derive, hkdf_expand, hkdf_extract
from chromatindb.client import ChromatinClient
from chromatindb.types import (
    BlobRef,
    DeleteResult,
    ListPage,
    ReadResult,
    WriteResult,
)
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
    ConnectionError,
    CryptoError,
    DecodeError,
    DecryptionError,
    HandshakeError,
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
    "BlobRef",
    "SHA3_256_SIZE",
    "ChromatinClient",
    "ChromatinError",
    "ConnectionError",
    "CryptoError",
    "DecodeError",
    "DeleteResult",
    "DecryptionError",
    "HandshakeError",
    "Identity",
    "IdentityError",
    "KeyDerivationError",
    "KeyFileError",
    "ListPage",
    "NamespaceError",
    "ProtocolError",
    "ReadResult",
    "SignatureError",
    "TransportMsgType",
    "WireError",
    "WriteResult",
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
