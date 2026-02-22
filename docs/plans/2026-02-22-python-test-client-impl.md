# Python Test Client — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build an interactive REPL client in Python for testing chromatin-node deployments.

**Architecture:** Async Python (`asyncio`) with `websockets` for WS transport and `liboqs-python` for ML-DSA-87 signatures. Three modules: crypto utilities, protocol layer, and REPL. Push notifications handled by a background listener task that prints inline.

**Tech Stack:** Python 3.10+, websockets, liboqs-python, hashlib (SHA3-256), asyncio

**Environment:** venv at `.venv/`, activate with `source .venv/bin/activate`. Deps already installed: `websockets`, `liboqs-python`.

**Test nodes:** 195.181.202.122 ports 62000-62002 (WS). Run client from dev machine.

---

### Task 1: Crypto Utilities

**Files:**
- Create: `tools/client/crypto_utils.py`
- Create: `tools/client/tests/test_crypto_utils.py`

**Step 1: Write tests for keygen, fingerprint, signing, and PoW**

```python
# tools/client/tests/test_crypto_utils.py
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crypto_utils import (
    generate_keypair,
    load_keypair,
    save_keypair,
    fingerprint_of,
    sign,
    mine_pow,
    count_leading_zero_bits,
)


def test_generate_keypair():
    pubkey, seckey = generate_keypair()
    assert len(pubkey) == 2592  # ML-DSA-87 public key
    assert len(seckey) == 4896  # ML-DSA-87 secret key


def test_fingerprint():
    pubkey, _ = generate_keypair()
    fp = fingerprint_of(pubkey)
    assert len(fp) == 32
    # Deterministic
    assert fingerprint_of(pubkey) == fp


def test_sign_and_verify():
    pubkey, seckey = sign_keypair = generate_keypair()
    message = b"chromatin-auth:" + os.urandom(32)
    sig = sign(seckey, message)
    assert len(sig) == 4627  # ML-DSA-87 signature


def test_save_and_load_keypair():
    pubkey, seckey = generate_keypair()
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "test.key")
        save_keypair(path, pubkey, seckey)
        loaded_pub, loaded_sec = load_keypair(path)
        assert loaded_pub == pubkey
        assert loaded_sec == seckey


def test_count_leading_zero_bits():
    assert count_leading_zero_bits(b"\x00\x00\x00\xff") == 24
    assert count_leading_zero_bits(b"\x00\x00\xff\xff") == 16
    assert count_leading_zero_bits(b"\x00\x01\xff\xff") == 15
    assert count_leading_zero_bits(b"\xff\xff\xff\xff") == 0
    assert count_leading_zero_bits(b"\x00\x00\x00\x00") == 32


def test_mine_pow():
    # Mine with very low difficulty (8 bits) for speed
    prefix = b"test-prefix"
    nonce = mine_pow(prefix, difficulty=8)
    import hashlib
    h = hashlib.sha3_256(prefix + nonce.to_bytes(8, "big")).digest()
    assert count_leading_zero_bits(h) >= 8
```

**Step 2: Run tests to verify they fail**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/test_crypto_utils.py -v`
Expected: FAIL with ModuleNotFoundError

**Step 3: Implement crypto_utils.py**

```python
# tools/client/crypto_utils.py
"""ML-DSA-87 key management, SHA3-256 hashing, and proof-of-work mining."""

import hashlib
import os
import struct

import oqs


def generate_keypair() -> tuple[bytes, bytes]:
    """Generate an ML-DSA-87 keypair. Returns (pubkey, seckey)."""
    signer = oqs.Signature("ML-DSA-87")
    pubkey = signer.generate_keypair()
    seckey = signer.export_secret_key()
    return bytes(pubkey), bytes(seckey)


def fingerprint_of(pubkey: bytes) -> bytes:
    """SHA3-256 hash of a public key."""
    return hashlib.sha3_256(pubkey).digest()


def sign(seckey: bytes, message: bytes) -> bytes:
    """Sign a message with ML-DSA-87."""
    signer = oqs.Signature("ML-DSA-87", secret_key=seckey)
    return bytes(signer.sign(message))


