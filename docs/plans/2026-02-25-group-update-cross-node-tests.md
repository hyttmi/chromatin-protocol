# GROUP_UPDATE + Cross-Node Replication Tests Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add GROUP_UPDATE support to the Python client and a dedicated cross-node replication test section that verifies data written on one node is readable from another — the core DHT correctness guarantee.

**Architecture:** `cmd_group_update` is wire-identical to `cmd_group_create` (same GROUP_META binary, same `build_group_meta` builder, just `GROUP_UPDATE` message type). Cross-node tests reuse the same keypair but connect two separate client objects to DIFFERENT bootstrap servers to prove replication. With R=min(3, N) and 3 known bootstrap nodes, all 3 hold all responsible data, so any bootstrap can serve any key after replication propagates.

**Tech Stack:** Python asyncio, `tools/client/protocol.py`, `tools/client/builders.py`, `tools/client/test_live.py`. Live test network: `SERVERS = [(0.bootstrap.pqcc.fi, 62010), (1.bootstrap.pqcc.fi, 62011), (2.bootstrap.pqcc.fi, 62012)]`.

---

## Task 1: Add `cmd_group_update` to the Python client

**Files:**
- Modify: `tools/client/protocol.py` — add method after `cmd_group_create` (line ~228)

`GROUP_UPDATE` has the exact same wire format as `GROUP_CREATE`. The only difference is the message `type` field. The GROUP_META binary is built the same way with `build_group_meta` (new version number, updated member list).

**Step 1: Add the method**

Insert after `cmd_group_create`:

```python
async def cmd_group_update(self, group_meta: bytes) -> dict:
    mid = self._msg_id()
    return await self.send_command({
        "type": "GROUP_UPDATE", "id": mid,
        "group_meta": group_meta.hex(),
    }, timeout=30.0)
```

**Step 2: Verify it's importable**

```bash
cd tools/client && python -c "from protocol import ChromatinClient; print('ok')"
```
Expected: `ok`

**Step 3: Commit**

```bash
git add tools/client/protocol.py
git commit -m "feat(client): add cmd_group_update to Python client"
```

---

## Task 2: GROUP_UPDATE tests

**Files:**
- Modify: `tools/client/test_live.py` — add new section after existing Group Tests (after line ~713, before Multi-Message Stress Tests)

**Context:** Alice owns the group (role 0x02). Bob is a member (role 0x00). Tests use `kem_dummy = b"\x00" * 1568` like the existing group tests. `group_id` and `group_ok` from the existing Group Tests section are in scope (they're declared at the start of that try block — check that they're accessible; if not, use a new group_id).

**Step 1: Add the test section**

Add between the Group Tests section and Multi-Message Stress Tests:

