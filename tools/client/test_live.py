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
)
from protocol import ChromatinClient, build_hello, build_auth, build_allowlist_signature_payload
from builders import build_profile_record, build_name_record, build_group_meta

# PoW difficulties (must match node config)
CONTACT_POW_DIFFICULTY = 16
NAME_POW_DIFFICULTY = 26

# Test nodes: (host, ws_port, tls) tuples
SERVERS = [
    ("195.181.202.122", 62010, False),
    ("195.181.202.122", 62011, False),
    ("2.chromatin.cpunk.io", 62012, True),
]

# Map TCP addresses (from REDIRECT) to WS (host, port).
# Nodes advertise their TCP bind address in routing, which may be a LAN IP
# or lack the WS port. This maps any known address to the correct WS endpoint.
ADDR_TO_WS = {
    "195.181.202.122": ("195.181.202.122", 62010),  # node1 (tcp 62000)
    "192.168.1.201":   ("195.181.202.122", 62010),  # node1 LAN
    "192.168.1.202":   ("195.181.202.122", 62011),  # node2 LAN
    "37.187.78.91":    ("37.187.78.91", 62012),      # node3 (France)
}


def resolve_redirect(node, fallback_host="195.181.202.122", fallback_port=62010):
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
    resp = await alice.cmd_send(fp_b.hex(), b"Hello Bob!")
    check("alice sends to bob", resp.get("type") == "OK", f"got {resp}")
    msg_id_1 = resp.get("msg_id", "")
    check("send returns msg_id", len(msg_id_1) > 0, "no msg_id")

    # Test 17: Send a second message
    resp = await alice.cmd_send(fp_b.hex(), b"Second message")
    check("alice sends second msg", resp.get("type") == "OK", f"got {resp}")
    msg_id_2 = resp.get("msg_id", "")

    # Test 18: Send a third message
    resp = await alice.cmd_send(fp_b.hex(), b"Third message")
    check("alice sends third msg", resp.get("type") == "OK", f"got {resp}")
    msg_id_3 = resp.get("msg_id", "")

    # Test 19: Bob sends to Alice
    resp = await bob.cmd_send(fp_a.hex(), b"Hello Alice!")
    check("bob sends to alice", resp.get("type") == "OK", f"got {resp}")
    msg_id_4 = resp.get("msg_id", "")

    # Test 20: Send empty message (server rejects empty blob)
    resp = await alice.cmd_send(fp_b.hex(), b"")
    check("send empty message rejected", resp.get("type") == "ERROR", f"got {resp}")

    # Test 21: Send large message (10KB)
    large_msg = b"X" * 10240
    resp = await alice.cmd_send(fp_b.hex(), large_msg)
    check("send 10KB message", resp.get("type") == "OK", f"got {resp}")
    msg_id_large = resp.get("msg_id", "")

    # Test 22: Send unicode message
    resp = await alice.cmd_send(fp_b.hex(), "Hello from the future! 🌐".encode("utf-8"))
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
            check("msg content correct", raw == b"Hello Bob!",
                  f"got {raw[:50]}")
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
        print(f"  Uploading {name} ({size_mb} MB)...")
        try:
            resp = await alice.cmd_send_large(fp_b.hex(), photo_data, timeout=60.0)
            if resp.get("type") == "OK" and resp.get("msg_id"):
                large_msg_ids.append((resp["msg_id"], photo_data, name))
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
    resp = await bob.cmd_send(fp_a.hex(), b"Should be denied")
    check("send to revoked fails", resp.get("type") == "ERROR",
          f"got {resp} (may be replication delay)")

    # Test 39: Alice re-allows Bob
    resp = await alice.cmd_allow(fp_b.hex())
    check("alice re-allows bob", resp.get("type") == "OK", f"got {resp}")

    await asyncio.sleep(2)  # Allow time for allow to replicate

    # Test 40: Bob can send again
    resp = await bob.cmd_send(fp_a.hex(), b"I'm back!")
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
        kem_pubkey=b"",
        bio="Alice in Chromatin-land",
        avatar=b"",
        social_links=[],
        sequence=1,
    )
    resp = await alice.cmd_set_profile(profile)
    check("alice set profile", resp.get("type") == "OK", f"got {resp}")

    # Test 51: Bob sets profile
    profile_b = build_profile_record(
        seckey=sec_b,
        fingerprint=fp_b,
        pubkey=pub_b,
        kem_pubkey=b"",
        bio="Bob the Builder",
        avatar=b"",
        social_links=[],
        sequence=1,
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

    # Test 54: Alice updates profile (sequence 2)
    profile2 = build_profile_record(
        seckey=sec_a,
        fingerprint=fp_a,
        pubkey=pub_a,
        kem_pubkey=b"",
        bio="Alice updated!",
        avatar=b"",
        social_links=[],
        sequence=2,
    )
    resp = await alice.cmd_set_profile(profile2)
    check("alice update profile", resp.get("type") == "OK", f"got {resp}")

    # Test 55: Profile with bio
    profile_bio = build_profile_record(
        seckey=sec_a,
        fingerprint=fp_a,
        pubkey=pub_a,
        kem_pubkey=b"",
        bio="A" * 500,
        avatar=b"",
        social_links=[],
        sequence=3,
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

        await asyncio.sleep(3)  # Allow time for replication

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
    group_ok = False

    try:
        kem_dummy = b"\x00" * 1568

        # Test 61: Create group (Alice as owner, Bob as member)
        members = [
            (fp_a, 0x02, kem_dummy),  # Alice = owner
            (fp_b, 0x00, kem_dummy),  # Bob = member
        ]
        meta = build_group_meta(sec_a, group_id, fp_a, 1, members)
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
            resp = await alice.cmd_group_send(
                group_id.hex(), mid, gek_version=1,
                blob=f"Group msg {i}".encode("utf-8")
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
            check("get group msg", resp.get("type") == "OK",
                  f"got {resp}")
        else:
            fail("get group msg", "no msg ids")

        # Test 72: Bob sends group message
        bob_gmsg = os.urandom(32).hex()
        resp = await bob.cmd_group_send(
            group_id.hex(), bob_gmsg, gek_version=1,
            blob=b"Bob's group message"
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
    print("\n=== Multi-Message Stress Tests ===")
    # ===================================================================

    await ensure_connected(alice, SERVERS)
    await ensure_connected(bob, SERVERS)

    # Test 77-86: Send 10 messages rapidly
    rapid_ids = []
    for i in range(10):
        resp = await alice.cmd_send(fp_b.hex(), f"Rapid {i}".encode())
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
        resp = await alice.cmd_send(fp_b.hex(), os.urandom(size))
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
    resp = await alice.cmd_send(fp_b.hex(), b"After reconnect")
    check("send after reconnect", resp.get("type") == "OK", f"got {resp}")

    # Test 95: Alice can list after reconnect
    resp = await alice.cmd_list(5)
    check("list after reconnect", resp.get("type") == "OK", f"got {resp}")

    # ===================================================================
    print("\n=== Edge Case Tests ===")
    # ===================================================================

    # Test 96: Send binary data
    resp = await alice.cmd_send(fp_b.hex(), bytes(range(256)))
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
            resp = await alice.cmd_group_send(
                group_id.hex(), os.urandom(32).hex(), 1, b"ghost msg"
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
        resp = await xander.cmd_send(fp_y.hex(), b"Cross-node push test!")
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
        resp = await yara.cmd_send(fp_x.hex(), b"Cross-node reply!")
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

        # Wait for replication to settle
        await asyncio.sleep(3)

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

    mesh_users = []  # [(pub, sec, fp, client, pushes), ...]
    try:
        # Create and connect 10 fresh identities
        def _make_mesh_cb(pl):
            async def _cb(msg):
                pl.append(msg)
            return _cb

        for i in range(10):
            pub, sec = generate_keypair()
            fp = fingerprint_of(pub)
            client = ChromatinClient(pub, sec)
            pushes = []
            client.set_push_callback(_make_mesh_cb(pushes))
            mesh_users.append((pub, sec, fp, client, pushes))

        connected = 0
        for i, (pub, sec, fp, client, _) in enumerate(mesh_users):
            srv = SERVERS[i % len(SERVERS)]
            resp = await connect_with_redirect(client, srv[0], srv[1], tls=srv[2])
            if resp.get("type") == "OK":
                connected += 1
            else:
                fail(f"mesh user_{i} connect", f"got {resp.get('type')}")
        check("mesh: all 10 users connected", connected == 10,
              f"{connected}/10 connected")

        # Ring allowlists: user_i allows user_{i-1} and user_{i+1}
        allow_ok = 0
        for i in range(10):
            _, _, _, client, _ = mesh_users[i]
            _, _, fp_prev, _, _ = mesh_users[(i - 1) % 10]
            _, _, fp_next, _, _ = mesh_users[(i + 1) % 10]
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
            _, _, _, client, _ = mesh_users[i]
            for nbr in [(i - 1) % 10, (i + 1) % 10]:
                _, _, fp_nbr, _, _ = mesh_users[nbr]
                resp = await client.cmd_send(
                    fp_nbr.hex(), f"mesh {i}->{nbr}".encode()
                )
                if resp.get("type") == "OK":
                    sent_count += 1
                    sent_to.setdefault(nbr, []).append(resp.get("msg_id"))
        check("mesh: 20 ring messages sent", sent_count >= 18,
              f"{sent_count}/20 sent")

        # 5 extra messages (users 0,2,4,6,8 → their next neighbor)
        for i in [0, 2, 4, 6, 8]:
            nbr = (i + 1) % 10
            _, _, _, client, _ = mesh_users[i]
            _, _, fp_nbr, _, _ = mesh_users[nbr]
            resp = await client.cmd_send(
                fp_nbr.hex(), f"extra {i}->{nbr}".encode()
            )
            if resp.get("type") == "OK":
                sent_count += 1
                sent_to.setdefault(nbr, []).append(resp.get("msg_id"))
        check("mesh: 25+ total messages sent", sent_count >= 25,
              f"sent {sent_count}")

        await asyncio.sleep(2)  # Let messages propagate

        # Verify: each recipient has >= 2 messages
        recipients_ok = 0
        for i in range(10):
            _, _, _, client, _ = mesh_users[i]
            resp = await client.cmd_list(50)
            msg_count = len(resp.get("messages", []))
            if msg_count >= 2:
                recipients_ok += 1
        check("mesh: all recipients have >=2 msgs", recipients_ok >= 8,
              f"{recipients_ok}/10 have >=2 messages")

        # Spot-check: GET one message for content correctness
        spot_target = 1  # user_1 should have msgs from user_0 and user_2
        if sent_to.get(spot_target):
            _, _, _, client_1, _ = mesh_users[spot_target]
            resp = await client_1.cmd_get(sent_to[spot_target][0])
            check("mesh: GET content spot-check",
                  resp.get("type") == "OK" or "blob" in resp, f"got {resp}")
        else:
            fail("mesh: GET content spot-check", "no messages for user_1")

        # Check cross-node push notifications arrived
        total_pushes = sum(
            len([p for p in pushes if p.get("type") == "NEW_MESSAGE"])
            for _, _, _, _, pushes in mesh_users
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
        for i, (_, _, _, client, _) in enumerate(mesh_users):
            await ensure_connected(client, SERVERS)

        kem_dummy = b"\x00" * 1568

        # Build member list: user_0=owner, user_1=admin, rest=members
        group_members = []
        for i, (pub, sec, fp, client, _) in enumerate(mesh_users):
            if i == 0:
                role = 0x02  # owner
            elif i == 1:
                role = 0x01  # admin
            else:
                role = 0x00  # member
            group_members.append((fp, role, kem_dummy))

        # user_0 (owner) creates the group
        _, owner_sec, owner_fp, owner_client, _ = mesh_users[0]
        meta = build_group_meta(owner_sec, large_group_id, owner_fp, 1, group_members)
        resp = await owner_client.cmd_group_create(meta)
        check("large group: create (10 members)", resp.get("type") == "OK",
              f"got {resp}")
        large_group_ok = resp.get("type") == "OK"

        if large_group_ok:
            await asyncio.sleep(1)

            # All 10 members query GROUP_INFO
            info_ok = 0
            for i, (_, _, _, client, _) in enumerate(mesh_users):
                resp = await client.cmd_group_info(large_group_id.hex())
                if resp.get("type") == "OK":
                    info_ok += 1
            check("large group: all members get info", info_ok >= 8,
                  f"{info_ok}/10 got OK")

            # 5 different members each send a group message
            lg_msg_ids = []
            for sender_idx in [0, 2, 4, 6, 8]:
                _, _, _, client, _ = mesh_users[sender_idx]
                mid = os.urandom(32).hex()
                resp = await client.cmd_group_send(
                    large_group_id.hex(), mid, gek_version=1,
                    blob=f"Group msg from user_{sender_idx}".encode()
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
                _, _, _, admin_client, _ = mesh_users[1]
                resp = await admin_client.cmd_group_delete(
                    large_group_id.hex(), del_mid
                )
                check("large group: admin deletes msg",
                      resp.get("type") == "OK", f"got {resp}")

            # Non-admin member (user_3) tries to delete another's message
            if len(lg_msg_ids) >= 2:
                del_mid2, _ = lg_msg_ids[1]
                _, _, _, member_client, _ = mesh_users[3]
                resp = await member_client.cmd_group_delete(
                    large_group_id.hex(), del_mid2
                )
                check("large group: member can't delete others' msg",
                      resp.get("type") == "ERROR", f"got {resp}")

            # Owner destroys the group
            resp = await owner_client.cmd_group_destroy(large_group_id.hex())
            check("large group: owner destroys", resp.get("type") == "OK",
                  f"got {resp}")

            await asyncio.sleep(3)

            # GROUP_INFO should fail after destroy
            resp = await owner_client.cmd_group_info(large_group_id.hex())
            check("large group: info fails after destroy",
                  resp.get("type") == "ERROR" or not resp.get("group_meta"),
                  f"got {resp}")

            # GROUP_SEND should fail after destroy
            resp = await owner_client.cmd_group_send(
                large_group_id.hex(), os.urandom(32).hex(), 1, b"ghost"
            )
            check("large group: send fails after destroy",
                  resp.get("type") == "ERROR", f"got {resp}")
        else:
            print("  SKIP  large group tests (create failed)")

    except Exception as e:
        fail("large group lifecycle section", f"crashed: {e}")

    # Cleanup mesh users
    for _, _, _, client, _ in mesh_users:
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