def save_keypair(path: str, pubkey: bytes, seckey: bytes) -> None:
    """Save keypair to file. Format: pubkey_len(4 BE) || pubkey || seckey."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack(">I", len(pubkey)))
        f.write(pubkey)
        f.write(seckey)


def load_keypair(path: str) -> tuple[bytes, bytes]:
    """Load keypair from file."""
    with open(path, "rb") as f:
        data = f.read()
    pubkey_len = struct.unpack(">I", data[:4])[0]
    pubkey = data[4 : 4 + pubkey_len]
    seckey = data[4 + pubkey_len :]
    return pubkey, seckey


def count_leading_zero_bits(data: bytes) -> int:
    """Count leading zero bits in a byte string."""
    bits = 0
    for byte in data:
        if byte == 0:
            bits += 8
        else:
            # Count leading zeros in this byte
            for i in range(7, -1, -1):
                if byte & (1 << i):
                    return bits
                bits += 1
            return bits
    return bits


def mine_pow(prefix: bytes, difficulty: int) -> int:
    """Mine a proof-of-work nonce. Returns nonce such that
    SHA3-256(prefix || nonce_BE(8)) has >= difficulty leading zero bits."""
    nonce = 0
    while True:
        h = hashlib.sha3_256(prefix + nonce.to_bytes(8, "big")).digest()
        if count_leading_zero_bits(h) >= difficulty:
            return nonce
        nonce += 1
```

**Step 4: Run tests to verify they pass**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/test_crypto_utils.py -v`
Expected: 6 passed

**Step 5: Commit**

```bash
git add tools/client/crypto_utils.py tools/client/tests/test_crypto_utils.py
git commit -m "feat(client): add crypto utilities (keygen, signing, PoW)"
```

---

### Task 2: Protocol Layer — Connection and Auth

**Files:**
- Create: `tools/client/protocol.py`
- Create: `tools/client/tests/test_protocol.py`

**Step 1: Write tests for auth handshake message building**

```python
# tools/client/tests/test_protocol.py
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
    msg = build_auth(pubkey, seckey, nonce, msg_id=2)
    parsed = json.loads(msg)
    assert parsed["type"] == "AUTH"
    assert parsed["id"] == 2
    assert parsed["pubkey"] == pubkey.hex()
    assert len(bytes.fromhex(parsed["signature"])) == 4627


def test_build_allowlist_signature_payload():
    fp = os.urandom(32)
    target_fp = os.urandom(32)
    # ALLOW action = 0x01
    payload = build_allowlist_signature_payload(fp, target_fp, sequence=1, action=0x01)
    assert payload[:20] == b"chromatin:allowlist:"
    assert payload[20:52] == fp
    assert payload[52] == 0x01
    assert payload[53:85] == target_fp
    assert len(payload) == 93  # 20 + 32 + 1 + 32 + 8
```

**Step 2: Run tests to verify they fail**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/test_protocol.py -v`
Expected: FAIL with ImportError

**Step 3: Implement protocol.py (connection + auth + message builders)**

```python
# tools/client/protocol.py
"""WebSocket protocol layer for Chromatin client."""

import asyncio
import base64
import json
import struct
import time
from typing import Optional, Callable, Awaitable

import websockets

from crypto_utils import fingerprint_of, sign


class ChromatinClient:
    """Async WebSocket client for a Chromatin node."""

    def __init__(self, pubkey: bytes, seckey: bytes):
        self.pubkey = pubkey
        self.seckey = seckey
        self.fingerprint = fingerprint_of(pubkey)
        self.ws: Optional[websockets.ClientConnection] = None
        self._next_id = 1
        self._pending: dict[int, asyncio.Future] = {}
        self._push_callback: Optional[Callable[[dict], Awaitable[None]]] = None
        self._listener_task: Optional[asyncio.Task] = None
        self._allowlist_seq = 0

    def _msg_id(self) -> int:
        mid = self._next_id
        self._next_id += 1
        return mid

    async def connect(self, host: str, port: int) -> dict:
        """Connect, perform HELLO + AUTH handshake. Returns OK response."""
        uri = f"ws://{host}:{port}/ws"
        self.ws = await websockets.connect(uri, max_size=2**22)

        # HELLO
        hello_id = self._msg_id()
        await self.ws.send(build_hello(self.fingerprint, hello_id))
        resp = json.loads(await self.ws.recv())

        if resp.get("type") == "REDIRECT":
            await self.ws.close()
            self.ws = None
            return resp

        if resp.get("type") == "ERROR":
            await self.ws.close()
            self.ws = None
            return resp

        if resp.get("type") != "CHALLENGE":
            raise RuntimeError(f"Expected CHALLENGE, got {resp.get('type')}")

        nonce = bytes.fromhex(resp["nonce"])

        # AUTH
        auth_id = self._msg_id()
        await self.ws.send(build_auth(self.pubkey, self.seckey, nonce, auth_id))
        resp = json.loads(await self.ws.recv())

        if resp.get("type") == "ERROR":
            await self.ws.close()
            self.ws = None
            return resp

        # Start background listener
        self._listener_task = asyncio.create_task(self._listen())
        return resp

    async def disconnect(self):
        """Close the WebSocket connection."""
        if self._listener_task:
            self._listener_task.cancel()
            try:
                await self._listener_task
            except asyncio.CancelledError:
                pass
        if self.ws:
            await self.ws.close()
            self.ws = None

    def set_push_callback(self, callback: Callable[[dict], Awaitable[None]]):
        """Set callback for push notifications."""
        self._push_callback = callback

    async def _listen(self):
        """Background task: receive messages, route to pending futures or push callback."""
        try:
            async for raw in self.ws:
                if isinstance(raw, bytes):
                    # Binary frame (chunked download) — skip for now
                    continue
                msg = json.loads(raw)
                msg_id = msg.get("id")
                if msg_id is not None and msg_id in self._pending:
                    self._pending.pop(msg_id).set_result(msg)
                elif self._push_callback:
                    await self._push_callback(msg)
        except websockets.ConnectionClosed:
            pass
        except asyncio.CancelledError:
            raise

    async def send_command(self, cmd: dict, timeout: float = 10.0) -> dict:
        """Send a JSON command and wait for the response by ID."""
        msg_id = cmd["id"]
        future = asyncio.get_event_loop().create_future()
        self._pending[msg_id] = future
        await self.ws.send(json.dumps(cmd))
        try:
            return await asyncio.wait_for(future, timeout)
        except asyncio.TimeoutError:
            self._pending.pop(msg_id, None)
            return {"type": "ERROR", "id": msg_id, "code": 408, "reason": "timeout"}

    # --- Command builders ---

    async def cmd_status(self) -> dict:
        mid = self._msg_id()
        return await self.send_command({"type": "STATUS", "id": mid})

    async def cmd_send(self, to_fp: str, blob: bytes) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "SEND", "id": mid,
            "to": to_fp,
            "blob": base64.b64encode(blob).decode(),
        })

    async def cmd_list(self, limit: int = 50, after: str = "") -> dict:
        mid = self._msg_id()
        cmd = {"type": "LIST", "id": mid, "limit": limit}
        if after:
            cmd["after"] = after
        return await self.send_command(cmd)

    async def cmd_get(self, msg_id: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({"type": "GET", "id": mid, "msg_id": msg_id})

    async def cmd_delete(self, msg_ids: list[str]) -> dict:
        mid = self._msg_id()
        return await self.send_command({"type": "DELETE", "id": mid, "msg_ids": msg_ids})

    async def cmd_allow(self, target_fp: str) -> dict:
        self._allowlist_seq += 1
        payload = build_allowlist_signature_payload(
            self.fingerprint, bytes.fromhex(target_fp),
            self._allowlist_seq, action=0x01,
        )
        sig = sign(self.seckey, payload)
        mid = self._msg_id()
        return await self.send_command({
            "type": "ALLOW", "id": mid,
            "fingerprint": target_fp,
            "sequence": self._allowlist_seq,
            "signature": sig.hex(),
        })

    async def cmd_revoke(self, target_fp: str) -> dict:
        self._allowlist_seq += 1
        payload = build_allowlist_signature_payload(
            self.fingerprint, bytes.fromhex(target_fp),
            self._allowlist_seq, action=0x00,
        )
        sig = sign(self.seckey, payload)
        mid = self._msg_id()
        return await self.send_command({
            "type": "REVOKE", "id": mid,
            "fingerprint": target_fp,
            "sequence": self._allowlist_seq,
            "signature": sig.hex(),
        })

    async def cmd_contact_request(self, to_fp: str, blob: bytes,
                                   pow_nonce: int, timestamp: int) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "CONTACT_REQUEST", "id": mid,
            "to": to_fp,
            "blob": base64.b64encode(blob).decode(),
            "pow_nonce": pow_nonce,
            "timestamp": timestamp,
        })

    async def cmd_list_requests(self) -> dict:
        mid = self._msg_id()
        return await self.send_command({"type": "LIST_REQUESTS", "id": mid})

    async def cmd_resolve_name(self, name: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({"type": "RESOLVE_NAME", "id": mid, "name": name})

    async def cmd_get_profile(self, fp: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({"type": "GET_PROFILE", "id": mid, "fingerprint": fp})

    async def cmd_set_profile(self, profile_binary: bytes) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "SET_PROFILE", "id": mid,
            "profile": base64.b64encode(profile_binary).decode(),
        })

    async def cmd_register_name(self, name_record: bytes) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "REGISTER_NAME", "id": mid,
            "name_record": base64.b64encode(name_record).decode(),
        })

    async def cmd_group_create(self, group_meta: bytes) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_CREATE", "id": mid,
            "group_meta": group_meta.hex(),
        })

    async def cmd_group_info(self, group_id: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_INFO", "id": mid,
            "group_id": group_id,
        })

    async def cmd_group_send(self, group_id: str, msg_id: str,
                              gek_version: int, blob: bytes) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_SEND", "id": mid,
            "group_id": group_id,
            "msg_id": msg_id,
            "gek_version": gek_version,
            "blob": blob.hex(),
        })

    async def cmd_group_list(self, group_id: str, limit: int = 50,
                              after: str = "") -> dict:
        mid = self._msg_id()
        cmd = {"type": "GROUP_LIST", "id": mid, "group_id": group_id, "limit": limit}
        if after:
            cmd["after"] = after
        return await self.send_command(cmd)

    async def cmd_group_get(self, group_id: str, msg_id: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_GET", "id": mid,
            "group_id": group_id,
            "msg_id": msg_id,
        })

    async def cmd_group_delete(self, group_id: str, msg_id: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_DELETE", "id": mid,
            "group_id": group_id,
            "msg_id": msg_id,
        })

    async def cmd_group_destroy(self, group_id: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_DESTROY", "id": mid,
            "group_id": group_id,
        })


# --- Standalone message builders (for unit testing without connection) ---

def build_hello(fingerprint: bytes, msg_id: int) -> str:
    return json.dumps({
        "type": "HELLO",
        "id": msg_id,
        "fingerprint": fingerprint.hex(),
    })


def build_auth(pubkey: bytes, seckey: bytes, nonce: bytes, msg_id: int) -> str:
    message = b"chromatin-auth:" + nonce
    sig = sign(seckey, message)
    return json.dumps({
        "type": "AUTH",
        "id": msg_id,
        "signature": sig.hex(),
        "pubkey": pubkey.hex(),
    })


def build_allowlist_signature_payload(
    own_fp: bytes, target_fp: bytes, sequence: int, action: int
) -> bytes:
    """Build the payload that gets signed for ALLOW/REVOKE.
    Format: "chromatin:allowlist:" || own_fp(32) || action(1) || target_fp(32) || sequence(8 BE)
    """
    return (
        b"chromatin:allowlist:"
        + own_fp
        + bytes([action])
        + target_fp
        + struct.pack(">Q", sequence)
    )
```

**Step 4: Run tests to verify they pass**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/test_protocol.py -v`
Expected: 3 passed

**Step 5: Commit**

```bash
git add tools/client/protocol.py tools/client/tests/test_protocol.py
git commit -m "feat(client): add protocol layer (connection, auth, commands)"
```

---

### Task 3: Profile and Name Record Builders

**Files:**
- Create: `tools/client/builders.py`
- Create: `tools/client/tests/test_builders.py`

These build the binary records needed for SET_PROFILE and REGISTER_NAME.

**Step 1: Write tests**

```python
# tools/client/tests/test_builders.py
import os
import sys
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crypto_utils import generate_keypair, fingerprint_of
from builders import build_profile_record, build_name_record


def test_build_profile_record():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    record = build_profile_record(
        seckey=seckey,
        fingerprint=fp,
        pubkey=pubkey,
        kem_pubkey=b"",  # empty for now
        bio="Hello world",
        avatar=b"",
        social_links=[],
        sequence=1,
    )
    # Should start with fingerprint
    assert record[:32] == fp
    # Should be parseable: fp(32) + pubkey_len(2) + pubkey + ...
    offset = 32
    pk_len = struct.unpack(">H", record[offset : offset + 2])[0]
    assert pk_len == len(pubkey)


def test_build_name_record():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    record = build_name_record(
        seckey=seckey,
        name="alice",
        fingerprint=fp,
        pow_nonce=0,
        sequence=1,
    )
    # Should start with name_len(1) = 5
    assert record[0] == 5
    assert record[1:6] == b"alice"
    assert record[6:38] == fp
```

**Step 2: Run tests to verify they fail**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/test_builders.py -v`
Expected: FAIL with ImportError

**Step 3: Implement builders.py**

```python
# tools/client/builders.py
"""Binary record builders for SET_PROFILE and REGISTER_NAME."""

import struct

from crypto_utils import sign


def build_profile_record(
    seckey: bytes,
    fingerprint: bytes,
    pubkey: bytes,
    kem_pubkey: bytes,
    bio: str,
    avatar: bytes,
    social_links: list[tuple[str, str]],  # [(platform, handle), ...]
    sequence: int,
) -> bytes:
    """Build a signed profile binary record.

    Format:
        fingerprint(32) ||
        pubkey_len(2 BE) || pubkey ||
        kem_pubkey_len(2 BE) || kem_pubkey ||
        bio_len(2 BE) || bio(UTF-8) ||
        avatar_len(4 BE) || avatar ||
        social_links_count(1) ||
          [platform_len(1) || platform || handle_len(1) || handle] * count ||
        sequence(8 BE) ||
        sig_len(2 BE) || signature
    """
    bio_bytes = bio.encode("utf-8")
    body = bytearray()
    body += fingerprint
    body += struct.pack(">H", len(pubkey))
    body += pubkey
    body += struct.pack(">H", len(kem_pubkey))
    body += kem_pubkey
    body += struct.pack(">H", len(bio_bytes))
    body += bio_bytes
    body += struct.pack(">I", len(avatar))
    body += avatar
    body += struct.pack("B", len(social_links))
    for platform, handle in social_links:
        p = platform.encode("utf-8")
        h = handle.encode("utf-8")
        body += struct.pack("B", len(p))
        body += p
        body += struct.pack("B", len(h))
        body += h
    body += struct.pack(">Q", sequence)

    sig = sign(seckey, bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)


def build_name_record(
    seckey: bytes,
    name: str,
    fingerprint: bytes,
    pow_nonce: int,
    sequence: int,
) -> bytes:
    """Build a signed name registration record.

    Format:
        name_len(1) || name(UTF-8) ||
        fingerprint(32) ||
        pow_nonce(8 BE) ||
        sequence(8 BE) ||
        sig_len(2 BE) || signature
    """
    name_bytes = name.encode("utf-8")
    body = bytearray()
    body += struct.pack("B", len(name_bytes))
    body += name_bytes
    body += fingerprint
    body += struct.pack(">Q", pow_nonce)
    body += struct.pack(">Q", sequence)

    sig = sign(seckey, bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)


def build_group_meta(
    seckey: bytes,
    group_id: bytes,
    owner_fingerprint: bytes,
    version: int,
    members: list[tuple[bytes, int, bytes]],  # [(fp, role, kem_ciphertext), ...]
) -> bytes:
    """Build a signed GROUP_META binary record.

    Format:
        group_id(32) ||
        owner_fingerprint(32) ||
        version(4 BE) ||
        member_count(2 BE) ||
          [member_fp(32) || role(1) || kem_ciphertext(1568)] * count ||
        sig_len(2 BE) || signature
    """
    body = bytearray()
    body += group_id
    body += owner_fingerprint
    body += struct.pack(">I", version)
    body += struct.pack(">H", len(members))
    for fp, role, kem_ct in members:
        body += fp
        body += struct.pack("B", role)
        body += kem_ct  # 1568 bytes for ML-KEM-1024

    sig = sign(seckey, bytes(body))
    body += struct.pack(">H", len(sig))
    body += sig
    return bytes(body)
```

**Step 4: Run tests to verify they pass**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/test_builders.py -v`
Expected: 2 passed

**Step 5: Commit**

```bash
git add tools/client/builders.py tools/client/tests/test_builders.py
git commit -m "feat(client): add profile and name record builders"
```

---

### Task 4: REPL — Core Loop and Basic Commands

**Files:**
- Create: `tools/client/chromatin_client.py`
- Create: `tools/client/tests/__init__.py` (empty, for pytest discovery)
- Create: `tools/client/requirements.txt`

**Step 1: Create requirements.txt**

```
websockets>=13.0
liboqs-python>=0.10.0
```

**Step 2: Create empty __init__.py for test discovery**

```bash
touch tools/client/tests/__init__.py
```

**Step 3: Implement the REPL**

```python
#!/usr/bin/env python3
# tools/client/chromatin_client.py
"""Chromatin interactive test client."""

import asyncio
import base64
import hashlib
import json
import os
import sys
import time

from crypto_utils import (
    generate_keypair,
    load_keypair,
    save_keypair,
    fingerprint_of,
    mine_pow,
)
from protocol import ChromatinClient
from builders import build_profile_record, build_name_record, build_group_meta

IDENTITY_DIR = os.path.expanduser("~/.chromatin")
IDENTITY_PATH = os.path.join(IDENTITY_DIR, "identity.key")

# PoW difficulty defaults (must match node config)
CONTACT_POW_DIFFICULTY = 16
NAME_POW_DIFFICULTY = 28


def load_or_create_identity() -> tuple[bytes, bytes]:
    """Load identity from disk, or generate a new one."""
    if os.path.exists(IDENTITY_PATH):
        pubkey, seckey = load_keypair(IDENTITY_PATH)
        fp = fingerprint_of(pubkey)
        print(f"Loaded identity: {fp.hex()}")
        return pubkey, seckey

    print("No identity found. Generating ML-DSA-87 keypair...")
    pubkey, seckey = generate_keypair()
    save_keypair(IDENTITY_PATH, pubkey, seckey)
    fp = fingerprint_of(pubkey)
    print(f"New identity: {fp.hex()}")
    print(f"Saved to: {IDENTITY_PATH}")
    return pubkey, seckey


async def push_handler(msg: dict):
    """Handle push notifications from the server."""
    msg_type = msg.get("type", "UNKNOWN")
    if msg_type == "NEW_MESSAGE":
        sender = msg.get("from", "?")[:16]
        size = msg.get("size", 0)
        print(f"\n[PUSH] NEW_MESSAGE from {sender}... ({size} bytes)")
    elif msg_type == "CONTACT_REQUEST":
        sender = msg.get("from", "?")[:16]
        print(f"\n[PUSH] CONTACT_REQUEST from {sender}...")
    elif msg_type == "NEW_GROUP_MESSAGE":
        group = msg.get("group_id", "?")[:16]
        sender = msg.get("sender", "?")[:16]
        print(f"\n[PUSH] NEW_GROUP_MESSAGE in {group}... from {sender}...")
    else:
        print(f"\n[PUSH] {msg_type}: {json.dumps(msg)}")


HELP_TEXT = """
Commands:
  connect <host> <port>         Connect and authenticate
  disconnect                    Close connection
  status                        Node status
  identity                      Show own fingerprint

  send <fingerprint> <text>     Send a message
  list [limit] [after]          List inbox messages
  get <msg_id>                  Fetch a message
  delete <msg_id> [...]         Delete messages

  allow <fingerprint>           Add contact to allowlist
  revoke <fingerprint>          Remove from allowlist
  request <fingerprint> <text>  Send contact request (mines PoW)
  list_requests                 List pending contact requests

  register <name>               Register a name (mines PoW, ~minutes)
  resolve <name>                Resolve name to fingerprint
  set_profile <bio>             Set profile with bio text
  get_profile <fingerprint>     Get user profile

  group_create <fp> [fp...]     Create group with members
  group_info <group_id>         Get group metadata
  group_send <group_id> <text>  Send group message
  group_list <group_id>         List group messages
  group_get <group_id> <msg_id> Get group message
  group_delete <group_id> <mid> Delete group message
  group_destroy <group_id>      Destroy group (owner only)

  help                          Show this help
  quit / exit                   Exit