```python
    # ===================================================================
    print("\n=== Group Update Tests ===")
    # ===================================================================

    try:
        upd_group_id = os.urandom(32)
        kem_dummy = b"\x00" * 1568

        # Create a fresh group for update tests (Alice owner, Bob member)
        members_v1 = [
            (fp_a, 0x02, kem_dummy),  # Alice = owner
            (fp_b, 0x00, kem_dummy),  # Bob = member
        ]
        meta_v1 = build_group_meta(sec_a, upd_group_id, fp_a, 1, members_v1)
        resp = await alice.cmd_group_create(meta_v1)
        check("group update: create base group", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(0.5)

        # Test: Bob can send to the group (he's a member)
        mid = os.urandom(32).hex()
        resp = await bob.cmd_group_send(upd_group_id.hex(), mid, gek_version=1, blob=b"Bob v1 msg")
        check("group update: bob sends before update", resp.get("type") == "OK", f"got {resp}")

        # Add a new member (Charlie) via GROUP_UPDATE (version 2)
        pub_c2, sec_c2 = generate_keypair()
        fp_c2 = fingerprint_of(pub_c2)
        charlie2 = ChromatinClient(pub_c2, sec_c2)
        resp_c2 = await connect_with_redirect(charlie2, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        check("group update: charlie2 connected", resp_c2.get("type") == "OK", f"got {resp_c2}")

        members_v2 = [
            (fp_a, 0x02, kem_dummy),   # Alice = owner
            (fp_b, 0x00, kem_dummy),   # Bob = member
            (fp_c2, 0x00, kem_dummy),  # Charlie2 = new member
        ]
        meta_v2 = build_group_meta(sec_a, upd_group_id, fp_a, 2, members_v2)
        resp = await alice.cmd_group_update(meta_v2)
        check("group update: add charlie2 (v2)", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)

        # New member can fetch group info
        resp = await charlie2.cmd_group_info(upd_group_id.hex())
        check("group update: new member gets group info", resp.get("type") == "OK", f"got {resp}")

        # New member can send to the group
        mid2 = os.urandom(32).hex()
        resp = await charlie2.cmd_group_send(upd_group_id.hex(), mid2, gek_version=2, blob=b"Charlie2 msg")
        check("group update: new member can send", resp.get("type") == "OK", f"got {resp}")

        # Remove Bob via GROUP_UPDATE (version 3) — owner-only
        members_v3 = [
            (fp_a, 0x02, kem_dummy),   # Alice = owner
            (fp_c2, 0x00, kem_dummy),  # Charlie2 = member
            # Bob removed
        ]
        meta_v3 = build_group_meta(sec_a, upd_group_id, fp_a, 3, members_v3)
        resp = await alice.cmd_group_update(meta_v3)
        check("group update: remove bob (v3)", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(1)

        # Removed member can no longer send to the group
        mid3 = os.urandom(32).hex()
        resp = await bob.cmd_group_send(upd_group_id.hex(), mid3, gek_version=3, blob=b"Bob after removal")
        check("group update: removed member can't send", resp.get("type") == "ERROR",
              f"got {resp} (bob should be rejected after removal)")

        # Replay old version (v1) must be rejected
        resp = await alice.cmd_group_update(meta_v1)
        check("group update: replay old version rejected", resp.get("type") == "ERROR",
              f"got {resp} (old version should be rejected)")

        # Non-member can't update the group
        meta_v4_bob = build_group_meta(sec_b, upd_group_id, fp_b, 4, members_v3)
        resp = await bob.cmd_group_update(meta_v4_bob)
        check("group update: non-member can't update", resp.get("type") == "ERROR",
              f"got {resp} (non-member update should be rejected)")

        await charlie2.disconnect()

    except Exception as e:
        fail("group update section", f"crashed: {e}")
```

**Step 2: Run just this section manually to eyeball it**

```bash
cd tools/client && python test_live.py 2>&1 | grep -A2 "Group Update Tests"
```
Expected: section runs, tests pass or show meaningful failures.

**Step 3: Commit**

```bash
git add tools/client/test_live.py
git commit -m "test(live): add GROUP_UPDATE tests (add/remove member, replay rejection)"
```

---

## Task 3: Cross-node replication test helper

**Files:**
- Modify: `tools/client/test_live.py` — add a helper near the top (after `ensure_connected`, around line ~122)

The key strategy: after an operation, connect a **second client with the same keypair** to a **different bootstrap server** (without following REDIRECT) and verify the same data is accessible. This proves replication happened.

`connect_to_specific` bypasses REDIRECT — it connects to the given server regardless. If the server returns REDIRECT, it's logged but not followed (this is a "raw" connection for replication probing).

**Step 1: Add helper**

After the `ensure_connected` function:

```python
async def connect_raw(client, host, port, tls=False):
    """Connect to a specific node without following REDIRECT.
    Returns the AUTH response (OK or REDIRECT — caller decides what to do with it).
    Used for cross-node replication tests: we deliberately connect to a node
    that may not be responsible for this client to prove replication works."""
    return await client.connect(host, port, tls=tls)
```

**Step 2: Verify no import issues**

```bash
cd tools/client && python -c "import test_live; print('ok')"
```

---

## Task 4: Cross-node replication test section

**Files:**
- Modify: `tools/client/test_live.py` — add after Ephemeral Event Tests, before the Cleanup section (around line ~1365)

This is the most important section. Strategy:

1. **1:1 messages**: Alice sends from node 0. Two separate "Bob" clients (same keypair) connect — one via REDIRECT (responsible node) and one to a DIFFERENT bootstrap server. Both must be able to LIST and GET the message.

