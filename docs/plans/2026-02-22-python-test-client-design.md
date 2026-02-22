# Python Test Client — Design

**Goal:** Interactive REPL client for testing chromatin-node deployments.
Connect to a node, authenticate, and issue protocol commands manually.

## Architecture

```
tools/client/
  chromatin_client.py   -- Main entry point + REPL loop
  crypto_utils.py       -- ML-DSA-87 keygen/sign, SHA3-256, PoW mining
  protocol.py           -- WS connection, auth handshake, command serialization
  requirements.txt      -- websockets, liboqs-python
```

**Stack:** Python 3.10+, `websockets` (async WS), `liboqs-python` (ML-DSA-87),
`hashlib` (SHA3-256, stdlib).

**Async model:** Single `asyncio` event loop with two concurrent tasks:
1. REPL input reader — reads user commands, sends WS messages, prints responses
2. Push listener — receives server pushes, prints them inline

## Identity

On first run, generates ML-DSA-87 keypair and saves to `~/.chromatin/identity.key`.
Subsequent runs load from disk. Fingerprint = SHA3-256(pubkey).

## REPL Commands

```
connect <host> <ws_port>      -- connect + HELLO + AUTH
status                        -- node status
send <fingerprint> <text>     -- send message (auto base64)
list [limit] [after]          -- list inbox
get <msg_id>                  -- fetch message
delete <msg_id> [msg_id...]   -- delete messages
allow <fingerprint>           -- add to allowlist
revoke <fingerprint>          -- remove from allowlist
request <fingerprint> <text>  -- contact request (mines PoW)
list_requests                 -- list pending contact requests
register <name>               -- register name (mines PoW)
set_profile <bio>             -- set profile with bio text
resolve <name>                -- resolve name to fingerprint
get_profile <fingerprint>     -- get user profile
group_create <member_fp...>   -- create group (self as owner)
group_info <group_id>         -- get group metadata
group_send <group_id> <text>  -- send group message
group_list <group_id>         -- list group messages
group_get <group_id> <msg_id> -- get group message
group_delete <group_id> <msg_id> -- delete group message
group_destroy <group_id>      -- destroy group
identity                      -- show own fingerprint + pubkey
help                          -- show commands
quit                          -- disconnect and exit
```

## Push Notifications

Printed inline as they arrive, prefixed for visibility:

```
[PUSH] NEW_MESSAGE from a3b4c5... (1.2 KB)
[PUSH] CONTACT_REQUEST from d6e7f8...
[PUSH] NEW_GROUP_MESSAGE in group 1a2b3c...
```

## PoW Mining

Brute-force nonce search in Python:
- Contact request: 16 leading zero bits (~65K iterations, fast)
- Name registration: 28 leading zero bits (~268M iterations, minutes in Python)

## Scope Exclusions

- No chunked transfers (all messages <64 KB for now)
- No TLS/SSL WebSocket support (nodes tested without TLS)
- No multi-connection support (one node at a time)
- No persistent message storage on client side

## Dependencies

- `websockets` — async WebSocket client
- `liboqs-python` — ML-DSA-87 signatures (FIPS 204)
- Python 3.10+ stdlib: `hashlib` (SHA3-256), `asyncio`, `base64`, `json`, `struct`
