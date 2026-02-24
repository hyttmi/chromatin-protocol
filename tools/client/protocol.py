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
        self._binary_frames: list[bytes] = []
        self._debug = False

    def _msg_id(self) -> int:
        mid = self._next_id
        self._next_id += 1
        return mid

    async def connect(self, host: str, port: int, tls: bool = False) -> dict:
        """Connect, perform HELLO + AUTH handshake. Returns OK response."""
        scheme = "wss" if tls else "ws"
        uri = f"{scheme}://{host}:{port}/ws"
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
        node_fp = bytes.fromhex(resp["node_fingerprint"])

        # AUTH
        auth_id = self._msg_id()
        await self.ws.send(build_auth(self.pubkey, self.seckey, nonce, auth_id, node_fp))
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
                    self._binary_frames.append(raw)
                    if self._debug:
                        ft = raw[0] if len(raw) > 0 else -1
                        ci = struct.unpack(">H", raw[5:7])[0] if len(raw) >= 7 else -1
                        print(f"    [listen] binary frame: type=0x{ft:02x} "
                              f"chunk_idx={ci} len={len(raw)}")
                    continue
                msg = json.loads(raw)
                msg_id = msg.get("id")
                if msg_id is not None and msg_id in self._pending:
                    self._pending.pop(msg_id).set_result(msg)
                elif self._push_callback:
                    await self._push_callback(msg)
        except websockets.ConnectionClosed:
            if self._debug:
                print("    [listen] connection closed")
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
        }, timeout=300.0)

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

    async def cmd_send_large(self, to_fp: str, blob: bytes,
                              timeout: float = 60.0) -> dict:
        """Send a large message via chunked upload."""
        CHUNK_SIZE = 1048576  # 1 MiB
        mid = self._msg_id()
        cmd = {"type": "SEND", "id": mid, "to": to_fp, "size": len(blob)}

        future = asyncio.get_event_loop().create_future()
        self._pending[mid] = future
        await self.ws.send(json.dumps(cmd))

        resp = await asyncio.wait_for(future, timeout)
        if resp.get("type") != "SEND_READY":
            return resp

        request_id = resp["request_id"]

        num_chunks = (len(blob) + CHUNK_SIZE - 1) // CHUNK_SIZE
        for i in range(num_chunks):
            offset = i * CHUNK_SIZE
            payload = blob[offset:offset + CHUNK_SIZE]
            frame = bytearray()
            frame.append(0x01)  # UPLOAD_CHUNK
            frame += struct.pack(">I", request_id)
            frame += struct.pack(">H", i)
            frame += payload
            await self.ws.send(bytes(frame))

        ok_future = asyncio.get_event_loop().create_future()
        self._pending[mid] = ok_future
        return await asyncio.wait_for(ok_future, timeout)

    async def cmd_get_large(self, msg_id: str, timeout: float = 60.0) -> dict:
        """Get a message, collecting chunked download frames if needed.

        Returns dict with 'blob_bytes' key containing the raw bytes for
        chunked downloads, or standard response for inline."""
        # Clear any stale binary frames
        self._binary_frames.clear()

        mid = self._msg_id()
        resp = await self.send_command(
            {"type": "GET", "id": mid, "msg_id": msg_id}, timeout=timeout)

        if resp.get("type") != "OK":
            return resp

        if resp.get("blob"):
            resp["blob_bytes"] = base64.b64decode(resp["blob"])
            return resp

        num_chunks = resp.get("chunks", 0)
        expected_size = resp.get("size", 0)
        chunks = {}

        if self._debug:
            print(f"    [get_large] expecting {num_chunks} chunks, "
                  f"size={expected_size}, buffered={len(self._binary_frames)}")

        deadline = asyncio.get_event_loop().time() + timeout
        poll_count = 0
        while len(chunks) < num_chunks:
            # Process any buffered binary frames
            while self._binary_frames:
                raw = self._binary_frames.pop(0)
                if len(raw) >= 7 and raw[0] == 0x02:
                    chunk_index = struct.unpack(">H", raw[5:7])[0]
                    chunks[chunk_index] = raw[7:]
                    if self._debug:
                        print(f"    [get_large] processed chunk {chunk_index}, "
                              f"have {len(chunks)}/{num_chunks}")

            if len(chunks) >= num_chunks:
                break

            # Poll: yield control so the listener can receive more frames
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                if self._debug:
                    print(f"    [get_large] TIMEOUT after {poll_count} polls, "
                          f"have chunks: {sorted(chunks.keys())}")
                raise asyncio.TimeoutError(
                    f"chunked download timeout: got {len(chunks)}/{num_chunks} chunks")
            poll_count += 1
            await asyncio.sleep(0.05)

        result = bytearray()
        for i in range(num_chunks):
            result += chunks[i]
        resp["blob_bytes"] = bytes(result[:expected_size])
        return resp

    async def cmd_group_destroy(self, group_id: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "GROUP_DESTROY", "id": mid,
            "group_id": group_id,
        })

    async def cmd_event(self, to_fp: str, event: str) -> dict:
        mid = self._msg_id()
        return await self.send_command({
            "type": "EVENT", "id": mid,
            "to": to_fp, "event": event,
        })


# --- Standalone message builders ---

def build_hello(fingerprint: bytes, msg_id: int) -> str:
    return json.dumps({
        "type": "HELLO",
        "id": msg_id,
        "fingerprint": fingerprint.hex(),
    })


def build_auth(pubkey: bytes, seckey: bytes, nonce: bytes, msg_id: int,
               node_fingerprint: bytes = b"") -> str:
    message = b"chromatin-auth:" + node_fingerprint + nonce
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