2. **Profile**: SET_PROFILE on node 0, GET_PROFILE from node 1 and node 2.

3. **Name resolution**: REGISTER_NAME on node 0, RESOLVE_NAME from node 1 and node 2.

4. **Group cross-node**: GROUP_CREATE on node 0, GROUP_INFO from node 1, GROUP_SEND from member on node 2, GROUP_LIST from node 0 shows the message.

All operations use fresh identities to avoid state pollution from earlier tests.

**Step 1: Add the section**

Before the `# Cleanup` section:

```python
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

        await asyncio.sleep(2)  # Let allowlist replicate

        # --- 1:1 message replication ---
        repl_blob = b"Replication test message payload"
        resp = await repl_sender.cmd_send(fp_r2.hex(), repl_blob)
        check("repl: sender sends message", resp.get("type") == "OK", f"got {resp}")
        repl_msg_id = resp.get("msg_id", "")

        await asyncio.sleep(3)  # Let message replicate

        # Receiver (on its responsible node) can list and get
        resp = await repl_receiver.cmd_list()
        check("repl: receiver lists on responsible node", resp.get("type") == "OK", f"got {resp}")
        repl_msgs = resp.get("messages", [])
        check("repl: message visible on responsible node", len(repl_msgs) >= 1,
              f"got {len(repl_msgs)} messages")

        # Connect a SECOND receiver client (same keypair) to a DIFFERENT server
        # without following REDIRECT — this tests that the OTHER bootstrap nodes
        # also have the replicated data
        for srv_idx in range(len(SERVERS)):
            srv = SERVERS[srv_idx]
            # Try to find a server different from the one repl_receiver is on
            repl_receiver2 = ChromatinClient(pub_r2, sec_r2)
            resp2 = await connect_raw(repl_receiver2, srv[0], srv[1], tls=srv[2])
            r2_node = ""
            if resp2.get("type") == "OK":
                r2_node = (await repl_receiver2.cmd_status()).get("node_id", "?")[:12]
            elif resp2.get("type") == "REDIRECT":
                # Even if redirected, we follow this time just to connect somewhere
                await repl_receiver2.disconnect()
                resp2 = await connect_with_redirect(repl_receiver2, srv[0], srv[1], tls=srv[2])
                r2_node = (await repl_receiver2.cmd_status()).get("node_id", "?")[:12] if resp2.get("type") == "OK" else "?"

            if resp2.get("type") == "OK" and r2_node != r_node[:12]:
                print(f"  receiver2 on {r2_node}... (different node — testing replication)")
                resp = await repl_receiver2.cmd_list()
                check(f"repl: message visible on node {srv_idx}", resp.get("type") == "OK", f"got {resp}")
                msgs2 = resp.get("messages", [])
                check(f"repl: message replicated to node {srv_idx}", len(msgs2) >= 1,
                      f"got {len(msgs2)} messages on server {srv[0]}")

                if repl_msg_id and msgs2:
                    resp = await repl_receiver2.cmd_get(repl_msg_id)
                    blob_back = bytes.fromhex(resp.get("blob", "")) if resp.get("blob") else b""
                    check(f"repl: GET correct blob from node {srv_idx}",
                          blob_back == repl_blob, f"got {len(blob_back)} bytes")

                await repl_receiver2.disconnect()
                break
        else:
            print("  WARNING: could not find a second node for replication check — skipped")

        # --- Profile replication ---
        resp = await repl_sender.cmd_set_profile(
            build_profile(sec_r1, pub_r1, b"", bio="Replication tester", avatar=b"",
                          social_links=[], sequence=1)
        )
        check("repl: set profile on node A", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)

        # GET profile from a DIFFERENT node
        repl_getter = ChromatinClient(pub_r2, sec_r2)
        for srv_idx in range(len(SERVERS)):
            srv = SERVERS[srv_idx]
            resp_conn = await connect_raw(repl_getter, srv[0], srv[1], tls=srv[2])
            if resp_conn.get("type") == "OK":
                g_node = (await repl_getter.cmd_status()).get("node_id", "?")[:12]
                if g_node != s_node[:12]:
                    print(f"  profile getter on {g_node}... (different from setter)")
                    resp = await repl_getter.cmd_get_profile(fp_r1.hex())
                    check("repl: profile visible from different node",
                          resp.get("found") == True, f"got {resp}")
                    await repl_getter.disconnect()
                    break
            await repl_getter.disconnect()

        # --- Group replication ---
        pub_grp_owner, sec_grp_owner = generate_keypair()
        pub_grp_member, sec_grp_member = generate_keypair()
        fp_grp_owner = fingerprint_of(pub_grp_owner)
        fp_grp_member = fingerprint_of(pub_grp_member)

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
        kem_dummy = b"\x00" * 1568
        grp_members_v1 = [
            (fp_grp_owner, 0x02, kem_dummy),
            (fp_grp_member, 0x00, kem_dummy),
        ]
        meta = build_group_meta(sec_grp_owner, repl_group_id, fp_grp_owner, 1, grp_members_v1)
        resp = await grp_owner.cmd_group_create(meta)
        check("repl: group create on node A", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)  # Let GROUP_META replicate

        # Member on different node can fetch group info
        resp = await grp_member.cmd_group_info(repl_group_id.hex())
        check("repl: member gets group info from different node",
              resp.get("type") == "OK", f"got {resp}")

        # Member on different node can send group message
        gm_id = os.urandom(32).hex()
        resp = await grp_member.cmd_group_send(repl_group_id.hex(), gm_id, gek_version=1,
                                                blob=b"Cross-node group msg")
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

        # GROUP_UPDATE cross-node: owner updates group on node A, member sees it on node B
        grp_members_v2 = [
            (fp_grp_owner, 0x02, kem_dummy),
            (fp_grp_member, 0x01, kem_dummy),  # promote member to admin
        ]
        meta_v2 = build_group_meta(sec_grp_owner, repl_group_id, fp_grp_owner, 2, grp_members_v2)
        resp = await grp_owner.cmd_group_update(meta_v2)
        check("repl: group update (promote member) on node A", resp.get("type") == "OK", f"got {resp}")

        await asyncio.sleep(2)

        resp = await grp_member.cmd_group_info(repl_group_id.hex())
        check("repl: member sees updated group meta from node B", resp.get("type") == "OK", f"got {resp}")
        if resp.get("type") == "OK":
            raw_meta = bytes.fromhex(resp.get("group_meta", ""))
            # member_count(2 BE) is at offset 68 (32+32+4)
            # role is at offset 68+2+32 = 102 for first member, 68+2+32+1+1568+32 = ... check member list
            # Simple check: version 2 should be in the meta (bytes 64-68 BE)
            if len(raw_meta) >= 68:
                version_in_meta = int.from_bytes(raw_meta[64:68], "big")
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
```

