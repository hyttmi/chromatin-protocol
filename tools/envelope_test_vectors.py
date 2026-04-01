#!/usr/bin/env python3
"""Generate cross-SDK test vectors for envelope encryption.

Output: sdk/python/tests/vectors/envelope_vectors.json
Run: python3 tools/envelope_test_vectors.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# Adjust path so we can import the SDK from the source tree
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "sdk" / "python"))

import oqs
from chromatindb.identity import Identity
from chromatindb._envelope import envelope_encrypt, envelope_decrypt, envelope_parse


def _make_vector(
    description: str,
    plaintext: bytes,
    sender: Identity,
    recipients: list[Identity] | None = None,
) -> dict:
    """Generate one test vector with full key material and envelope."""
    if recipients is None:
        recipients = []

    envelope = envelope_encrypt(plaintext, recipients, sender)

    # Verify sender can decrypt before serializing
    assert envelope_decrypt(envelope, sender) == plaintext, (
        f"Self-check failed for vector: {description}"
    )

    meta = envelope_parse(envelope)

    vector: dict = {
        "description": description,
        "plaintext_hex": plaintext.hex(),
        "sender_kem_secret_hex": sender._kem.export_secret_key().hex(),
        "sender_kem_public_hex": sender.kem_public_key.hex(),
        "sender_signing_public_hex": sender.public_key.hex(),
        "envelope_hex": envelope.hex(),
        "expected_version": meta["version"],
        "expected_suite": meta["suite"],
        "expected_recipient_count": meta["recipient_count"],
    }

    # Add recipient keys if present
    for i, r in enumerate(recipients):
        prefix = f"recipient{i}" if len(recipients) > 1 else "recipient"
        vector[f"{prefix}_kem_secret_hex"] = r._kem.export_secret_key().hex()
        vector[f"{prefix}_kem_public_hex"] = r.kem_public_key.hex()
        vector[f"{prefix}_signing_public_hex"] = r.public_key.hex()

        # Verify recipient can also decrypt
        assert envelope_decrypt(envelope, r) == plaintext, (
            f"Self-check failed for {prefix} in vector: {description}"
        )

    return vector


def main() -> None:
    vectors: list[dict] = []

    # Vector 1: Single-recipient (sender only)
    sender1 = Identity.generate()
    vectors.append(_make_vector(
        description="single-recipient (sender only)",
        plaintext=b"hello chromatindb envelope",
        sender=sender1,
    ))

    # Vector 2: Two-recipient (sender + one recipient)
    sender2 = Identity.generate()
    recipient2 = Identity.generate()
    vectors.append(_make_vector(
        description="two-recipient (sender + one recipient)",
        plaintext=b"multi-recipient envelope test data",
        sender=sender2,
        recipients=[recipient2],
    ))

    # Vector 3: Empty plaintext
    sender3 = Identity.generate()
    vectors.append(_make_vector(
        description="empty plaintext single-recipient",
        plaintext=b"",
        sender=sender3,
    ))

    output_path = (
        Path(__file__).resolve().parent.parent
        / "sdk"
        / "python"
        / "tests"
        / "vectors"
        / "envelope_vectors.json"
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        json.dump({"vectors": vectors}, f, indent=2)
        f.write("\n")

    print(f"Wrote {len(vectors)} vectors to {output_path}")


if __name__ == "__main__":
    main()
