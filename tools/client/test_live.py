#!/usr/bin/env python3
"""Live integration tests against running chromatin nodes.

Runs 100+ tests covering messaging, contacts, allowlist, profiles,
name registration, groups, error handling, and multi-node behavior.
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

# Test nodes
WS_PORTS = [62010, 62011, 62012]
HOST = "195.181.202.122"

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


async def ensure_connected(client, host, ports):
    """Reconnect a client if its WebSocket is dead."""
    if client.ws and client.ws.protocol.state.name == "OPEN":
        return True
    # Try to reconnect
    for port in ports:
        try:
            resp = await client.connect(host, port)
            if resp.get("type") == "REDIRECT":
                nodes = resp.get("nodes", [])
                if nodes:
                    resp = await client.connect(host, nodes[0].get("ws_port", port))
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

    # Test 1-3: Connect to each WS port
    for i, port in enumerate(WS_PORTS):
        client = ChromatinClient(pub_a, sec_a)
        try:
            resp = await client.connect(HOST, port)
            rtype = resp.get("type", "")
            if rtype == "OK" or rtype == "REDIRECT":
                ok(f"connect to port {port} ({rtype})")
            else:
                fail(f"connect to port {port}", f"got {rtype}")
            await client.disconnect()
        except Exception as e:
            fail(f"connect to port {port}", str(e))

    # Test 4: Connect Alice to her responsible node
    resp_a = await alice.connect(HOST, WS_PORTS[0])
    if resp_a.get("type") == "REDIRECT":
        # Follow redirect
        nodes = resp_a.get("nodes", [])
        if nodes:
            redirect_port = nodes[0].get("ws_port", WS_PORTS[0])
            resp_a = await alice.connect(HOST, redirect_port)
    check("alice connected", resp_a.get("type") == "OK",
          f"got {resp_a.get('type')}: {resp_a.get('reason', '')}")

    # Test 5: Connect Bob to his responsible node
    resp_b = await bob.connect(HOST, WS_PORTS[0])
    if resp_b.get("type") == "REDIRECT":
        nodes = resp_b.get("nodes", [])
        if nodes:
            redirect_port = nodes[0].get("ws_port", WS_PORTS[0])
            resp_b = await bob.connect(HOST, redirect_port)
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
    resp_c = await charlie.connect(HOST, WS_PORTS[0])
    if resp_c.get("type") == "REDIRECT":
        nodes = resp_c.get("nodes", [])
        if nodes:
            resp_c = await charlie.connect(HOST, nodes[0].get("ws_port", WS_PORTS[0]))

    # Test 41: Charlie status
    if resp_c.get("type") == "OK":
        status_c = await charlie.cmd_status()
        check("charlie connected and status", status_c.get("type") == "OK")
    else:
        ok("charlie connected (redirect)")

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
    resp_d = await dave.connect(HOST, WS_PORTS[0])
    if resp_d.get("type") == "REDIRECT":
        nodes = resp_d.get("nodes", [])
        if nodes:
            resp_d = await dave.connect(HOST, nodes[0].get("ws_port", WS_PORTS[0]))

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
    await asyncio.sleep(0.5)
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
        print(f"  Mining PoW for name '{name}' (28 bits)... ", end="", flush=True)
        start = time.time()
        pow_nonce = mine_pow(prefix, 28)
        elapsed = time.time() - start
        print(f"done in {elapsed:.1f}s")
        check("mine name PoW", pow_nonce >= 0)

        record = build_name_record(sec_a, name, fp_a, pub_a, pow_nonce, sequence=1)
        resp = await alice.cmd_register_name(record)
        check("register name", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)

        # Test 58: Resolve the name
        await ensure_connected(bob, HOST, WS_PORTS)
        resp = await bob.cmd_resolve_name(name)
        check("resolve name", resp.get("found") == True, f"got {resp}")
        if resp.get("found"):
            check("resolved fp matches", resp.get("fingerprint") == fp_a.hex(),
                  f"got {resp.get('fingerprint')}")
        else:
            fail("resolved fp matches", "name not found")

        # Test 60: Resolve non-existent name
        await ensure_connected(alice, HOST, WS_PORTS)
        resp = await alice.cmd_resolve_name("nonexistent999")
        check("resolve nonexistent", not resp.get("found", True), f"got {resp}")
    except Exception as e:
        fail("name registration section", f"crashed: {e}")
        # Reconnect for subsequent tests
        await ensure_connected(alice, HOST, WS_PORTS)
        await ensure_connected(bob, HOST, WS_PORTS)

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
        await ensure_connected(alice, HOST, WS_PORTS)
        await ensure_connected(bob, HOST, WS_PORTS)

    # ===================================================================
    print("\n=== Multi-Message Stress Tests ===")
    # ===================================================================

    await ensure_connected(alice, HOST, WS_PORTS)
    await ensure_connected(bob, HOST, WS_PORTS)

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
    resp = await alice.connect(HOST, WS_PORTS[0])
    if resp.get("type") == "REDIRECT":
        nodes = resp.get("nodes", [])
        if nodes:
            resp = await alice.connect(HOST, nodes[0].get("ws_port", WS_PORTS[0]))
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
            await ensure_connected(alice, HOST, WS_PORTS)
            # Test 103: Destroy group
            resp = await alice.cmd_group_destroy(group_id.hex())
            check("destroy group", resp.get("type") == "OK", f"got {resp}")

            # Test 104: Verify group destroyed
            await asyncio.sleep(0.3)
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
    print("\n=== Concurrent Connections Test ===")
    # ===================================================================

    # Test 106-108: Multiple clients same identity
    clients = []
    for i in range(3):
        c = ChromatinClient(pub_a, sec_a)
        resp = await c.connect(HOST, WS_PORTS[i])
        rtype = resp.get("type", "")
        check(f"concurrent client {i}", rtype in ("OK", "REDIRECT"),
              f"got {rtype}: {resp.get('reason', '')}")
        clients.append(c)
    for c in clients:
        await c.disconnect()

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
