#!/usr/bin/env python3
"""Chromatin interactive test client."""

import asyncio
import base64
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
NAME_POW_DIFFICULTY = 26


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
  connect <host> <port> [--tls] Connect and authenticate
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
                    print("Usage: connect <host> <port> [--tls]")
                    continue
                host, port = parts[1], int(parts[2])
                tls = "--tls" in parts[3:]
                print(f"Connecting to {host}:{port} ({'WSS' if tls else 'WS'})...")
                resp = await client.connect(host, port, tls=tls)
                resp_type = resp.get("type", "")
                if resp_type == "REDIRECT":
                    nodes = resp.get("nodes", [])
                    print("REDIRECT -- not responsible. Try:")
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
                                blob_preview = f" -- {raw[:80].decode('utf-8', errors='replace')}"
                            except Exception:
                                blob_preview = f" -- ({m.get('size', '?')} bytes)"
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
                print("Deleted." if resp.get("type") == "OK" else f"Error: {resp}")

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
                                blob_text = base64.b64decode(r["blob"]).decode(
                                    "utf-8", errors="replace"
                                )
                            except Exception:
                                pass
                        print(f"  from {r['from'][:16]}... -- {blob_text}")

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
                    client.seckey, name, client.fingerprint, client.pubkey,
                    nonce, sequence=1,
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
                                blob_text = bytes.fromhex(m["blob"]).decode(
                                    "utf-8", errors="replace"
                                )
                                blob_text = f" -- {blob_text[:80]}"
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
        tls = "--tls" in sys.argv[3:]
        print(f"Connecting to {host}:{port} ({'WSS' if tls else 'WS'})...")
        resp = await client.connect(host, port, tls=tls)
        resp_type = resp.get("type", "")
        if resp_type == "REDIRECT":
            nodes = resp.get("nodes", [])
            print("REDIRECT -- try:")
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
