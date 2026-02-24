import os
import sys
import json

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crypto_utils import generate_keypair, fingerprint_of
from protocol import (
    build_hello,
    build_auth,
    build_allowlist_signature_payload,
)


def test_build_hello():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    msg = build_hello(fp, msg_id=1)
    parsed = json.loads(msg)
    assert parsed["type"] == "HELLO"
    assert parsed["id"] == 1
    assert parsed["fingerprint"] == fp.hex()


def test_build_auth():
    pubkey, seckey = generate_keypair()
    nonce = os.urandom(32)
    node_fp = os.urandom(32)
    msg = build_auth(pubkey, seckey, nonce, msg_id=2, node_fingerprint=node_fp)
    parsed = json.loads(msg)
    assert parsed["type"] == "AUTH"
    assert parsed["id"] == 2
    assert parsed["pubkey"] == pubkey.hex()
    assert len(bytes.fromhex(parsed["signature"])) == 4627


def test_build_allowlist_signature_payload():
    fp = os.urandom(32)
    target_fp = os.urandom(32)
    payload = build_allowlist_signature_payload(fp, target_fp, sequence=1, action=0x01)
    assert payload[:20] == b"chromatin:allowlist:"
    assert payload[20:52] == fp
    assert payload[52] == 0x01
    assert payload[53:85] == target_fp
    assert len(payload) == 93