**Note on imports:** `build_profile` — check if it's already imported at the top of `test_live.py`. If not, add it to the import from `builders`.

**Step 2: Check existing builders import**

```bash
grep "^from builders\|^import builders\|build_profile\|build_group_meta" tools/client/test_live.py | head -10
```

If `build_profile` isn't imported, add it to the builders import line.

**Step 3: Run and eyeball**

```bash
cd tools/client && python test_live.py 2>&1 | grep -A3 "Cross-Node Replication"
```

**Step 4: Commit**

```bash
git add tools/client/test_live.py
git commit -m "test(live): add cross-node replication tests

Tests that data written on one node is readable from another:
- 1:1 message replication (LIST + GET from different node)
- Profile replication (GET_PROFILE from different node)
- Group create/update/send cross-node (meta and messages replicate)"
```

---

## Task 5: Additional security/completeness tests

**Files:**
- Modify: `tools/client/test_live.py` — add to existing edge case section (~line 767)

**Step 1: Add allowlist sequence replay test**

After existing edge case tests:

```python
    # --- Allowlist sequence replay rejection ---
    try:
        pub_sq, sec_sq = generate_keypair()
        fp_sq = fingerprint_of(pub_sq)
        seq_client = ChromatinClient(pub_sq, sec_sq)
        resp = await connect_with_redirect(seq_client, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        if resp.get("type") == "OK":
            # Set allowlist sequence=1
            resp = await seq_client.cmd_allow(fp_a.hex())
            check("seq: allow seq=1", resp.get("type") == "OK", f"got {resp}")
            # Replay sequence=1 (should be rejected — same or lower sequence)
            resp = await seq_client.cmd_allow(fp_a.hex())  # seq=1 again — idempotent or rejected
            # Note: seq=1 replay: server may accept (idempotent) or reject. Document actual behavior.
            print(f"  seq replay result: {resp.get('type')} (idempotent or rejected both acceptable)")
            # REVOKE with seq=2, then try to re-ALLOW with seq=1 (lower — must be rejected)
            resp = await seq_client.cmd_revoke(fp_a.hex())
            check("seq: revoke seq=2", resp.get("type") == "OK", f"got {resp}")
            # Try to replay allow with seq=1 (lower than current seq=2) — MUST be rejected
            # We can't easily send a raw lower-sequence command via cmd_allow (it auto-increments)
            # so this is a note: the C++ tests cover this; Python client auto-increments
            print("  NOTE: sequence monotonicity enforced server-side; C++ tests cover raw replay")
            await seq_client.disconnect()
    except Exception as e:
        print(f"  seq replay section: {e}")

    # --- Large message NEW_MESSAGE push is metadata-only ---
    try:
        pub_lp1, sec_lp1 = generate_keypair()
        pub_lp2, sec_lp2 = generate_keypair()
        fp_lp1 = fingerprint_of(pub_lp1)
        fp_lp2 = fingerprint_of(pub_lp2)
        lp_sender = ChromatinClient(pub_lp1, sec_lp1)
        lp_receiver = ChromatinClient(pub_lp2, sec_lp2)
        lp_pushes = []
        lp_receiver.set_push_callback(lambda m: lp_pushes.append(m))

        resp = await connect_with_redirect(lp_sender, SERVERS[0][0], SERVERS[0][1], tls=SERVERS[0][2])
        resp2 = await connect_with_redirect(lp_receiver, SERVERS[1][0], SERVERS[1][1], tls=SERVERS[1][2])
        if resp.get("type") == "OK" and resp2.get("type") == "OK":
            await lp_sender.cmd_allow(fp_lp2.hex())
            await lp_receiver.cmd_allow(fp_lp1.hex())
            await asyncio.sleep(1)

            # Send large message (100 KB > 64 KB threshold)
            large_blob = os.urandom(100 * 1024)
            resp = await lp_sender.cmd_send_large(fp_lp2.hex(), large_blob)
            check("large msg push: send large ok", resp.get("type") == "OK", f"got {resp}")

            await asyncio.sleep(3)

            large_pushes = [p for p in lp_pushes if p.get("type") == "NEW_MESSAGE"]
            check("large msg push: receiver got NEW_MESSAGE", len(large_pushes) >= 1,
                  f"got {len(large_pushes)} pushes")
            if large_pushes:
                push = large_pushes[0]
                # Large message push must NOT contain inline blob
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
```

**Step 2: Run and verify**

```bash
cd tools/client && python test_live.py 2>&1 | grep -E "large msg push|seq:"
```

**Step 3: Commit**

```bash
git add tools/client/test_live.py
git commit -m "test(live): add large message push metadata-only verification

- Large (>64KB) NEW_MESSAGE push must not contain inline blob
- Adds notes on sequence replay (covered by C++ unit tests)"
```

---

## Notes

- **Replication delay:** All cross-node tests use `asyncio.sleep(2-3)` to allow SYNC_REQ/SYNC_RESP to propagate. If tests are flaky, increase delays.
- **Same-node case:** If `connect_with_redirect` happens to land all clients on the same node (possible if XOR distances align), the cross-node loop tries all 3 SERVERS to find a different one. If all 3 map to the same node, the check is skipped with a warning — this is expected behavior, not a failure.
- **GROUP_UPDATE wire format:** Identical to GROUP_CREATE. The node accepts it if the signature is valid and `version > stored_version`. Non-members and non-owners/admins are rejected.
- **cmd_allow auto-increments sequence:** The Python client's `cmd_allow`/`cmd_revoke` auto-increment the sequence counter on each call. Sequence replay testing at the wire level is covered by the C++ unit tests in `test_ws_server.cpp`.