""".strip()


async def repl(client: ChromatinClient):
    """Main REPL loop."""
    print("Chromatin Test Client")
    print(f"Identity: {client.fingerprint.hex()}")
    print("Type 'help' for commands.\n")

    loop = asyncio.get_event_loop()

    while True:
        try:
            line = await loop.run_in_executor(None, lambda: input("chromatin> "))
        except (EOFError, KeyboardInterrupt):
            break

        line = line.strip()
        if not line:
            continue

        parts = line.split()
        cmd = parts[0].lower()

        try:
            if cmd in ("quit", "exit"):
                break

            elif cmd == "help":
                print(HELP_TEXT)

            elif cmd == "identity":
                print(f"Fingerprint: {client.fingerprint.hex()}")
                print(f"Public key:  {client.pubkey.hex()[:64]}...")

            elif cmd == "connect":
                if len(parts) < 3:
                    print("Usage: connect <host> <port>")
                    continue
                host, port = parts[1], int(parts[2])
                print(f"Connecting to {host}:{port}...")
                resp = await client.connect(host, port)
                resp_type = resp.get("type", "")
                if resp_type == "REDIRECT":
                    nodes = resp.get("nodes", [])
                    print("REDIRECT — not responsible. Try:")
                    for n in nodes:
                        print(f"  {n['address']}:{n['ws_port']} (seq {n.get('seq', '?')})")
                elif resp_type == "ERROR":
                    print(f"Error: {resp.get('reason', 'unknown')}")
                else:
                    pending = resp.get("pending_messages", 0)
                    print(f"Authenticated. {pending} pending messages.")

            elif cmd == "disconnect":
                await client.disconnect()
                print("Disconnected.")

            elif cmd == "status":
                resp = await client.cmd_status()
                print(json.dumps(resp, indent=2))

            elif cmd == "send":
                if len(parts) < 3:
                    print("Usage: send <fingerprint> <text>")
                    continue
                to_fp = parts[1]
                text = " ".join(parts[2:])
                resp = await client.cmd_send(to_fp, text.encode("utf-8"))
                if resp.get("type") == "ERROR":
                    print(f"Error {resp.get('code')}: {resp.get('reason')}")
                else:
                    print(f"Sent. msg_id={resp.get('msg_id', '?')}")

            elif cmd == "list":
                limit = int(parts[1]) if len(parts) > 1 else 50
                after = parts[2] if len(parts) > 2 else ""
                resp = await client.cmd_list(limit, after)
                if resp.get("type") == "ERROR":
                    print(f"Error {resp.get('code')}: {resp.get('reason')}")
                else:
                    msgs = resp.get("messages", [])
                    print(f"{len(msgs)} messages:")
                    for m in msgs:
                        blob_preview = ""
                        if m.get("blob"):
                            try:
                                raw = base64.b64decode(m["blob"])
                                blob_preview = f" — {raw[:80].decode('utf-8', errors='replace')}"
                            except Exception:
                                blob_preview = f" — ({m.get('size', '?')} bytes)"
                        print(f"  [{m['msg_id'][:16]}...] from {m['from'][:16]}... "
                              f"size={m.get('size', '?')}{blob_preview}")
                    if resp.get("has_more"):
                        print("  (more messages available)")

            elif cmd == "get":
                if len(parts) < 2:
                    print("Usage: get <msg_id>")
                    continue
                resp = await client.cmd_get(parts[1])
                if resp.get("type") == "ERROR":
                    print(f"Error {resp.get('code')}: {resp.get('reason')}")
                elif resp.get("blob"):
                    raw = base64.b64decode(resp["blob"])
                    print(f"Message ({len(raw)} bytes):")
                    print(raw.decode("utf-8", errors="replace"))
                else:
                    print(f"Large message: {resp.get('size')} bytes, {resp.get('chunks')} chunks")
                    print("(chunked download not yet supported)")

            elif cmd == "delete":
                if len(parts) < 2:
                    print("Usage: delete <msg_id> [msg_id...]")
                    continue
                resp = await client.cmd_delete(parts[1:])
                print(f"Deleted." if resp.get("type") == "OK" else f"Error: {resp}")

            elif cmd == "allow":
                if len(parts) < 2:
                    print("Usage: allow <fingerprint>")
                    continue
                resp = await client.cmd_allow(parts[1])
                print("Allowed." if resp.get("type") == "OK" else f"Error: {resp}")

            elif cmd == "revoke":
                if len(parts) < 2:
                    print("Usage: revoke <fingerprint>")
                    continue
                resp = await client.cmd_revoke(parts[1])
                print("Revoked." if resp.get("type") == "OK" else f"Error: {resp}")

            elif cmd == "request":
                if len(parts) < 3:
                    print("Usage: request <fingerprint> <text>")
                    continue
                to_fp = parts[1]
                text = " ".join(parts[2:])
                timestamp = int(time.time() * 1000)
                print(f"Mining PoW ({CONTACT_POW_DIFFICULTY} bits)...")
                prefix = (
                    b"chromatin:request:"
                    + client.fingerprint
                    + bytes.fromhex(to_fp)
                    + timestamp.to_bytes(8, "big")
                )
                nonce = mine_pow(prefix, CONTACT_POW_DIFFICULTY)
                print(f"PoW found: nonce={nonce}")
                resp = await client.cmd_contact_request(
                    to_fp, text.encode("utf-8"), nonce, timestamp
                )
                print("Sent." if resp.get("type") == "OK" else f"Error: {resp}")

            elif cmd == "list_requests":
                resp = await client.cmd_list_requests()
                if resp.get("type") == "ERROR":
                    print(f"Error {resp.get('code')}: {resp.get('reason')}")
                else:
                    reqs = resp.get("requests", [])
                    print(f"{len(reqs)} contact requests:")
                    for r in reqs:
                        blob_text = ""
                        if r.get("blob"):
                            try:
                                blob_text = base64.b64decode(r["blob"]).decode("utf-8", errors="replace")
                            except Exception:
                                pass
                        print(f"  from {r['from'][:16]}... — {blob_text}")

            elif cmd == "register":
                if len(parts) < 2:
                    print("Usage: register <name>")
                    continue
                name = parts[1]
                print(f"Mining name PoW ({NAME_POW_DIFFICULTY} bits)... this may take minutes.")
                prefix = b"chromatin:name:" + name.encode() + client.fingerprint
                nonce = mine_pow(prefix, NAME_POW_DIFFICULTY)
                print(f"PoW found: nonce={nonce}")
                record = build_name_record(
                    client.seckey, name, client.fingerprint, nonce, sequence=1
                )
                resp = await client.cmd_register_name(record)
                if resp.get("type") == "ERROR":
                    print(f"Error {resp.get('code')}: {resp.get('reason')}")
                else:
                    print(f"Name '{name}' registered.")

            elif cmd == "resolve":
                if len(parts) < 2:
                    print("Usage: resolve <name>")
                    continue
                resp = await client.cmd_resolve_name(parts[1])
                if resp.get("found"):
                    print(f"{parts[1]} => {resp['fingerprint']}")
                else:
                    print("Name not found.")

            elif cmd == "set_profile":
                if len(parts) < 2:
                    print("Usage: set_profile <bio text>")
                    continue
                bio = " ".join(parts[1:])
                record = build_profile_record(
                    seckey=client.seckey,
                    fingerprint=client.fingerprint,
                    pubkey=client.pubkey,
                    kem_pubkey=b"",
                    bio=bio,
                    avatar=b"",
                    social_links=[],
                    sequence=1,
                )
                resp = await client.cmd_set_profile(record)
                if resp.get("type") == "ERROR":
                    print(f"Error {resp.get('code')}: {resp.get('reason')}")
                else:
                    print("Profile updated.")

            elif cmd == "get_profile":
                if len(parts) < 2:
                    print("Usage: get_profile <fingerprint>")
                    continue
                resp = await client.cmd_get_profile(parts[1])
                if resp.get("type") == "ERROR":
                    print(f"Error {resp.get('code')}: {resp.get('reason')}")
                elif resp.get("found"):
                    print(json.dumps({
                        k: v for k, v in resp.items()
                        if k not in ("type", "id", "pubkey", "kem_pubkey")
                    }, indent=2))
                else:
                    print("Profile not found.")

            elif cmd == "group_create":
                if len(parts) < 2:
                    print("Usage: group_create <member_fp> [member_fp...]")
                    continue
                group_id = os.urandom(32)
                # Build member list: self as owner, others as members
                # Use dummy KEM ciphertext (1568 zero bytes) for testing
                kem_dummy = b"\x00" * 1568
                members = [(client.fingerprint, 0x02, kem_dummy)]  # self = owner
                for fp_hex in parts[1:]:
                    members.append((bytes.fromhex(fp_hex), 0x00, kem_dummy))
                meta = build_group_meta(
                    client.seckey, group_id, client.fingerprint, 1, members
                )
                resp = await client.cmd_group_create(meta)
                if resp.get("ok"):
                    print(f"Group created: {resp.get('group_id', group_id.hex())}")
                else:
                    print(f"Error: {resp}")

            elif cmd == "group_info":
                if len(parts) < 2:
                    print("Usage: group_info <group_id>")
                    continue
                resp = await client.cmd_group_info(parts[1])
                if resp.get("ok"):
                    print(f"Group meta (hex): {resp.get('group_meta', '')[:128]}...")
                else:
                    print(f"Error: {resp}")

            elif cmd == "group_send":
                if len(parts) < 3:
                    print("Usage: group_send <group_id> <text>")
                    continue
                group_id = parts[1]
                text = " ".join(parts[2:])
                msg_id = os.urandom(32).hex()
                resp = await client.cmd_group_send(
                    group_id, msg_id, gek_version=1, blob=text.encode("utf-8")
                )
                if resp.get("ok"):
                    print(f"Sent. msg_id={resp.get('msg_id', msg_id)}")
                else:
                    print(f"Error: {resp}")

            elif cmd == "group_list":
                if len(parts) < 2:
                    print("Usage: group_list <group_id>")
                    continue
                resp = await client.cmd_group_list(parts[1])
                if resp.get("ok"):
                    msgs = resp.get("messages", [])
                    print(f"{len(msgs)} group messages:")
                    for m in msgs:
                        blob_text = ""
                        if m.get("blob"):
                            try:
                                blob_text = bytes.fromhex(m["blob"]).decode("utf-8", errors="replace")
                                blob_text = f" — {blob_text[:80]}"
                            except Exception:
                                pass
                        print(f"  [{m['msg_id'][:16]}...] from {m['sender'][:16]}... "
                              f"gek_v{m.get('gek_version', '?')}{blob_text}")
                else:
                    print(f"Error: {resp}")

            elif cmd == "group_get":
                if len(parts) < 3:
                    print("Usage: group_get <group_id> <msg_id>")
                    continue
                resp = await client.cmd_group_get(parts[1], parts[2])
                if resp.get("ok") and resp.get("blob"):
                    raw = bytes.fromhex(resp["blob"])
                    print(f"Message ({len(raw)} bytes):")
                    print(raw.decode("utf-8", errors="replace"))
                elif resp.get("ok"):
                    print(f"Large: {resp.get('size')} bytes (chunked, not supported yet)")
                else:
                    print(f"Error: {resp}")

            elif cmd == "group_delete":
                if len(parts) < 3:
                    print("Usage: group_delete <group_id> <msg_id>")
                    continue
                resp = await client.cmd_group_delete(parts[1], parts[2])
                print("Deleted." if resp.get("ok") else f"Error: {resp}")

            elif cmd == "group_destroy":
                if len(parts) < 2:
                    print("Usage: group_destroy <group_id>")
                    continue
                resp = await client.cmd_group_destroy(parts[1])
                print("Destroyed." if resp.get("ok") else f"Error: {resp}")

            else:
                print(f"Unknown command: {cmd}. Type 'help' for commands.")

        except Exception as e:
            print(f"Error: {e}")


async def main():
    pubkey, seckey = load_or_create_identity()
    client = ChromatinClient(pubkey, seckey)
    client.set_push_callback(push_handler)

    # Auto-connect if args provided
    if len(sys.argv) >= 3:
        host, port = sys.argv[1], int(sys.argv[2])
        print(f"Connecting to {host}:{port}...")
        resp = await client.connect(host, port)
        resp_type = resp.get("type", "")
        if resp_type == "REDIRECT":
            nodes = resp.get("nodes", [])
            print("REDIRECT — try:")
            for n in nodes:
                print(f"  {n['address']}:{n['ws_port']}")
        elif resp_type == "ERROR":
            print(f"Error: {resp.get('reason')}")
        else:
            print(f"Authenticated. {resp.get('pending_messages', 0)} pending.")

    try:
        await repl(client)
    finally:
        await client.disconnect()
        print("Goodbye.")


if __name__ == "__main__":
    asyncio.run(main())
```

**Step 4: Run all tests**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/ -v`
Expected: 11 passed (6 crypto + 3 protocol + 2 builder)

**Step 5: Quick smoke test (no server needed)**

Run: `source .venv/bin/activate && cd tools/client && python chromatin_client.py`
Expected: Generates identity, shows prompt, `help` works, `identity` shows fingerprint, `quit` exits.

**Step 6: Commit**

```bash
git add tools/client/chromatin_client.py tools/client/requirements.txt tools/client/tests/__init__.py
git commit -m "feat(client): add interactive REPL with all protocol commands"
```

---

### Task 5: Run All Tests and Final Verification

**Step 1: Run the full test suite**

Run: `source .venv/bin/activate && python -m pytest tools/client/tests/ -v`
Expected: All 11 tests pass.

**Step 2: Smoke test the REPL**

Run: `source .venv/bin/activate && cd tools/client && python chromatin_client.py`
Test: type `help`, `identity`, `quit`

**Step 3: Commit any fixes**

If anything needed fixing, commit here.

**Step 4: Final commit with all files**

```bash
git add tools/client/
git commit -m "feat(client): complete Python test client for Chromatin protocol"
```
