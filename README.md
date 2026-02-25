# Chromatin Protocol

> A decentralized, post-quantum-safe messaging protocol.  
> No central servers. No backdoors. No compromises.

**cpunk.io** — Censorship-resistant communication for everyone.

---

## What is Chromatin?

Chromatin is an open messaging protocol built on a custom Kademlia DHT. Messages
are stored and delivered by a network of independent nodes — anyone can run one.
There is no company, no central server, and nothing to shut down.

All cryptography is post-quantum from the ground up: **ML-DSA-87** for
signatures, **ML-KEM-1024** for key exchange, and **AES-256-GCM** for message
encryption. Classical algorithms (RSA, elliptic curve) are not used anywhere in
the protocol.

---

## Key Properties

- **Censorship-resistant** — fully decentralized DHT, no single point of control
- **Post-quantum safe** — NIST FIPS 203/204 algorithms only (ML-KEM-1024, ML-DSA-87)
- **End-to-end encrypted** — nodes store and forward opaque ciphertext; they never see plaintext
- **Multi-device friendly** — messages are retained for 7 days across all responsible nodes
- **Spam-resistant** — contact requests require proof-of-work; inboxes are allowlist-enforced
- **Permanent identity** — your keypair is your identity; no company can revoke it
- **Human-readable names** — short usernames (`[a-z0-9]{3,36}`) registered on-chain with PoW
- **Open protocol** — any client can interact with the network; `chromatin-node` is the reference implementation

---

## What's in this Repo

| Path | Description |
|------|-------------|
| `src/` | Node implementation (C++20) |
| `tests/` | C++ unit + integration tests (GoogleTest) |
| `docs/` | Design documents and plans |
| `docker/` | Docker configuration for multi-node testing |
| `PROTOCOL.md` | Architecture and design reference |
| `PROTOCOL-SPEC.md` | Wire format specification |
| `TECHNICAL-PAPER.md` | Non-technical overview of Chromatin |
| `MESSAGING-FLOW.md` | Step-by-step guide to how messages travel |

> **Coming soon:** `examples/` — code examples for interacting with the network via WebSocket.

---

## Building

**Prerequisites:** CMake 3.20+, a C++20 compiler, and `libsodium`. All other
dependencies (liboqs, libmdbx, uWebSockets, spdlog, jsoncpp) are fetched
automatically by CMake.

```sh
# Install libsodium (Debian/Ubuntu)
apt install libsodium-dev

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Or use Docker:

```sh
docker build -f Dockerfile -t chromatin-node .
```

---

## Running a Node

```sh
./build/chromatin-node --help
```

By default the node binds TCP on port `4000` (node-to-node) and WebSocket on
port `4001` (client-to-node), and bootstraps from `{0,1,2}.bootstrap.cpunk.io`.

---

## Documentation

| Document | Audience |
|----------|----------|
| [TECHNICAL-PAPER.md](TECHNICAL-PAPER.md) | Non-technical: what Chromatin is and why it exists |
| [MESSAGING-FLOW.md](MESSAGING-FLOW.md) | Non-technical: how messages travel through the network |
| [PROTOCOL.md](PROTOCOL.md) | Technical: architecture, design decisions, data flows |
| [PROTOCOL-SPEC.md](PROTOCOL-SPEC.md) | Technical: complete wire format specification |

---

## Status

**Pre-MVP — active development.** The protocol and implementation are not yet
production-ready. No backward compatibility is guaranteed before MVP.

The live test network runs at `{0,1,2}.bootstrap.pqcc.fi`.

---

## License

TBD.
