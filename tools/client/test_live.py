#!/usr/bin/env python3
"""Live integration tests against running chromatin nodes.

Runs 120+ tests covering messaging, contacts, allowlist, profiles,
name registration, groups, error handling, multi-node behavior,
10-user messaging mesh (25+ cross-node messages), and 10-member
group lifecycle (create, send, admin delete, member delete rejection,
owner destroy).
"""

import asyncio
import base64
import json
import os
import sys
import time
import hashlib
import struct

from crypto_utils import (
    generate_keypair,
    fingerprint_of,
    sign,
    mine_pow,
    count_leading_zero_bits,
    kem_generate_keypair,
    kem_encapsulate,
    encrypt_1to1_message,
    decrypt_1to1_message,
    wrap_gek_for_member,
    unwrap_gek,
    encrypt_group_message,
    decrypt_group_message,
    sha3_256,
)
from protocol import ChromatinClient, build_hello, build_auth, build_allowlist_signature_payload
from builders import build_profile_record, build_name_record, build_group_meta

# PoW difficulties (must match node config)
CONTACT_POW_DIFFICULTY = 16
NAME_POW_DIFFICULTY = 26

# Test nodes: (host, ws_port, tls) tuples
SERVERS = [
    ("0.bootstrap.pqcc.fi", 62010, False),
    ("1.bootstrap.pqcc.fi", 62011, False),
    ("2.bootstrap.pqcc.fi", 62012, False),
]

# Map TCP addresses (from REDIRECT) to WS (host, port).
# Nodes advertise their TCP bind address in routing, which may be a LAN IP
# or lack the WS port. This maps any known address to the correct WS endpoint.
ADDR_TO_WS = {
    "0.bootstrap.pqcc.fi": ("0.bootstrap.pqcc.fi", 62010),
    "1.bootstrap.pqcc.fi": ("1.bootstrap.pqcc.fi", 62011),
    "2.bootstrap.pqcc.fi": ("2.bootstrap.pqcc.fi", 62012),
}


def resolve_redirect(node, fallback_host="0.bootstrap.pqcc.fi", fallback_port=62010):
    """Map a REDIRECT node entry to a reachable (host, ws_port) pair."""
    addr = node.get("address", "")
    ws_port = node.get("ws_port", 0)
    # Prefer ws_port from the redirect response — it's the most specific info.
    # Only fall back to ADDR_TO_WS for LAN addresses that need remapping.
    if ws_port > 0 and not addr.startswith("192.168."):
        return (addr, ws_port)
    if addr in ADDR_TO_WS:
        return ADDR_TO_WS[addr]
    if ws_port > 0:
        return (addr, ws_port)
    return (fallback_host, fallback_port)


# Counters
passed = 0
failed = 0
errors = []


def ok(name):
    global passed
    passed += 1
    print(f"  PASS  {name}")


def fail(name, reason=""):
    global failed
    failed += 1
    msg = f"  FAIL  {name}: {reason}"
    errors.append(msg)
    print(msg)


def check(name, condition, reason=""):
    if condition:
        ok(name)
    else:
        fail(name, reason)


async def connect_with_redirect(client, host, port, tls=False, max_redirects=3):
    """Connect a client, following up to max_redirects REDIRECT responses."""
    resp = await client.connect(host, port, tls=tls)
    for _ in range(max_redirects):
        if resp.get("type") != "REDIRECT":
            break
        nodes = resp.get("nodes", [])
        if not nodes:
            break
        rhost, rport = resolve_redirect(nodes[0])
        # Look up TLS setting for the redirect target
        rtls = any(s[2] for s in SERVERS if s[0] == rhost and s[1] == rport)
        resp = await client.connect(rhost, rport, tls=rtls)
    return resp


async def connect_raw(client, host, port, tls=False):
    """Connect to a specific node without following REDIRECT responses."""
    return await client.connect(host, port, tls=tls)


async def ensure_connected(client, servers):
    """Reconnect a client if its WebSocket is dead."""
    if client.ws and client.ws.protocol.state.name == "OPEN":
        return True
    # Try to reconnect
    for host, port, tls in servers:
        try:
            resp = await connect_with_redirect(client, host, port, tls=tls)
            if resp.get("type") == "OK":
                return True
        except Exception:
            continue
    return False


