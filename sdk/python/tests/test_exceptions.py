"""Tests for chromatindb exception hierarchy and FlatBuffers generated code.

Validates:
- All 11 exception classes are importable
- Inheritance chain matches D-13 specification exactly
- Exceptions carry messages correctly
- catch-all ChromatinError works for all SDK exceptions
- FlatBuffers TransportMsgType enum values match transport.fbs
- FlatBuffers Blob table is importable
"""

from __future__ import annotations

import pytest

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


class TestExceptionHierarchy:
    """Verify the exception class inheritance tree matches D-13."""

    def test_chromatin_error_is_base(self) -> None:
        """ChromatinError inherits from Exception."""
        assert issubclass(ChromatinError, Exception)

    def test_crypto_hierarchy(self) -> None:
        """CryptoError -> ChromatinError, with three children."""
        assert issubclass(CryptoError, ChromatinError)
        assert issubclass(SignatureError, CryptoError)
        assert issubclass(DecryptionError, CryptoError)
        assert issubclass(KeyDerivationError, CryptoError)

    def test_identity_hierarchy(self) -> None:
        """IdentityError -> ChromatinError, with two children."""
        assert issubclass(IdentityError, ChromatinError)
        assert issubclass(KeyFileError, IdentityError)
        assert issubclass(NamespaceError, IdentityError)

    def test_wire_hierarchy(self) -> None:
        """WireError -> ChromatinError, with one child."""
        assert issubclass(WireError, ChromatinError)
        assert issubclass(DecodeError, WireError)

    def test_protocol_error(self) -> None:
        """ProtocolError -> ChromatinError (leaf in Phase 70)."""
        assert issubclass(ProtocolError, ChromatinError)
        # ProtocolError should NOT be a subclass of CryptoError, etc.
        assert not issubclass(ProtocolError, CryptoError)
        assert not issubclass(ProtocolError, IdentityError)
        assert not issubclass(ProtocolError, WireError)

    def test_all_are_chromatin_errors(self) -> None:
        """Every SDK exception is a ChromatinError (transitive)."""
        all_exceptions = [
            CryptoError,
            SignatureError,
            DecryptionError,
            KeyDerivationError,
            IdentityError,
            KeyFileError,
            NamespaceError,
            WireError,
            DecodeError,
            ProtocolError,
        ]
        for exc_class in all_exceptions:
            assert issubclass(exc_class, ChromatinError), (
                f"{exc_class.__name__} is not a ChromatinError subclass"
            )


class TestExceptionBehavior:
    """Verify exceptions carry messages and are catchable."""

    def test_message_preserved(self) -> None:
        """Exception message string is preserved."""
        msg = "ML-DSA-87 verification failed: bad signature"
        exc = SignatureError(msg)
        assert str(exc) == msg

    def test_catch_all(self) -> None:
        """except ChromatinError catches all SDK exceptions."""
        all_exceptions = [
            ChromatinError("base"),
            CryptoError("crypto"),
            SignatureError("sig"),
            DecryptionError("decrypt"),
            KeyDerivationError("kdf"),
            IdentityError("identity"),
            KeyFileError("keyfile"),
            NamespaceError("namespace"),
            WireError("wire"),
            DecodeError("decode"),
            ProtocolError("protocol"),
        ]
        for exc in all_exceptions:
            with pytest.raises(ChromatinError):
                raise exc

    def test_crypto_catch(self) -> None:
        """except CryptoError catches SignatureError, DecryptionError, KeyDerivationError."""
        for exc_class in [SignatureError, DecryptionError, KeyDerivationError]:
            with pytest.raises(CryptoError):
                raise exc_class(f"test {exc_class.__name__}")

    def test_identity_catch(self) -> None:
        """except IdentityError catches KeyFileError, NamespaceError."""
        for exc_class in [KeyFileError, NamespaceError]:
            with pytest.raises(IdentityError):
                raise exc_class(f"test {exc_class.__name__}")

    def test_wire_catch(self) -> None:
        """except WireError catches DecodeError."""
        with pytest.raises(WireError):
            raise DecodeError("bad flatbuffer")


class TestFlatBuffersImport:
    """Verify FlatBuffers generated code is importable and correct."""

    def test_transport_msg_type_enum(self) -> None:
        """TransportMsgType enum values match transport.fbs schema."""
        from chromatindb.generated.transport_generated import TransportMsgType

        assert TransportMsgType.Data == 8
        assert TransportMsgType.WriteAck == 30
        assert TransportMsgType.ReadRequest == 31
        assert TransportMsgType.Ping == 5
        assert TransportMsgType.Goodbye == 7
        assert TransportMsgType.Subscribe == 19
        assert TransportMsgType.TimeRangeResponse == 58

    def test_transport_message_class(self) -> None:
        """TransportMessage class is importable."""
        from chromatindb.generated.transport_generated import TransportMessage

        assert hasattr(TransportMessage, "GetRootAs")

    def test_blob_class(self) -> None:
        """Blob class is importable and has expected fields."""
        from chromatindb.generated.blob_generated import Blob

        assert hasattr(Blob, "GetRootAs")
        assert hasattr(Blob, "Init")

    def test_blob_builder_functions(self) -> None:
        """Blob builder functions are importable."""
        from chromatindb.generated.blob_generated import (
            BlobAddData,
            BlobAddNamespaceId,
            BlobAddPubkey,
            BlobAddSignature,
            BlobAddTimestamp,
            BlobAddTtl,
            BlobEnd,
            BlobStart,
        )

        # Just verify they exist and are callable
        assert callable(BlobStart)
        assert callable(BlobEnd)
        assert callable(BlobAddNamespaceId)
        assert callable(BlobAddPubkey)
        assert callable(BlobAddData)
        assert callable(BlobAddTtl)
        assert callable(BlobAddTimestamp)
        assert callable(BlobAddSignature)


class TestPackageImport:
    """Verify the top-level package is importable and exports correctly."""

    def test_version(self) -> None:
        """Package version is set."""
        import chromatindb

        assert chromatindb.__version__ == "0.1.0"

    def test_exception_reexports(self) -> None:
        """All exceptions are re-exported from chromatindb namespace."""
        import chromatindb

        assert chromatindb.ChromatinError is ChromatinError
        assert chromatindb.CryptoError is CryptoError
        assert chromatindb.SignatureError is SignatureError
        assert chromatindb.DecryptionError is DecryptionError
        assert chromatindb.KeyDerivationError is KeyDerivationError
        assert chromatindb.IdentityError is IdentityError
        assert chromatindb.KeyFileError is KeyFileError
        assert chromatindb.NamespaceError is NamespaceError
        assert chromatindb.WireError is WireError
        assert chromatindb.DecodeError is DecodeError
        assert chromatindb.ProtocolError is ProtocolError
