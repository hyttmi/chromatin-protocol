"""WebSocket protocol layer for Chromatin client."""

import asyncio
import base64
import json
import struct
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
        }, timeout=30.0)

    async def cmd_group_create(self, group_meta: bytes) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_CREATE", "id": mid,
            "group_meta": group_meta.hex(),
        }, timeout=30.0)

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


# --- Standalone message builders ---

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
    """Build the payload for ALLOW/REVOKE signatures.
    Format: "chromatin:allowlist:" || own_fp(32) || action(1) || target_fp(32) || sequence(8 BE)
    """
    return (
        b"chromatin:allowlist:"
        + own_fp
        + bytes([action])
        + target_fp
        + struct.pack(">Q", sequence)
    )