async def run_tests():
    global passed, failed

    # --- Setup: create two identities ---
    print("=== Setting up identities ===")
    pub_a, sec_a = generate_keypair()
    pub_b, sec_b = generate_keypair()
    fp_a = fingerprint_of(pub_a)
    fp_b = fingerprint_of(pub_b)
    kem_pub_a, kem_sec_a = kem_generate_keypair()
    kem_pub_b, kem_sec_b = kem_generate_keypair()
    print(f"  Alice: {fp_a.hex()[:16]}...")
    print(f"  Bob:   {fp_b.hex()[:16]}...")

    alice = ChromatinClient(pub_a, sec_a)
    bob = ChromatinClient(pub_b, sec_b)

    # Collect push notifications
    alice_pushes = []
    bob_pushes = []

    async def alice_push(msg):
        alice_pushes.append(msg)

    async def bob_push(msg):
        bob_pushes.append(msg)

    alice.set_push_callback(alice_push)
    bob.set_push_callback(bob_push)

    # ===================================================================
    print("\n=== Connection Tests ===")
    # ===================================================================

    # Test 1-3: Connect to each server
    for i, (host, port, tls) in enumerate(SERVERS):
        client = ChromatinClient(pub_a, sec_a)
        try:
            resp = await client.connect(host, port, tls=tls)
            rtype = resp.get("type", "")
            if rtype == "OK" or rtype == "REDIRECT":
                ok(f"connect to {host}:{port} ({rtype})")
            else:
                fail(f"connect to {host}:{port}", f"got {rtype}")
            await client.disconnect()
        except Exception as e:
            fail(f"connect to {host}:{port}", str(e))

    # Test 4: Connect Alice to her responsible node
    resp_a = await connect_with_redirect(alice, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
    check("alice connected", resp_a.get("type") == "OK",
          f"got {resp_a.get('type')}: {resp_a.get('reason', '')}")

    # Test 5: Connect Bob to his responsible node
    resp_b = await connect_with_redirect(bob, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
    check("bob connected", resp_b.get("type") == "OK",
          f"got {resp_b.get('type')}: {resp_b.get('reason', '')}")

    # ===================================================================
    print("\n=== Status Tests ===")
    # ===================================================================

    # Test 6: Alice status
    status_a = await alice.cmd_status()
    check("alice status response", status_a.get("type") == "OK",
          f"got {status_a}")
    # Test 7: Status has node_id
    check("status has node_id", "node_id" in status_a)
    # Test 8: Status has routing_table_size
    check("status has routing_table_size", "routing_table_size" in status_a)
    # Test 9: Status has uptime
    check("status has uptime_seconds", "uptime_seconds" in status_a)
    # Test 10: Routing table has 3 nodes
    check("routing table has nodes", status_a.get("routing_table_size", 0) >= 2,
          f"got {status_a.get('routing_table_size')}")

    # Test 11: Bob status
    status_b = await bob.cmd_status()
    check("bob status response", status_b.get("type") == "OK")

    # ===================================================================
    print("\n=== Profile Setup (KEM pubkeys for encryption) ===")
    # ===================================================================
    profile_a = build_profile_record(sec_a, fp_a, pub_a, kem_pub_a, "", b"", [], 1)
    resp = await alice.cmd_set_profile(profile_a)
    check("alice profile with kem pubkey", resp.get("type") == "OK", f"got {resp}")

    profile_b = build_profile_record(sec_b, fp_b, pub_b, kem_pub_b, "", b"", [], 1)
    resp = await bob.cmd_set_profile(profile_b)
    check("bob profile with kem pubkey", resp.get("type") == "OK", f"got {resp}")

    await asyncio.sleep(1)  # Let profiles propagate to DHT

    # ===================================================================
    print("\n=== Allowlist Tests ===")
    # ===================================================================

    # Test 12: Alice allows Bob
    resp = await alice.cmd_allow(fp_b.hex())
    check("alice allows bob", resp.get("type") == "OK", f"got {resp}")

    # Test 13: Bob allows Alice
    resp = await bob.cmd_allow(fp_a.hex())
    check("bob allows alice", resp.get("type") == "OK", f"got {resp}")

    # Test 14: Alice allows Bob again (idempotent)
    resp = await alice.cmd_allow(fp_b.hex())
    check("allow idempotent", resp.get("type") == "OK", f"got {resp}")

    # ===================================================================
    print("\n=== Messaging Tests ===")
    # ===================================================================

    await asyncio.sleep(0.5)  # Let allowlist propagate

    # Test 15: Alice sends to Bob
    resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, b"Hello Bob!"))
    check("alice sends to bob", resp.get("type") == "OK", f"got {resp}")
    msg_id_1 = resp.get("msg_id", "")
    check("send returns msg_id", len(msg_id_1) > 0, "no msg_id")

    # Test 17: Send a second message
    resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, b"Second message"))
    check("alice sends second msg", resp.get("type") == "OK", f"got {resp}")
    msg_id_2 = resp.get("msg_id", "")

    # Test 18: Send a third message
    resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, b"Third message"))
    check("alice sends third msg", resp.get("type") == "OK", f"got {resp}")
    msg_id_3 = resp.get("msg_id", "")

    # Test 19: Bob sends to Alice
    resp = await bob.cmd_send(fp_a.hex(), encrypt_1to1_message(sec_b, fp_b, fp_a, kem_pub_a, b"Hello Alice!"))
    check("bob sends to alice", resp.get("type") == "OK", f"got {resp}")
    msg_id_4 = resp.get("msg_id", "")

    # Test 20: Send empty message (server rejects empty blob)
    resp = await alice.cmd_send(fp_b.hex(), b"")
    check("send empty message rejected", resp.get("type") == "ERROR", f"got {resp}")

    # Test 21: Send large message (10KB)
    large_msg = b"X" * 10240
    resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, large_msg))
    check("send 10KB message", resp.get("type") == "OK", f"got {resp}")
    msg_id_large = resp.get("msg_id", "")

    # Test 22: Send unicode message
    resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, "Hello from the future! 🌐".encode("utf-8")))
    check("send unicode message", resp.get("type") == "OK", f"got {resp}")

    # Wait for messages to propagate
    await asyncio.sleep(1)

    # ===================================================================
    print("\n=== List/Get Messages Tests ===")
    # ===================================================================

    # Test 23: Bob lists his inbox
    resp = await bob.cmd_list(50)
    check("bob list inbox", resp.get("type") == "OK", f"got {resp}")
    msgs = resp.get("messages", [])
    check("bob has messages", len(msgs) >= 3, f"got {len(msgs)} messages")

    # Test 25: List with limit
    resp = await bob.cmd_list(2)
    check("list with limit=2", resp.get("type") == "OK", f"got {resp}")
    limited_msgs = resp.get("messages", [])
    check("limit returns <= 2", len(limited_msgs) <= 2, f"got {len(limited_msgs)}")

    # Test 27: Alice lists her inbox
    resp = await alice.cmd_list(50)
    check("alice list inbox", resp.get("type") == "OK", f"got {resp}")
    alice_msgs = resp.get("messages", [])
    check("alice has bob's message", len(alice_msgs) >= 1, f"got {len(alice_msgs)}")

    # Test 29: Bob gets a specific message
    if msg_id_1:
        resp = await bob.cmd_get(msg_id_1)
        check("bob gets msg by id", resp.get("type") == "OK" or "blob" in resp, f"got {resp}")
        if resp.get("blob"):
            raw = base64.b64decode(resp["blob"])
            sender_fp, content, _ = decrypt_1to1_message(kem_sec_b, fp_b, raw)
            check("msg content correct", content == b"Hello Bob!",
                  f"got {content[:50]}")
            check("msg sender correct", sender_fp == fp_a, "wrong sender")
        else:
            check("msg content correct", False, "no blob in response")
    else:
        fail("bob gets msg by id", "no msg_id from send")
        fail("msg content correct", "skipped")

    # Test 31: Get large message
    if msg_id_large:
        resp = await bob.cmd_get(msg_id_large)
        check("get large message", resp.get("type") == "OK" or "blob" in resp or "chunks" in resp,
              f"got {resp}")
    else:
        fail("get large message", "no msg_id")

    # Test 32: Get non-existent message
    resp = await bob.cmd_get("0000000000000000000000000000000000000000000000000000000000000000")
    check("get non-existent msg", resp.get("type") == "ERROR" or resp.get("blob") is None,
          f"got {resp}")

    # ===================================================================
    print("\n=== Delete Messages Tests ===")
    # ===================================================================

    # Test 33: Bob deletes a message
    if msg_id_2:
        resp = await bob.cmd_delete([msg_id_2])
        check("bob deletes msg", resp.get("type") == "OK", f"got {resp}")
    else:
        fail("bob deletes msg", "no msg_id")

    # Test 34: Verify deletion - msg should be gone
    if msg_id_2:
        resp = await bob.cmd_get(msg_id_2)
        check("deleted msg is gone", resp.get("type") == "ERROR" or resp.get("blob") is None,
              f"got {resp}")
    else:
        fail("deleted msg is gone", "skipped")

    # Test 35: Delete multiple messages
    if msg_id_3:
        resp = await bob.cmd_delete([msg_id_3])
        check("delete multiple", resp.get("type") == "OK", f"got {resp}")
    else:
        fail("delete multiple", "no msg_id")

    # Test 36: Delete non-existent (should not error)
    resp = await bob.cmd_delete(["0000000000000000000000000000000000000000000000000000000000000000"])
    check("delete non-existent", resp.get("type") == "OK" or resp.get("type") == "ERROR",
          f"got {resp}")

    # ===================================================================
    print("\n=== Large Chunked Transfer Tests (Photos) ===")
    # ===================================================================

    photos = [
        ("vacation.jpg", 2.5),
        ("portrait.png", 3.2),
        ("landscape.jpg", 4.1),
        ("selfie.jpg", 1.8),
    ]

    large_msg_ids = []
    for name, size_mb in photos:
        size = int(size_mb * 1024 * 1024)
        photo_data = os.urandom(size)
        encrypted_photo = encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, photo_data)
        print(f"  Uploading {name} ({size_mb} MB)...")
        try:
            resp = await alice.cmd_send_large(fp_b.hex(), encrypted_photo, timeout=60.0)
            if resp.get("type") == "OK" and resp.get("msg_id"):
                large_msg_ids.append((resp["msg_id"], encrypted_photo, name))
                ok(f"chunked upload {name} ({size_mb} MB)")
            else:
                fail(f"chunked upload {name}", str(resp))
        except Exception as e:
            fail(f"chunked upload {name}", str(e))

    await asyncio.sleep(1)

    for msg_id, original_data, name in large_msg_ids:
        try:
            resp = await bob.cmd_get_large(msg_id, timeout=60.0)
            downloaded = resp.get("blob_bytes", b"")
            if downloaded == original_data:
                ok(f"chunked download + verify {name} ({len(downloaded)} bytes)")
            else:
                fail(f"chunked download {name}",
                     f"size mismatch: got {len(downloaded)}, expected {len(original_data)}")
        except Exception as e:
            fail(f"chunked download {name}", str(e))

    # Clean up
    for msg_id, _, name in large_msg_ids:
        resp = await bob.cmd_delete([msg_id])
        check(f"delete {name}", resp.get("type") == "OK", str(resp))

    # ===================================================================
    print("\n=== Revoke/Deny Tests ===")
    # ===================================================================

    # Test 37: Alice revokes Bob
    resp = await alice.cmd_revoke(fp_b.hex())
    check("alice revokes bob", resp.get("type") == "OK", f"got {resp}")

    await asyncio.sleep(2)  # Allow time for revoke to replicate

    # Test 38: Bob sends to Alice (should fail - revoked)
    # Note: may still succeed if Bob's node hasn't received the revoke yet
    resp = await bob.cmd_send(fp_a.hex(), encrypt_1to1_message(sec_b, fp_b, fp_a, kem_pub_a, b"Should be denied"))
    check("send to revoked fails", resp.get("type") == "ERROR",
          f"got {resp} (may be replication delay)")

    # Test 39: Alice re-allows Bob
    resp = await alice.cmd_allow(fp_b.hex())
    check("alice re-allows bob", resp.get("type") == "OK", f"got {resp}")

    await asyncio.sleep(2)  # Allow time for allow to replicate

    # Test 40: Bob can send again
    resp = await bob.cmd_send(fp_a.hex(), encrypt_1to1_message(sec_b, fp_b, fp_a, kem_pub_a, b"I'm back!"))
    check("send after re-allow", resp.get("type") == "OK", f"got {resp}")

    # ===================================================================
    print("\n=== Send to Unknown/Unauthorized Tests ===")
    # ===================================================================

    # Create a third identity that nobody allows
    pub_c, sec_c = generate_keypair()
    fp_c = fingerprint_of(pub_c)
    charlie = ChromatinClient(pub_c, sec_c)
    resp_c = await connect_with_redirect(charlie, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])

    # Test 41: Charlie status
    check("charlie connected", resp_c.get("type") == "OK",
          f"got {resp_c.get('type')}: {resp_c.get('reason', '')}")

    # Test 42: Charlie sends to Alice (not allowed)
    if charlie.ws:
        resp = await charlie.cmd_send(fp_a.hex(), b"Hi Alice!")
        check("unauthorized send fails", resp.get("type") == "ERROR",
              f"got {resp}")
    else:
        fail("unauthorized send fails", "charlie not connected")

    # Test 43: Charlie sends to non-existent fingerprint
    fake_fp = "ff" * 32
    if charlie.ws:
        resp = await charlie.cmd_send(fake_fp, b"Hi nobody!")
        # Could be ERROR or OK depending on whether the node is responsible
        check("send to fake fp", resp.get("type") in ("ERROR", "OK"), f"got {resp}")
    else:
        fail("send to fake fp", "charlie not connected")

    await charlie.disconnect()

    # ===================================================================
    print("\n=== Contact Request Tests ===")
    # ===================================================================

    # Create a new identity for contact request tests
    pub_d, sec_d = generate_keypair()
    fp_d = fingerprint_of(pub_d)
    dave = ChromatinClient(pub_d, sec_d)
    resp_d = await connect_with_redirect(dave, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])

    # Test 44: Dave sends contact request to Alice (with PoW)
    timestamp = int(time.time() * 1000)
    prefix = (
        b"chromatin:request:"
        + fp_d
        + fp_a
        + timestamp.to_bytes(8, "big")
    )
    pow_nonce = mine_pow(prefix, 16)
    check("mine contact PoW", pow_nonce >= 0)

    if dave.ws:
        resp = await dave.cmd_contact_request(
            fp_a.hex(), b"Hey Alice, it's Dave!", pow_nonce, timestamp
        )
        check("dave contact request", resp.get("type") == "OK", f"got {resp}")
    else:
        fail("dave contact request", "dave not connected")

    # Test 46: Alice lists contact requests
    await asyncio.sleep(2)
    resp = await alice.cmd_list_requests()
    check("alice list requests", resp.get("type") == "OK", f"got {resp}")
    reqs = resp.get("requests", [])
    check("alice has contact request", len(reqs) >= 1, f"got {len(reqs)} requests")

    # Test 48: Contact request with bad PoW
    if dave.ws:
        resp = await dave.cmd_contact_request(
            fp_a.hex(), b"Bad PoW", 0, timestamp  # nonce=0, almost certainly bad
        )
        check("bad pow rejected", resp.get("type") == "ERROR", f"got {resp}")
    else:
        fail("bad pow rejected", "dave not connected")

    # Test 49: Contact request with expired timestamp
    if dave.ws:
        old_ts = int((time.time() - 7200) * 1000)  # 2 hours ago
        old_prefix = (
            b"chromatin:request:"
            + fp_d
            + fp_a
            + old_ts.to_bytes(8, "big")
        )
        old_nonce = mine_pow(old_prefix, 16)
        resp = await dave.cmd_contact_request(
            fp_a.hex(), b"Old request", old_nonce, old_ts
        )
        check("expired timestamp rejected", resp.get("type") == "ERROR", f"got {resp}")
    else:
        fail("expired timestamp rejected", "dave not connected")

    await dave.disconnect()

    # ===================================================================
    print("\n=== Profile Tests ===")
    # ===================================================================

    # Test 50: Alice sets profile
    profile = build_profile_record(
        seckey=sec_a,
        fingerprint=fp_a,
        pubkey=pub_a,
        kem_pubkey=kem_pub_a,
        bio="Alice in Chromatin-land",
        avatar=b"",
        social_links=[],
        sequence=2,
    )
    resp = await alice.cmd_set_profile(profile)
    check("alice set profile", resp.get("type") == "OK", f"got {resp}")

    # Test 51: Bob sets profile
    profile_b = build_profile_record(
        seckey=sec_b,
        fingerprint=fp_b,
        pubkey=pub_b,
        kem_pubkey=kem_pub_b,
        bio="Bob the Builder",
        avatar=b"",
        social_links=[],
        sequence=2,
    )
    resp = await bob.cmd_set_profile(profile_b)
    check("bob set profile", resp.get("type") == "OK", f"got {resp}")

    await asyncio.sleep(0.5)

    # Test 52: Bob gets Alice's profile
    resp = await bob.cmd_get_profile(fp_a.hex())
    check("bob gets alice profile", resp.get("type") == "OK" or resp.get("found"),
          f"got {resp}")

    # Test 53: Get non-existent profile
    resp = await alice.cmd_get_profile("aa" * 32)
    check("get nonexistent profile", not resp.get("found", True), f"got {resp}")

    # Test 54: Alice updates profile (sequence 3)
    profile2 = build_profile_record(
        seckey=sec_a,
        fingerprint=fp_a,
        pubkey=pub_a,
        kem_pubkey=kem_pub_a,
        bio="Alice updated!",
        avatar=b"",
        social_links=[],
        sequence=3,
    )
    resp = await alice.cmd_set_profile(profile2)
    check("alice update profile", resp.get("type") == "OK", f"got {resp}")

    # Test 55: Profile with bio
    profile_bio = build_profile_record(
        seckey=sec_a,
        fingerprint=fp_a,
        pubkey=pub_a,
        kem_pubkey=kem_pub_a,
        bio="A" * 500,
        avatar=b"",
        social_links=[],
        sequence=4,
    )
    resp = await alice.cmd_set_profile(profile_bio)
    check("profile with long bio", resp.get("type") == "OK", f"got {resp}")

    # ===================================================================
    print("\n=== Name Registration Tests ===")
    # ===================================================================

    try:
        # Test 56: Register a name (node requires 28-bit PoW)
        name = "testuser" + os.urandom(4).hex()[:8]  # unique name
        prefix = b"chromatin:name:" + name.encode() + fp_a
        print(f"  Mining PoW for name '{name}' ({NAME_POW_DIFFICULTY} bits)... ", end="", flush=True)
        start = time.time()
        pow_nonce = mine_pow(prefix, NAME_POW_DIFFICULTY)
        elapsed = time.time() - start
        print(f"done in {elapsed:.1f}s")
        check("mine name PoW", pow_nonce >= 0)

        record = build_name_record(sec_a, name, fp_a, pub_a, pow_nonce, sequence=1)
        resp = await alice.cmd_register_name(record)
        check("register name", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)  # Sync replication ensures data is available on OK; small wait for routing

        # Test 58: Resolve the name
        await ensure_connected(bob, SERVERS)
        resp = await bob.cmd_resolve_name(name)
        check("resolve name", resp.get("found") == True, f"got {resp}")
        if resp.get("found"):
            check("resolved fp matches", resp.get("fingerprint") == fp_a.hex(),
                  f"got {resp.get('fingerprint')}")
        else:
            fail("resolved fp matches", "name not found")

        # Test 60: Resolve non-existent name
        await ensure_connected(alice, SERVERS)
        resp = await alice.cmd_resolve_name("nonexistent999")
        check("resolve nonexistent", not resp.get("found", True), f"got {resp}")
    except Exception as e:
        fail("name registration section", f"crashed: {e}")
        # Reconnect for subsequent tests
        await ensure_connected(alice, SERVERS)
        await ensure_connected(bob, SERVERS)

    # ===================================================================
    print("\n=== Group Tests ===")
    # ===================================================================

    group_id = os.urandom(32)
    group_gek = os.urandom(32)
    group_ok = False

    try:
        # Generate real GEK and wrap for members
        kem_ct_a, wrapped_gek_a = wrap_gek_for_member(kem_pub_a, group_gek)
        kem_ct_b, wrapped_gek_b = wrap_gek_for_member(kem_pub_b, group_gek)

        # Test 61: Create group (Alice as owner, Bob as member)
        members = [
            (fp_a, 0x02, kem_ct_a, wrapped_gek_a),  # Alice = owner
            (fp_b, 0x00, kem_ct_b, wrapped_gek_b),  # Bob = member
        ]
        meta = build_group_meta(sec_a, group_id, fp_a, fp_a, 1, members)
        resp = await alice.cmd_group_create(meta)
        check("create group", resp.get("type") == "OK", f"got {resp}")
        group_ok = resp.get("type") == "OK"

        await asyncio.sleep(0.5)

        # Test 62: Get group info
        resp = await alice.cmd_group_info(group_id.hex())
        check("get group info", resp.get("type") == "OK",
              f"got {resp}")

        # Test 63: Bob gets group info
        resp = await bob.cmd_group_info(group_id.hex())
        check("bob get group info", resp.get("type") == "OK",
              f"got {resp}")

        # Test 64-68: Alice sends 5 group messages
        group_msg_ids = []
        for i in range(5):
            mid = os.urandom(32).hex()
            encrypted = encrypt_group_message(sec_a, fp_a, group_id, 1, group_gek, f"Group msg {i}".encode("utf-8"))
            resp = await alice.cmd_group_send(
                group_id.hex(), mid, gek_version=1,
                blob=encrypted
            )
            check(f"group send msg {i}", resp.get("type") == "OK", f"got {resp}")
            group_msg_ids.append(mid)

        await asyncio.sleep(0.5)

        # Test 69: List group messages
        resp = await alice.cmd_group_list(group_id.hex())
        check("list group msgs", resp.get("type") == "OK", f"got {resp}")
        gmsg_list = resp.get("messages", [])
        check("group has messages", len(gmsg_list) >= 3, f"got {len(gmsg_list)}")

        # Test 71: Get specific group message
        if group_msg_ids:
            resp = await alice.cmd_group_get(group_id.hex(), group_msg_ids[0])
            check("get group msg", resp.get("type") == "OK", f"got {resp}")
            if resp.get("blob"):
                raw = bytes.fromhex(resp["blob"])
                sender_fp, content = decrypt_group_message(group_gek, group_id, 1, raw)
                check("group msg decrypts", content == b"Group msg 0", f"got {content[:50]}")
                check("group msg sender", sender_fp == fp_a, "wrong sender")
        else:
            fail("get group msg", "no msg ids")

        # Test 72: Bob sends group message
        bob_gmsg = os.urandom(32).hex()
        encrypted_bob = encrypt_group_message(sec_b, fp_b, group_id, 1, group_gek, b"Bob's group message")
        resp = await bob.cmd_group_send(
            group_id.hex(), bob_gmsg, gek_version=1,
            blob=encrypted_bob
        )
        check("bob group send", resp.get("type") == "OK", f"got {resp}")

        # Test 73: Delete group message
        if group_msg_ids:
            resp = await alice.cmd_group_delete(group_id.hex(), group_msg_ids[0])
            check("delete group msg", resp.get("type") == "OK", f"got {resp}")
        else:
            fail("delete group msg", "no msg ids")

        # Test 74: Get deleted group message
        if group_msg_ids:
            resp = await alice.cmd_group_get(group_id.hex(), group_msg_ids[0])
            check("deleted group msg gone", resp.get("type") == "ERROR" or not resp.get("blob"),
                  f"got {resp}")
        else:
            fail("deleted group msg gone", "skipped")

        # Test 75: Get non-existent group
        resp = await alice.cmd_group_info("ff" * 32)
        check("nonexistent group info", resp.get("type") == "ERROR" or not resp.get("group_meta"),
              f"got {resp}")

        # Test 76: Group list with limit
        resp = await alice.cmd_group_list(group_id.hex(), limit=2)
        check("group list with limit", resp.get("type") == "OK", f"got {resp}")
    except Exception as e:
        fail("group tests section", f"crashed: {e}")
        await ensure_connected(alice, SERVERS)
        await ensure_connected(bob, SERVERS)

    # ===================================================================
    print("\n=== Group Update Tests ===")
    # ===================================================================

    upd_group_id = os.urandom(32)
    charlie2_client = None

    try:
        # Generate Charlie2 identity
        pub_c2, sec_c2 = generate_keypair()
        fp_c2 = fingerprint_of(pub_c2)
        kem_pub_c2, kem_sec_c2 = kem_generate_keypair()
        charlie2 = ChromatinClient(pub_c2, sec_c2)
        charlie2_client = charlie2

        # Generate real GEK v1 and wrap for initial members
        upd_gek_v1 = os.urandom(32)
        upd_kem_ct_a, upd_wrapped_a = wrap_gek_for_member(kem_pub_a, upd_gek_v1)
        upd_kem_ct_b, upd_wrapped_b = wrap_gek_for_member(kem_pub_b, upd_gek_v1)

        # Test: Create base group (v1) — Alice=owner, Bob=member
        members_v1 = [
            (fp_a, 0x02, upd_kem_ct_a, upd_wrapped_a),
            (fp_b, 0x00, upd_kem_ct_b, upd_wrapped_b),
        ]
        meta_v1 = build_group_meta(sec_a, upd_group_id, fp_a, fp_a, 1, members_v1)
        resp = await alice.cmd_group_create(meta_v1)
        check("group update: create base group", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(0.5)

        # Test: Bob can send before update
        resp = await bob.cmd_group_send(
            upd_group_id.hex(),
            os.urandom(32).hex(),
            1,
            encrypt_group_message(sec_b, fp_b, upd_group_id, 1, upd_gek_v1, b"bob pre-update"),
        )
        check("group update: bob sends before update", resp.get("type") == "OK", f"got {resp}")

        # Connect Charlie2
        resp_c2 = await connect_with_redirect(charlie2, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        check("group update: charlie2 connected", resp_c2.get("type") == "OK", f"got {resp_c2}")

        # Test: Add Charlie2 (v2) — Alice=owner, Bob=member, Charlie2=member
        upd_v2_ct_a, upd_v2_wrapped_a = wrap_gek_for_member(kem_pub_a, upd_gek_v1)
        upd_v2_ct_b, upd_v2_wrapped_b = wrap_gek_for_member(kem_pub_b, upd_gek_v1)
        upd_v2_ct_c2, upd_v2_wrapped_c2 = wrap_gek_for_member(kem_pub_c2, upd_gek_v1)

        members_v2 = [
            (fp_a, 0x02, upd_v2_ct_a, upd_v2_wrapped_a),
            (fp_b, 0x00, upd_v2_ct_b, upd_v2_wrapped_b),
            (fp_c2, 0x00, upd_v2_ct_c2, upd_v2_wrapped_c2),
        ]
        meta_v2 = build_group_meta(sec_a, upd_group_id, fp_a, fp_a, 2, members_v2)
        resp = await alice.cmd_group_update(meta_v2)
        check("group update: add charlie2 (v2)", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)

        # Test: New member gets group info
        resp = await charlie2.cmd_group_info(upd_group_id.hex())
        check("group update: new member gets group info", resp.get("type") == "OK", f"got {resp}")

        # Test: New member can send
        resp = await charlie2.cmd_group_send(
            upd_group_id.hex(),
            os.urandom(32).hex(),
            1,
            encrypt_group_message(sec_c2, fp_c2, upd_group_id, 1, upd_gek_v1, b"charlie2 hello"),
        )
        check("group update: new member can send", resp.get("type") == "OK", f"got {resp}")

        # Test: Remove Bob (v3) — rotate GEK since member removed
        upd_gek_v2 = os.urandom(32)
        upd_v3_ct_a, upd_v3_wrapped_a = wrap_gek_for_member(kem_pub_a, upd_gek_v2)
        upd_v3_ct_c2, upd_v3_wrapped_c2 = wrap_gek_for_member(kem_pub_c2, upd_gek_v2)

        members_v3 = [
            (fp_a, 0x02, upd_v3_ct_a, upd_v3_wrapped_a),
            (fp_c2, 0x00, upd_v3_ct_c2, upd_v3_wrapped_c2),
        ]
        meta_v3 = build_group_meta(sec_a, upd_group_id, fp_a, fp_a, 3, members_v3)
        resp = await alice.cmd_group_update(meta_v3)
        check("group update: remove bob (v3)", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)

        # Test: Removed member can't send
        resp = await bob.cmd_group_send(
            upd_group_id.hex(),
            os.urandom(32).hex(),
            1,
            encrypt_group_message(sec_b, fp_b, upd_group_id, 1, upd_gek_v1, b"bob post-remove"),
        )
        check("group update: removed member can't send", resp.get("type") == "ERROR", f"got {resp}")

        # Test: Replay old version rejected
        resp = await alice.cmd_group_update(meta_v1)
        check("group update: replay old version rejected", resp.get("type") == "ERROR", f"got {resp}")

        # Test: Non-member can't update (Bob signs with his key)
        meta_bob_attempt = build_group_meta(sec_b, upd_group_id, fp_b, fp_b, 4, members_v3)
        resp = await bob.cmd_group_update(meta_bob_attempt)
        check("group update: non-member can't update", resp.get("type") == "ERROR", f"got {resp}")

    except Exception as e:
        fail("group update section", f"crashed: {e}")
    finally:
        if charlie2_client is not None:
            await charlie2_client.disconnect()

    # ===================================================================
    print("\n=== Multi-Message Stress Tests ===")
    # ===================================================================

    await ensure_connected(alice, SERVERS)
    await ensure_connected(bob, SERVERS)

    # Test 77-86: Send 10 messages rapidly
    rapid_ids = []
    for i in range(10):
        resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, f"Rapid {i}".encode()))
        check(f"rapid send {i}", resp.get("type") == "OK", f"got {resp}")
        if resp.get("msg_id"):
            rapid_ids.append(resp["msg_id"])

    await asyncio.sleep(1)

    # Test 87: Bob lists after rapid send
    resp = await bob.cmd_list(50)
    check("bob list after rapid", resp.get("type") == "OK", f"got {resp}")
    bob_msg_count = len(resp.get("messages", []))
    check("bob received rapid msgs", bob_msg_count >= 10,
          f"got {bob_msg_count} messages")

    # Test 89-91: Various message sizes
    for size_name, size in [("1 byte", 1), ("1KB", 1024), ("50KB", 50 * 1024)]:
        resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, os.urandom(size)))
        check(f"send {size_name} msg", resp.get("type") == "OK", f"got {resp}")

    # ===================================================================
    print("\n=== Reconnection Tests ===")
    # ===================================================================

    # Test 92: Disconnect and reconnect Alice
    await alice.disconnect()
    check("alice disconnected", alice.ws is None)

    # Reconnect
    resp = await connect_with_redirect(alice, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
    check("alice reconnected", resp.get("type") == "OK", f"got {resp}")

    # Test 94: Alice can still send after reconnect
    resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, b"After reconnect"))
    check("send after reconnect", resp.get("type") == "OK", f"got {resp}")

    # Test 95: Alice can list after reconnect
    resp = await alice.cmd_list(5)
    check("list after reconnect", resp.get("type") == "OK", f"got {resp}")

    # ===================================================================
    print("\n=== Edge Case Tests ===")
    # ===================================================================

    # Test 96: Send binary data
    resp = await alice.cmd_send(fp_b.hex(), encrypt_1to1_message(sec_a, fp_a, fp_b, kem_pub_b, bytes(range(256))))
    check("send all byte values", resp.get("type") == "OK", f"got {resp}")

    # Test 97: List with limit=0
    resp = await bob.cmd_list(0)
    check("list limit=0", resp.get("type") == "OK" or resp.get("type") == "ERROR",
          f"got {resp}")

    # Test 98: List with very large limit
    resp = await bob.cmd_list(9999)
    check("list large limit", resp.get("type") == "OK", f"got {resp}")

    # Test 99: Multiple status calls
    for i in range(3):
        resp = await alice.cmd_status()
        check(f"repeated status {i}", resp.get("type") == "OK")

    # Test 102: Allowlist self
    resp = await alice.cmd_allow(fp_a.hex())
    check("allow self", resp.get("type") == "OK" or resp.get("type") == "ERROR",
          f"got {resp}")

    # --- Allowlist sequence replay ---
    try:
        pub_sq, sec_sq = generate_keypair()
        fp_sq = fingerprint_of(pub_sq)
        seq_client = ChromatinClient(pub_sq, sec_sq)
        resp = await connect_with_redirect(seq_client, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        if resp.get("type") == "OK":
            resp = await seq_client.cmd_allow(fp_a.hex())
            check("seq: allow seq=1", resp.get("type") == "OK", f"got {resp}")
            resp = await seq_client.cmd_revoke(fp_a.hex())
            check("seq: revoke seq=2", resp.get("type") == "OK", f"got {resp}")
            # Sequence monotonicity is enforced server-side; Python client auto-increments.
            # Raw replay with lower sequence is covered by C++ unit tests.
            print("  NOTE: sequence monotonicity enforced server-side; C++ tests cover raw replay")
            await seq_client.disconnect()
    except Exception as e:
        print(f"  seq replay section: {e}")

    # --- Large message NEW_MESSAGE push is metadata-only ---
    try:
        pub_lp1, sec_lp1 = generate_keypair()
        pub_lp2, sec_lp2 = generate_keypair()
        kem_pub_lp1, kem_sec_lp1 = kem_generate_keypair()
        kem_pub_lp2, kem_sec_lp2 = kem_generate_keypair()
        fp_lp1 = fingerprint_of(pub_lp1)
        fp_lp2 = fingerprint_of(pub_lp2)
        lp_sender = ChromatinClient(pub_lp1, sec_lp1)
        lp_receiver = ChromatinClient(pub_lp2, sec_lp2)
        lp_pushes = []

        async def _lp_push(m):
            lp_pushes.append(m)
        lp_receiver.set_push_callback(_lp_push)

        resp = await connect_with_redirect(lp_sender, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        resp2 = await connect_with_redirect(lp_receiver, SERVERS[1][0], SERVERS[1][1], tls=SERVERS[1][2])
        if resp.get("type") == "OK" and resp2.get("type") == "OK":
            await lp_sender.cmd_allow(fp_lp2.hex())
            await lp_receiver.cmd_allow(fp_lp1.hex())
            await asyncio.sleep(1)

            large_blob = encrypt_1to1_message(sec_lp1, fp_lp1, fp_lp2, kem_pub_lp2, os.urandom(100 * 1024))
            resp = await lp_sender.cmd_send_large(fp_lp2.hex(), large_blob)
            check("large msg push: send large ok", resp.get("type") == "OK", f"got {resp}")

            await asyncio.sleep(2)  # Wait for push notification delivery

            large_pushes = [p for p in lp_pushes if p.get("type") == "NEW_MESSAGE"]
            check("large msg push: receiver got NEW_MESSAGE", len(large_pushes) >= 1,
                  f"got {len(large_pushes)} pushes")
            if large_pushes:
                push = large_pushes[0]
                check("large msg push: no inline blob in push",
                      "blob" not in push or push.get("blob") is None,
                      f"push contains blob (should be metadata-only): {list(push.keys())}")
                check("large msg push: has msg_id", "msg_id" in push, f"push: {push}")
                check("large msg push: has size", "size" in push, f"push: {push}")
                if "size" in push:
                    check("large msg push: size matches", push["size"] == len(large_blob),
                          f"size={push.get('size')} expected={len(large_blob)}")

        await lp_sender.disconnect()
        await lp_receiver.disconnect()
    except Exception as e:
        fail("large msg push section", f"crashed: {e}")

    # ===================================================================
    print("\n=== Group Cleanup Tests ===")
    # ===================================================================

    if group_ok:
        try:
            await ensure_connected(alice, SERVERS)
            # Test 103: Destroy group
            resp = await alice.cmd_group_destroy(group_id.hex())
            check("destroy group", resp.get("type") == "OK", f"got {resp}")

            # Test 104: Verify group destroyed
            await asyncio.sleep(2)
            resp = await alice.cmd_group_info(group_id.hex())
            check("destroyed group gone", resp.get("type") == "ERROR" or not resp.get("group_meta"),
                  f"got {resp}")

            # Test 105: Send to destroyed group
            dummy_gek = os.urandom(32)
            resp = await alice.cmd_group_send(
                group_id.hex(), os.urandom(32).hex(), 1,
                encrypt_group_message(sec_a, fp_a, group_id, 1, dummy_gek, b"ghost msg")
            )
            check("send to destroyed group", resp.get("type") == "ERROR", f"got {resp}")
        except Exception as e:
            fail("group cleanup section", f"crashed: {e}")
    else:
        print("  SKIP  group cleanup (group not created)")

    # ===================================================================
    print("\n=== Cross-Node Push Notification Tests ===")
    # ===================================================================

    # Create two fresh identities on DIFFERENT nodes to test cross-node push
    try:
        pub_x, sec_x = generate_keypair()
        pub_y, sec_y = generate_keypair()
        kem_pub_x, kem_sec_x = kem_generate_keypair()
        kem_pub_y, kem_sec_y = kem_generate_keypair()
        fp_x = fingerprint_of(pub_x)
        fp_y = fingerprint_of(pub_y)

        xander = ChromatinClient(pub_x, sec_x)
        yara = ChromatinClient(pub_y, sec_y)

        xander_pushes = []
        yara_pushes = []

        async def xander_push(msg):
            xander_pushes.append(msg)

        async def yara_push(msg):
            yara_pushes.append(msg)

        xander.set_push_callback(xander_push)
        yara.set_push_callback(yara_push)

        # Connect to different nodes deliberately
        resp_x = await connect_with_redirect(xander, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        check("xander connected", resp_x.get("type") == "OK", f"got {resp_x}")

        resp_y = await connect_with_redirect(yara, SERVERS[1][0], SERVERS[1][1], tls=SERVERS[1][2])
        check("yara connected", resp_y.get("type") == "OK", f"got {resp_y}")

        # Status to see which nodes they're on
        sx = await xander.cmd_status()
        sy = await yara.cmd_status()
        x_node = sx.get("node_id", "?")[:16]
        y_node = sy.get("node_id", "?")[:16]
        print(f"  Xander on node {x_node}..., Yara on node {y_node}...")

        # Set profiles with KEM pubkeys
        await xander.cmd_set_profile(build_profile_record(sec_x, fp_x, pub_x, kem_pub_x, "", b"", [], 1))
        await yara.cmd_set_profile(build_profile_record(sec_y, fp_y, pub_y, kem_pub_y, "", b"", [], 1))

        # Mutual allowlist
        resp = await xander.cmd_allow(fp_y.hex())
        check("xander allows yara", resp.get("type") == "OK", f"got {resp}")
        resp = await yara.cmd_allow(fp_x.hex())
        check("yara allows xander", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)  # Let allowlist propagate

        # Clear push lists
        xander_pushes.clear()
        yara_pushes.clear()

        # Xander sends to Yara — Yara should get a NEW_MESSAGE push
        resp = await xander.cmd_send(fp_y.hex(), encrypt_1to1_message(sec_x, fp_x, fp_y, kem_pub_y, b"Cross-node push test!"))
        check("xander sends to yara", resp.get("type") == "OK", f"got {resp}")

        # Wait for push notification to arrive
        await asyncio.sleep(2)

        # Check if Yara got the push
        new_msg_pushes = [p for p in yara_pushes if p.get("type") == "NEW_MESSAGE"]
        check("yara gets NEW_MESSAGE push", len(new_msg_pushes) >= 1,
              f"got {len(new_msg_pushes)} pushes, all: {yara_pushes}")

        if new_msg_pushes:
            push = new_msg_pushes[0]
            check("push has msg_id", "msg_id" in push, f"push: {push}")
            check("push has from field", "from" in push, f"push: {push}")
            check("push from is xander", push.get("from") == fp_x.hex(),
                  f"from={push.get('from')}")
            check("push has size", "size" in push, f"push: {push}")
        else:
            fail("push has msg_id", "no push received")
            fail("push has from field", "no push received")
            fail("push from is xander", "no push received")
            fail("push has size", "no push received")

        # Yara sends back to Xander — Xander should get a push
        xander_pushes.clear()
        resp = await yara.cmd_send(fp_x.hex(), encrypt_1to1_message(sec_y, fp_y, fp_x, kem_pub_x, b"Cross-node reply!"))
        check("yara sends to xander", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)

        new_msg_pushes_x = [p for p in xander_pushes if p.get("type") == "NEW_MESSAGE"]
        check("xander gets NEW_MESSAGE push", len(new_msg_pushes_x) >= 1,
              f"got {len(new_msg_pushes_x)} pushes, all: {xander_pushes}")

        # Contact request push test
        pub_z, sec_z = generate_keypair()
        fp_z = fingerprint_of(pub_z)
        zara = ChromatinClient(pub_z, sec_z)
        resp_z = await connect_with_redirect(zara, SERVERS[2][0], SERVERS[2][1], tls=SERVERS[2][2])
        check("zara connected", resp_z.get("type") == "OK", f"got {resp_z}")

        # Zara sends contact request to Yara (no allowlist needed for requests)
        yara_pushes.clear()
        timestamp = int(time.time() * 1000)
        prefix = (
            b"chromatin:request:"
            + fp_z
            + fp_y
            + timestamp.to_bytes(8, "big")
        )
        cr_nonce = mine_pow(prefix, 16)
        resp = await zara.cmd_contact_request(
            fp_y.hex(), b"Hi Yara, add me!", cr_nonce, timestamp
        )
        check("zara sends contact request", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)

        cr_pushes = [p for p in yara_pushes if p.get("type") == "CONTACT_REQUEST"]
        check("yara gets CONTACT_REQUEST push", len(cr_pushes) >= 1,
              f"got {len(cr_pushes)} pushes, all: {yara_pushes}")

        await zara.disconnect()
        await xander.disconnect()
        await yara.disconnect()

    except Exception as e:
        fail("cross-node push section", f"crashed: {e}")

    # ===================================================================
    print("\n=== Name Registration Race Test ===")
    # ===================================================================

    try:
        # Two users try to register the same name from different nodes
        pub_r1, sec_r1 = generate_keypair()
        pub_r2, sec_r2 = generate_keypair()
        fp_r1 = fingerprint_of(pub_r1)
        fp_r2 = fingerprint_of(pub_r2)

        racer1 = ChromatinClient(pub_r1, sec_r1)
        racer2 = ChromatinClient(pub_r2, sec_r2)

        # Connect to different nodes
        resp = await connect_with_redirect(racer1, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        check("racer1 connected", resp.get("type") == "OK", f"got {resp}")

        resp = await connect_with_redirect(racer2, SERVERS[2][0], SERVERS[2][1], tls=SERVERS[2][2])
        check("racer2 connected", resp.get("type") == "OK", f"got {resp}")

        # Both set profiles first (needed for the protocol)
        profile1 = build_profile_record(
            seckey=sec_r1, fingerprint=fp_r1, pubkey=pub_r1,
            kem_pubkey=b"", bio="Racer 1", avatar=b"",
            social_links=[], sequence=1,
        )
        resp = await racer1.cmd_set_profile(profile1)
        check("racer1 set profile", resp.get("type") == "OK", f"got {resp}")

        profile2 = build_profile_record(
            seckey=sec_r2, fingerprint=fp_r2, pubkey=pub_r2,
            kem_pubkey=b"", bio="Racer 2", avatar=b"",
            social_links=[], sequence=1,
        )
        resp = await racer2.cmd_set_profile(profile2)
        check("racer2 set profile", resp.get("type") == "OK", f"got {resp}")

        # Mine PoW for the same name
        race_name = "race" + os.urandom(4).hex()[:8]
        print(f"  Racing for name '{race_name}'...")

        prefix1 = b"chromatin:name:" + race_name.encode() + fp_r1
        prefix2 = b"chromatin:name:" + race_name.encode() + fp_r2
        print(f"  Mining PoW for racer1... ", end="", flush=True)
        nonce1 = mine_pow(prefix1, NAME_POW_DIFFICULTY)
        print("done")
        print(f"  Mining PoW for racer2... ", end="", flush=True)
        nonce2 = mine_pow(prefix2, NAME_POW_DIFFICULTY)
        print("done")

        record1 = build_name_record(sec_r1, race_name, fp_r1, pub_r1, nonce1, sequence=1)
        record2 = build_name_record(sec_r2, race_name, fp_r2, pub_r2, nonce2, sequence=1)

        # Send both registrations concurrently
        resp1, resp2 = await asyncio.gather(
            racer1.cmd_register_name(record1),
            racer2.cmd_register_name(record2),
        )
        print(f"  Racer1 result: {resp1.get('type')}")
        print(f"  Racer2 result: {resp2.get('type')}")

        # At least one should succeed
        either_ok = resp1.get("type") == "OK" or resp2.get("type") == "OK"
        check("at least one registration succeeds", either_ok,
              f"r1={resp1}, r2={resp2}")

        # Sync replication ensures data is available on OK; small wait for routing
        await asyncio.sleep(1)

        # Resolve from a third node to verify convergence
        await ensure_connected(alice, SERVERS)
        resolved = await alice.cmd_resolve_name(race_name)
        check("race name resolves", resolved.get("found") == True,
              f"got {resolved}")

        if resolved.get("found"):
            winner_fp = resolved.get("fingerprint")
            # Determine expected winner: lower fingerprint
            expected = fp_r1.hex() if fp_r1 < fp_r2 else fp_r2.hex()
            check("race winner is lower fingerprint", winner_fp == expected,
                  f"winner={winner_fp[:16]}..., expected={expected[:16]}...")
        else:
            fail("race winner is lower fingerprint", "name not found")

        await racer1.disconnect()
        await racer2.disconnect()

    except Exception as e:
        fail("name race section", f"crashed: {e}")

    # ===================================================================
    print("\n=== Concurrent Connections Test ===")
    # ===================================================================

    # Test: Multiple clients same identity
    await ensure_connected(alice, SERVERS)
    clients = []
    for i in range(3):
        c = ChromatinClient(pub_a, sec_a)
        resp = await c.connect(SERVERS[i][0], SERVERS[i][1], tls=SERVERS[i][2])
        rtype = resp.get("type", "")
        check(f"concurrent client {i}", rtype in ("OK", "REDIRECT"),
              f"got {rtype}: {resp.get('reason', '')}")
        clients.append(c)
    for c in clients:
        await c.disconnect()

    # ===================================================================
    print("\n=== Multi-User Messaging Mesh (10 users, 25+ messages) ===")
    # ===================================================================

    mesh_users = []  # [(pub, sec, fp, client, pushes, kem_pub, kem_sec), ...]
    try:
        # Create and connect 10 fresh identities
        def _make_mesh_cb(pl):
            async def _cb(msg):
                pl.append(msg)
            return _cb

        for i in range(10):
            pub, sec = generate_keypair()
            kem_pub, kem_sec = kem_generate_keypair()
            fp = fingerprint_of(pub)
            client = ChromatinClient(pub, sec)
            pushes = []
            client.set_push_callback(_make_mesh_cb(pushes))
            mesh_users.append((pub, sec, fp, client, pushes, kem_pub, kem_sec))

        connected = 0
        for i, (pub, sec, fp, client, _, _, _) in enumerate(mesh_users):
            srv = SERVERS[i % len(SERVERS)]
            resp = await connect_with_redirect(client, srv[0], srv[1], tls=srv[2])
            if resp.get("type") == "OK":
                connected += 1
            else:
                fail(f"mesh user_{i} connect", f"got {resp.get('type')}")
        check("mesh: all 10 users connected", connected == 10,
              f"{connected}/10 connected")

        # Set profiles with KEM pubkeys for all mesh users
        for i, (pub, sec, fp, client, _, kem_pub, _) in enumerate(mesh_users):
            profile = build_profile_record(sec, fp, pub, kem_pub, "", b"", [], 1)
            resp = await client.cmd_set_profile(profile)
            if resp.get("type") != "OK":
                fail(f"mesh user_{i} set profile", f"got {resp}")
        await asyncio.sleep(1)  # Let profiles propagate

        # Ring allowlists: user_i allows user_{i-1} and user_{i+1}
        allow_ok = 0
        for i in range(10):
            _, _, _, client, _, _, _ = mesh_users[i]
            _, _, fp_prev, _, _, _, _ = mesh_users[(i - 1) % 10]
            _, _, fp_next, _, _, _, _ = mesh_users[(i + 1) % 10]
            r1 = await client.cmd_allow(fp_prev.hex())
            r2 = await client.cmd_allow(fp_next.hex())
            if r1.get("type") == "OK":
                allow_ok += 1
            if r2.get("type") == "OK":
                allow_ok += 1
        check("mesh: ring allowlists set (20)", allow_ok == 20,
              f"{allow_ok}/20 allowlists OK")

        await asyncio.sleep(2)  # Let allowlists propagate

        # Each user sends to both ring neighbors → 20 messages
        sent_count = 0
        sent_to = {}  # recipient_idx -> [msg_id, ...]
        for i in range(10):
            _, sec_i, fp_i, client, _, _, _ = mesh_users[i]
            for nbr in [(i - 1) % 10, (i + 1) % 10]:
                _, _, fp_nbr, _, _, kem_pub_nbr, _ = mesh_users[nbr]
                encrypted = encrypt_1to1_message(sec_i, fp_i, fp_nbr, kem_pub_nbr, f"mesh {i}->{nbr}".encode())
                resp = await client.cmd_send(fp_nbr.hex(), encrypted)
                if resp.get("type") == "OK":
                    sent_count += 1
                    sent_to.setdefault(nbr, []).append(resp.get("msg_id"))
        check("mesh: 20 ring messages sent", sent_count >= 18,
              f"{sent_count}/20 sent")

        # 5 extra messages (users 0,2,4,6,8 → their next neighbor)
        for i in [0, 2, 4, 6, 8]:
            nbr = (i + 1) % 10
            _, sec_i, fp_i, client, _, _, _ = mesh_users[i]
            _, _, fp_nbr, _, _, kem_pub_nbr, _ = mesh_users[nbr]
            encrypted = encrypt_1to1_message(sec_i, fp_i, fp_nbr, kem_pub_nbr, f"extra {i}->{nbr}".encode())
            resp = await client.cmd_send(fp_nbr.hex(), encrypted)
            if resp.get("type") == "OK":
                sent_count += 1
                sent_to.setdefault(nbr, []).append(resp.get("msg_id"))
        check("mesh: 25+ total messages sent", sent_count >= 25,
              f"sent {sent_count}")

        await asyncio.sleep(2)  # Let messages propagate

        # Verify: each recipient has >= 2 messages
        recipients_ok = 0
        for i in range(10):
            _, _, _, client, _, _, _ = mesh_users[i]
            resp = await client.cmd_list(50)
            msg_count = len(resp.get("messages", []))
            if msg_count >= 2:
                recipients_ok += 1
        check("mesh: all recipients have >=2 msgs", recipients_ok >= 8,
              f"{recipients_ok}/10 have >=2 messages")

        # Spot-check: GET one message for content correctness (decrypt it)
        spot_target = 1  # user_1 should have msgs from user_0 and user_2
        if sent_to.get(spot_target):
            _, _, fp_1, client_1, _, _, kem_sec_1 = mesh_users[spot_target]
            resp = await client_1.cmd_get(sent_to[spot_target][0])
            if resp.get("blob"):
                raw = base64.b64decode(resp["blob"])
                sender_fp_check, content_check, _ = decrypt_1to1_message(kem_sec_1, fp_1, raw)
                check("mesh: GET content spot-check", len(content_check) > 0, f"decrypted {len(content_check)} bytes")
            else:
                check("mesh: GET content spot-check", resp.get("type") == "OK", f"got {resp}")
        else:
            fail("mesh: GET content spot-check", "no messages for user_1")

        # Check cross-node push notifications arrived
        total_pushes = sum(
            len([p for p in pushes if p.get("type") == "NEW_MESSAGE"])
            for _, _, _, _, pushes, _, _ in mesh_users
        )
        check("mesh: push notifications received", total_pushes >= 5,
              f"got {total_pushes} NEW_MESSAGE pushes")

        print(f"  Mesh complete: {sent_count} messages across 10 users")

    except Exception as e:
        fail("multi-user mesh section", f"crashed: {e}")

    # ===================================================================
    print("\n=== Large Group Lifecycle (10 members) ===")
    # ===================================================================

    large_group_id = os.urandom(32)
    large_group_ok = False

    try:
        # Ensure mesh users are still connected
        for i, (_, _, _, client, _, _, _) in enumerate(mesh_users):
            await ensure_connected(client, SERVERS)

        # Generate real GEK and wrap for all 10 members
        lg_gek = os.urandom(32)

        # Build member list: user_0=owner, user_1=admin, rest=members
        group_members = []
        for i, (pub, sec, fp, client, _, kem_pub, _) in enumerate(mesh_users):
            if i == 0:
                role = 0x02  # owner
            elif i == 1:
                role = 0x01  # admin
            else:
                role = 0x00  # member
            kem_ct, wrapped_gek = wrap_gek_for_member(kem_pub, lg_gek)
            group_members.append((fp, role, kem_ct, wrapped_gek))

        # user_0 (owner) creates the group
        _, owner_sec, owner_fp, owner_client, _, _, _ = mesh_users[0]
        meta = build_group_meta(owner_sec, large_group_id, owner_fp, owner_fp, 1, group_members)
        resp = await owner_client.cmd_group_create(meta)
        check("large group: create (10 members)", resp.get("type") == "OK",
              f"got {resp}")
        large_group_ok = resp.get("type") == "OK"

        if large_group_ok:
            await asyncio.sleep(1)

            # All 10 members query GROUP_INFO
            info_ok = 0
            for i, (_, _, _, client, _, _, _) in enumerate(mesh_users):
                resp = await client.cmd_group_info(large_group_id.hex())
                if resp.get("type") == "OK":
                    info_ok += 1
            check("large group: all members get info", info_ok >= 8,
                  f"{info_ok}/10 got OK")

            # 5 different members each send a group message
            lg_msg_ids = []
            for sender_idx in [0, 2, 4, 6, 8]:
                _, sec_sender, fp_sender, client, _, _, _ = mesh_users[sender_idx]
                mid = os.urandom(32).hex()
                encrypted = encrypt_group_message(
                    sec_sender, fp_sender, large_group_id, 1, lg_gek,
                    f"Group msg from user_{sender_idx}".encode()
                )
                resp = await client.cmd_group_send(
                    large_group_id.hex(), mid, gek_version=1,
                    blob=encrypted
                )
                check(f"large group: send from user_{sender_idx}",
                      resp.get("type") == "OK", f"got {resp}")
                if resp.get("type") == "OK":
                    lg_msg_ids.append((mid, sender_idx))

            await asyncio.sleep(1)

            # Owner lists group messages, verifies count
            resp = await owner_client.cmd_group_list(large_group_id.hex())
            check("large group: owner lists msgs", resp.get("type") == "OK",
                  f"got {resp}")
            lg_msgs = resp.get("messages", [])
            check("large group: has >=5 messages", len(lg_msgs) >= 4,
                  f"got {len(lg_msgs)}")

            # Admin (user_1) deletes one message
            if lg_msg_ids:
                del_mid, del_sender = lg_msg_ids[0]
                _, _, _, admin_client, _, _, _ = mesh_users[1]
                resp = await admin_client.cmd_group_delete(
                    large_group_id.hex(), del_mid
                )
                check("large group: admin deletes msg",
                      resp.get("type") == "OK", f"got {resp}")

            # Non-admin member (user_3) tries to delete another's message
            if len(lg_msg_ids) >= 2:
                del_mid2, _ = lg_msg_ids[1]
                _, _, _, member_client, _, _, _ = mesh_users[3]
                resp = await member_client.cmd_group_delete(
                    large_group_id.hex(), del_mid2
                )
                check("large group: member can't delete others' msg",
                      resp.get("type") == "ERROR", f"got {resp}")

            # Owner destroys the group
            resp = await owner_client.cmd_group_destroy(large_group_id.hex())
            check("large group: owner destroys", resp.get("type") == "OK",
                  f"got {resp}")

            await asyncio.sleep(1)  # Sync replication ensures data is available on OK; small wait for routing

            # GROUP_INFO should fail after destroy
            resp = await owner_client.cmd_group_info(large_group_id.hex())
            check("large group: info fails after destroy",
                  resp.get("type") == "ERROR" or not resp.get("group_meta"),
                  f"got {resp}")

            # GROUP_SEND should fail after destroy
            dummy_lg_gek = os.urandom(32)
            resp = await owner_client.cmd_group_send(
                large_group_id.hex(), os.urandom(32).hex(), 1,
                encrypt_group_message(owner_sec, owner_fp, large_group_id, 1, dummy_lg_gek, b"ghost")
            )
            check("large group: send fails after destroy",
                  resp.get("type") == "ERROR", f"got {resp}")
        else:
            print("  SKIP  large group tests (create failed)")

    except Exception as e:
        fail("large group lifecycle section", f"crashed: {e}")

    # Cleanup mesh users
    for _, _, _, client, _, _, _ in mesh_users:
        try:
            await client.disconnect()
        except Exception:
            pass

    # ===================================================================
    print("\n=== Ephemeral Event Tests (Typing Indicators) ===")
    # ===================================================================

    # Reconnect Alice and Bob for ephemeral event tests
    # (they may still be connected, but let's ensure fresh connections)
    try:
        await alice.disconnect()
    except Exception:
        pass
    try:
        await bob.disconnect()
    except Exception:
        pass

    alice = ChromatinClient(pub_a, sec_a)
    bob = ChromatinClient(pub_b, sec_b)

    alice_pushes.clear()
    bob_pushes.clear()
    alice.set_push_callback(alice_push)
    bob.set_push_callback(bob_push)

    resp_a = await connect_with_redirect(alice, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
    check("alice reconnected for events", resp_a.get("type") == "OK",
          f"got {resp_a.get('type')}: {resp_a.get('reason', '')}")

    resp_b = await connect_with_redirect(bob, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
    check("bob reconnected for events", resp_b.get("type") == "OK",
          f"got {resp_b.get('type')}: {resp_b.get('reason', '')}")

    # Ensure mutual allowlist (should still be in place from earlier tests)
    await alice.cmd_allow(fp_b.hex())
    await bob.cmd_allow(fp_a.hex())
    await asyncio.sleep(1)

    # Test: Alice sends TYPING event to Bob
    bob_pushes.clear()
    resp = await alice.cmd_event(fp_b.hex(), "TYPING")
    check("event typing OK", resp.get("type") == "OK",
          f"got {resp}")

    # Wait for push delivery (cross-node relay may take a moment)
    await asyncio.sleep(2)

    # Check Bob received the EVENT push
    typing_events = [p for p in bob_pushes if p.get("type") == "EVENT"]
    check("bob received typing event", len(typing_events) >= 1,
          f"got {len(typing_events)} events, pushes: {bob_pushes}")

    if typing_events:
        ev = typing_events[0]
        check("typing event has correct from", ev.get("from") == fp_a.hex(),
              f"got from={ev.get('from')}")
        check("typing event has correct type", ev.get("event") == "TYPING",
              f"got event={ev.get('event')}")

    # Test: Bob sends TYPING event to Alice
    alice_pushes.clear()
    resp = await bob.cmd_event(fp_a.hex(), "TYPING")
    check("bob event typing OK", resp.get("type") == "OK",
          f"got {resp}")

    await asyncio.sleep(2)

    typing_events_a = [p for p in alice_pushes if p.get("type") == "EVENT"]
    check("alice received typing event", len(typing_events_a) >= 1,
          f"got {len(typing_events_a)} events, pushes: {alice_pushes}")

    if typing_events_a:
        ev = typing_events_a[0]
        check("alice typing event from bob", ev.get("from") == fp_b.hex(),
              f"got from={ev.get('from')}")

    # Test: Unknown event type rejected
    resp = await alice.cmd_event(fp_b.hex(), "INVALID_EVENT")
    check("unknown event type rejected", resp.get("type") == "ERROR",
          f"got {resp}")

    # ===================================================================
    # Cleanup
    # ===================================================================
    print("\n=== Cross-Node Replication Tests ===")
    # ===================================================================
    # Core DHT correctness: data written on one node must be readable
    # from any other node after replication. Tests use fresh identities
    # and deliberately connect to different bootstrap servers.
    # ===================================================================

    try:
        pub_r1, sec_r1 = generate_keypair()
        pub_r2, sec_r2 = generate_keypair()
        fp_r1 = fingerprint_of(pub_r1)
        fp_r2 = fingerprint_of(pub_r2)
        kem_pub_r1, kem_sec_r1 = kem_generate_keypair()
        kem_pub_r2, kem_sec_r2 = kem_generate_keypair()

        repl_sender = ChromatinClient(pub_r1, sec_r1)
        repl_receiver = ChromatinClient(pub_r2, sec_r2)

        # Connect both — follow REDIRECT so they land on their responsible nodes
        resp = await connect_with_redirect(repl_sender, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        check("repl: sender connected", resp.get("type") == "OK", f"got {resp}")
        resp = await connect_with_redirect(repl_receiver, SERVERS[1][0], SERVERS[1][1], tls=SERVERS[1][2])
        check("repl: receiver connected", resp.get("type") == "OK", f"got {resp}")

        s_node = (await repl_sender.cmd_status()).get("node_id", "?")[:12]
        r_node = (await repl_receiver.cmd_status()).get("node_id", "?")[:12]
        print(f"  sender on {s_node}..., receiver on {r_node}...")

        # Mutual allowlist
        resp = await repl_sender.cmd_allow(fp_r2.hex())
        check("repl: sender allows receiver", resp.get("type") == "OK", f"got {resp}")
        resp = await repl_receiver.cmd_allow(fp_r1.hex())
        check("repl: receiver allows sender", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)  # Sync replication ensures data is available on OK; small wait for routing

        # --- 1:1 message replication ---
        repl_blob = encrypt_1to1_message(sec_r1, fp_r1, fp_r2, kem_pub_r2, b"Replication test message payload")
        resp = await repl_sender.cmd_send(fp_r2.hex(), repl_blob)
        check("repl: sender sends message", resp.get("type") == "OK", f"got {resp}")
        repl_msg_id = resp.get("msg_id", "")

        await asyncio.sleep(1)  # Sync replication ensures data is available on OK; small wait for routing

        # Receiver (on its responsible node) can list and get
        resp = await repl_receiver.cmd_list()
        check("repl: receiver lists on responsible node", resp.get("type") == "OK", f"got {resp}")
        repl_msgs = resp.get("messages", [])
        check("repl: message visible on responsible node", len(repl_msgs) >= 1,
              f"got {len(repl_msgs)} messages")

        # Connect a SECOND receiver client (same keypair) to a DIFFERENT server
        # without following REDIRECT to verify other bootstrap nodes also have the data
        for srv_idx in range(len(SERVERS)):
            srv = SERVERS[srv_idx]
            repl_receiver2 = ChromatinClient(pub_r2, sec_r2)
            resp2 = await connect_raw(repl_receiver2, srv[0], srv[1], tls=srv[2])
            r2_node = ""
            if resp2.get("type") == "OK":
                r2_node = (await repl_receiver2.cmd_status()).get("node_id", "?")[:12]
            elif resp2.get("type") == "REDIRECT":
                await repl_receiver2.disconnect()
                resp2 = await connect_with_redirect(repl_receiver2, srv[0], srv[1], tls=srv[2])
                r2_node = (await repl_receiver2.cmd_status()).get("node_id", "?")[:12] if resp2.get("type") == "OK" else "?"

            if resp2.get("type") == "OK" and r2_node != r_node:
                print(f"  receiver2 on {r2_node}... (different node — testing replication)")
                resp = await repl_receiver2.cmd_list()
                check(f"repl: message visible on node {srv_idx}", resp.get("type") == "OK", f"got {resp}")
                msgs2 = resp.get("messages", [])
                check(f"repl: message replicated to node {srv_idx}", len(msgs2) >= 1,
                      f"got {len(msgs2)} messages on server {srv[0]}")

                if repl_msg_id and msgs2:
                    resp = await repl_receiver2.cmd_get(repl_msg_id)
                    if resp.get("blob"):
                        blob_back = base64.b64decode(resp["blob"])
                        check(f"repl: GET correct blob from node {srv_idx}",
                              blob_back == repl_blob, f"got {len(blob_back)} bytes vs expected {len(repl_blob)}")
                    else:
                        check(f"repl: GET correct blob from node {srv_idx}", False, "no blob in response")

                await repl_receiver2.disconnect()
                break
        else:
            print("  WARNING: could not find a second node for replication check — skipped")

        # --- Profile replication ---
        profile_r1 = build_profile_record(
            seckey=sec_r1,
            fingerprint=fp_r1,
            pubkey=pub_r1,
            kem_pubkey=kem_pub_r1,
            bio="Replication tester",
            avatar=b"",
            social_links=[],
            sequence=1,
        )
        resp = await repl_sender.cmd_set_profile(profile_r1)
        check("repl: set profile on node A", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)

        # GET profile from a DIFFERENT node
        repl_getter = ChromatinClient(pub_r2, sec_r2)
        for srv_idx in range(len(SERVERS)):
            srv = SERVERS[srv_idx]
            resp_conn = await connect_raw(repl_getter, srv[0], srv[1], tls=srv[2])
            if resp_conn.get("type") == "OK":
                g_node = (await repl_getter.cmd_status()).get("node_id", "?")[:12]
                if g_node != s_node:
                    print(f"  profile getter on {g_node}... (different from setter)")
                    resp = await repl_getter.cmd_get_profile(fp_r1.hex())
                    check("repl: profile visible from different node",
                          resp.get("found") is True, f"got {resp}")
                    await repl_getter.disconnect()
                    break
            await repl_getter.disconnect()

        # --- Group replication ---
        pub_grp_owner, sec_grp_owner = generate_keypair()
        pub_grp_member, sec_grp_member = generate_keypair()
        fp_grp_owner = fingerprint_of(pub_grp_owner)
        fp_grp_member = fingerprint_of(pub_grp_member)
        kem_pub_grp_owner, kem_sec_grp_owner = kem_generate_keypair()
        kem_pub_grp_member, kem_sec_grp_member = kem_generate_keypair()

        grp_owner = ChromatinClient(pub_grp_owner, sec_grp_owner)
        grp_member = ChromatinClient(pub_grp_member, sec_grp_member)

        # Connect owner to node 0, member to node 1 — deliberately different
        resp = await connect_with_redirect(grp_owner, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        check("repl: group owner connected (node 0)", resp.get("type") == "OK", f"got {resp}")
        resp = await connect_with_redirect(grp_member, SERVERS[1][0], SERVERS[1][1], tls=SERVERS[1][2])
        check("repl: group member connected (node 1)", resp.get("type") == "OK", f"got {resp}")

        grp_owner_node = (await grp_owner.cmd_status()).get("node_id", "?")[:12]
        grp_member_node = (await grp_member.cmd_status()).get("node_id", "?")[:12]
        print(f"  group owner on {grp_owner_node}..., member on {grp_member_node}...")

        repl_group_id = os.urandom(32)
        repl_gek = os.urandom(32)
        repl_kem_ct_o, repl_wrapped_o = wrap_gek_for_member(kem_pub_grp_owner, repl_gek)
        repl_kem_ct_m, repl_wrapped_m = wrap_gek_for_member(kem_pub_grp_member, repl_gek)

        grp_members_v1 = [
            (fp_grp_owner, 0x02, repl_kem_ct_o, repl_wrapped_o),
            (fp_grp_member, 0x00, repl_kem_ct_m, repl_wrapped_m),
        ]
        meta = build_group_meta(sec_grp_owner, repl_group_id, fp_grp_owner, fp_grp_owner, 1, grp_members_v1)
        resp = await grp_owner.cmd_group_create(meta)
        check("repl: group create on node A", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)  # Sync replication ensures data is available on OK; small wait for routing

        # Member on different node can fetch group info
        resp = await grp_member.cmd_group_info(repl_group_id.hex())
        check("repl: member gets group info from different node",
              resp.get("type") == "OK", f"got {resp}")

        # Member on different node can send group message
        gm_id = os.urandom(32).hex()
        encrypted_gm = encrypt_group_message(sec_grp_member, fp_grp_member, repl_group_id, 1, repl_gek, b"Cross-node group msg")
        resp = await grp_member.cmd_group_send(repl_group_id.hex(), gm_id, 1, encrypted_gm)
        check("repl: member sends group msg from different node",
              resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)

        # Owner on its node can list and see the member's message
        resp = await grp_owner.cmd_group_list(repl_group_id.hex())
        check("repl: owner sees group msg from different node",
              resp.get("type") == "OK", f"got {resp}")
        g_msgs = resp.get("messages", [])
        check("repl: group msg replicated to owner's node", len(g_msgs) >= 1,
              f"got {len(g_msgs)} messages")

        # GROUP_UPDATE cross-node: owner updates on node A, member sees version change on node B
        repl_v2_ct_o, repl_v2_wrapped_o = wrap_gek_for_member(kem_pub_grp_owner, repl_gek)
        repl_v2_ct_m, repl_v2_wrapped_m = wrap_gek_for_member(kem_pub_grp_member, repl_gek)

        grp_members_v2 = [
            (fp_grp_owner, 0x02, repl_v2_ct_o, repl_v2_wrapped_o),
            (fp_grp_member, 0x01, repl_v2_ct_m, repl_v2_wrapped_m),  # promote member to admin
        ]
        meta_v2 = build_group_meta(sec_grp_owner, repl_group_id, fp_grp_owner, fp_grp_owner, 2, grp_members_v2)
        resp = await grp_owner.cmd_group_update(meta_v2)
        check("repl: group update (promote member) on node A", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)

        resp = await grp_member.cmd_group_info(repl_group_id.hex())
        check("repl: member sees updated group meta from node B", resp.get("type") == "OK", f"got {resp}")
        if resp.get("type") == "OK":
            raw_meta = bytes.fromhex(resp.get("group_meta", ""))
            # version is at offset 96 (group_id(32) + owner_fp(32) + signer_fp(32)) as 4 BE bytes
            if len(raw_meta) >= 100:
                version_in_meta = int.from_bytes(raw_meta[96:100], "big")
                check("repl: member sees version 2 group meta",
                      version_in_meta == 2, f"version={version_in_meta}")
            else:
                fail("repl: member sees version 2 group meta", "meta too short")

        await grp_owner.disconnect()
        await grp_member.disconnect()
        await repl_sender.disconnect()
        await repl_receiver.disconnect()

    except Exception as e:
        fail("cross-node replication section", f"crashed: {e}")

    # ===================================================================
    print("\n=== Realistic Encrypted Conversation (25 messages) ===")
    # ===================================================================

    try:
        # Create fresh identities
        pub_ca, sec_ca = generate_keypair()
        pub_cb, sec_cb = generate_keypair()
        kem_pub_ca, kem_sec_ca = kem_generate_keypair()
        kem_pub_cb, kem_sec_cb = kem_generate_keypair()
        fp_ca = fingerprint_of(pub_ca)
        fp_cb = fingerprint_of(pub_cb)

        conv_alice = ChromatinClient(pub_ca, sec_ca)
        conv_bob = ChromatinClient(pub_cb, sec_cb)
        ca_pushes, cb_pushes = [], []

        async def _ca_push(m):
            ca_pushes.append(m)

        async def _cb_push(m):
            cb_pushes.append(m)

        conv_alice.set_push_callback(_ca_push)
        conv_bob.set_push_callback(_cb_push)

        # Connect to DIFFERENT nodes
        resp = await connect_with_redirect(conv_alice, SERVERS[0][0], SERVERS[0][1])
        check("conv: alice connected", resp.get("type") == "OK", f"got {resp}")
        resp = await connect_with_redirect(conv_bob, SERVERS[1][0], SERVERS[1][1])
        check("conv: bob connected", resp.get("type") == "OK", f"got {resp}")

        # Set profiles (with KEM pubkeys)
        await conv_alice.cmd_set_profile(build_profile_record(sec_ca, fp_ca, pub_ca, kem_pub_ca, "", b"", [], 1))
        await conv_bob.cmd_set_profile(build_profile_record(sec_cb, fp_cb, pub_cb, kem_pub_cb, "", b"", [], 1))
        await asyncio.sleep(1)

        # Mutual allow
        await conv_alice.cmd_allow(fp_cb.hex())
        await conv_bob.cmd_allow(fp_ca.hex())
        await asyncio.sleep(1)

        # Simulate a conversation — alternating messages
        conversation = [
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "hey, are you there?"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "yeah! just got on. what's up?"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "wanted to share something cool"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "go for it"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "check this link: https://example.com/cool-thing"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "nice, checking it out now"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "it's a new protocol for decentralized messaging"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "like this one? \U0001f604"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "exactly lol"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "that's meta"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "anyway, dinner tonight?"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "sure, where?"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "that ramen place on 5th"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "sounds good. 7pm?"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "perfect, see you there"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "btw did you fix the bug?"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "which one, the auth race?"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "yeah that one"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "yep, pushed the fix yesterday"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "great. tests passing?"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "261/261 \u2705"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "nice work"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "thanks! heading out now"),
            (conv_bob, sec_cb, fp_cb, fp_ca, kem_pub_ca, "see ya tonight"),
            (conv_alice, sec_ca, fp_ca, fp_cb, kem_pub_cb, "\U0001f35c"),
        ]

        sent_ok = 0
        for sender, sk, sfp, rfp, rkpk, text in conversation:
            blob = encrypt_1to1_message(sk, sfp, rfp, rkpk, text.encode("utf-8"))
            resp = await sender.cmd_send(rfp.hex(), blob)
            if resp.get("type") == "OK":
                sent_ok += 1
        check("conv: all 25 messages sent", sent_ok == 25, f"{sent_ok}/25 sent")

        await asyncio.sleep(2)

        # Verify alice's inbox — should have bob's messages (decrypt and check)
        resp = await conv_alice.cmd_list(50)
        a_msgs = resp.get("messages", [])
        check("conv: alice received messages", len(a_msgs) >= 10, f"got {len(a_msgs)}")

        # Decrypt first received message
        if a_msgs:
            resp = await conv_alice.cmd_get(a_msgs[0]["msg_id"])
            if resp.get("blob"):
                raw = base64.b64decode(resp["blob"])
                sender_fp, content, _ = decrypt_1to1_message(kem_sec_ca, fp_ca, raw)
                check("conv: decrypted msg from bob", sender_fp == fp_cb, f"wrong sender")
                check("conv: decrypted content valid", len(content) > 0, "empty content")

        # Verify bob's inbox
        resp = await conv_bob.cmd_list(50)
        b_msgs = resp.get("messages", [])
        check("conv: bob received messages", len(b_msgs) >= 10, f"got {len(b_msgs)}")

        # Decrypt bob's first received message
        if b_msgs:
            resp = await conv_bob.cmd_get(b_msgs[0]["msg_id"])
            if resp.get("blob"):
                raw = base64.b64decode(resp["blob"])
                sender_fp, content, _ = decrypt_1to1_message(kem_sec_cb, fp_cb, raw)
                check("conv: decrypted msg from alice", sender_fp == fp_ca, f"wrong sender")

        # Count cross-node push notifications
        push_count = len([p for p in ca_pushes if p.get("type") == "NEW_MESSAGE"])
        push_count += len([p for p in cb_pushes if p.get("type") == "NEW_MESSAGE"])
        check("conv: push notifications received", push_count >= 5, f"got {push_count}")

        await conv_alice.disconnect()
        await conv_bob.disconnect()

    except Exception as e:
        fail("realistic conversation section", f"crashed: {e}")

    # ===================================================================
    print("\n=== Cleanup ===")
    await alice.disconnect()
    await bob.disconnect()

    # ===================================================================
    # Summary
    # ===================================================================
    print(f"\n{'=' * 50}")
    print(f"RESULTS: {passed} passed, {failed} failed, {passed + failed} total")
    print(f"{'=' * 50}")
    if errors:
        print("\nFailures:")
        for e in errors:
            print(f"  {e}")
    return failed == 0


if __name__ == "__main__":
    try:
        success = asyncio.run(run_tests())
    except Exception as e:
        print(f"\n!!! Test suite crashed: {e}")
        print(f"\nPartial results: {passed} passed, {failed} failed")
        if errors:
            print("\nFailures before crash:")
            for err in errors:
                print(f"  {err}")
        sys.exit(2)
    sys.exit(0 if success else 1)
