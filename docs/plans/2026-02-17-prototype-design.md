# Helix Prototype Design

> Python prototype to validate core protocol mechanics before C++ implementation.

## Purpose

Test the unified Kademlia + mdbx replication architecture in a Docker cluster
with 3-5 nodes. No real crypto — mock everything. Focus on data flow.

## In Scope

- Node IDs via SHA256 (mock for SHA3-256)
- XOR distance + responsibility computation
- Full membership (every node knows all nodes)
- Sequence-based replication log (sqlite)
- Inbox: STORE, FETCH, ACK/DELETE
- WebSocket server per node (client auth, SEND, ACK, NEW_MESSAGE push)
- Basic profile storage (fingerprint + name, no signatures)
- Docker Compose cluster (3 nodes)
- Test client CLI

## Out of Scope

- Real crypto (liboqs, ML-DSA, ML-KEM)
- Allowlists, contact requests, PoW
- Name registration
- Trust/reputation
- Compaction

## Structure

```
proto/
  node.py          — main entrypoint, UDP + WebSocket event loop
  kademlia.py      — XOR distance, node table, responsibility
  storage.py       — sqlite replication log
  replication.py   — sync protocol between nodes
  ws_server.py     — WebSocket client handler
  client.py        — test client CLI
  Dockerfile
  docker-compose.yml
  requirements.txt
```

## Key Flows to Validate

1. Node bootstrap: 3 nodes start, discover each other
2. Profile store: client publishes profile, stored on R responsible nodes
3. Message send: Alice → node → Bob's R responsible nodes → Bob
4. Delete-after-fetch: Bob ACKs → DEL replicates to all R nodes
5. Node crash + recovery: kill a node, restart, sync from peers
