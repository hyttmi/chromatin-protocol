# Chromatin: A Decentralized, Quantum-Safe Messenger

> **cpunk.io** — Censorship-resistant communication for everyone.

---

## The Problem

Today's messengers have a fundamental flaw: **they rely on central servers**.

When you send a message on WhatsApp, Signal, or Telegram, it passes through
servers owned by a single company. This creates three serious problems:

1. **Censorship** — A government can order the company to block users, delete
   messages, or shut down entirely. This has happened in multiple countries.

2. **Surveillance** — Even with end-to-end encryption, the company knows who
   you're talking to, when, and how often. This metadata is valuable and
   subpoenable.

3. **Single point of failure** — If the company goes bankrupt, gets acquired,
   or decides to change course, your communication platform disappears.

And there's a looming fourth problem: **quantum computers**. The encryption
used by every major messenger today (RSA, Elliptic Curve) will be breakable
by quantum computers. Adversaries are already collecting encrypted traffic
today to decrypt it later — a strategy known as "harvest now, decrypt later."

---

## The Solution: Chromatin

Chromatin is a messaging network with no central server. Instead, it's a
**network of independent nodes** — computers run by different people in
different places. They work together to store and deliver messages, but
no single node (or person) controls the network.

### No Central Server

```
Traditional messenger:          Chromatin:

     [Users]                    [Users]
        │                      ╱  │  ╲
        ▼                     ▼   ▼   ▼
  ┌──────────┐           [Node] [Node] [Node]
  │ Company  │           [Node] [Node] [Node]
  │ Server   │           [Node] [Node] [Node]
  └──────────┘
  Single point             No single point
  of failure               of failure
```

Anyone can run a Chromatin node. There's nothing to shut down because there's
no center. Even if half the nodes disappear, the remaining ones continue
operating. Messages are stored on **3 independent nodes** simultaneously,
so losing one doesn't lose your data.

---

## DNA: Your Digital Identity

In Chromatin, your identity is called a **DNA**. It's a cryptographic identity
that belongs to you — not to any company or server.

When you create a DNA, your device generates a unique **fingerprint** (a
string of characters that identifies you) and a pair of cryptographic keys:

- **Public key** — shared with the world, used by others to encrypt messages
  to you
- **Secret key** — kept only on your device, used to decrypt messages and
  prove your identity

No one can impersonate you because no one has your secret key. No server
can revoke your identity because no server issued it. Your DNA is permanent
and self-sovereign.

Every identity must register a **human-readable name** (like "alice" or
"bob42") that maps to its fingerprint. Name registration is free — it just
requires a computational puzzle that takes your device a couple of minutes
to solve. This prevents name squatting without requiring payment or a
central registrar.

---

## How Messages Are Delivered

The network uses a system called **Kademlia** to decide which nodes are
responsible for storing which data. Every piece of data — your profile,
your name, your inbox — is assigned to 3 nodes based on mathematical
proximity. This assignment is deterministic: any node in the network can
compute who's responsible for what.

When Alice sends a message to Bob:

1. **Alice encrypts** the message so only Bob can read it
2. **Alice's node** figures out which 3 nodes hold Bob's inbox
3. **The message is forwarded** to all 3 of Bob's responsible nodes
4. **Bob connects** to any of his nodes and picks up the message
5. **Bob decrypts** the message with his secret key

The nodes never see the actual message — they only handle encrypted blobs.
They don't know what Alice said to Bob. They don't even know it's a text
message versus a photo versus a file.

---

## Spam Prevention

Chromatin uses an **allowlist** model: you must approve someone before they can
message you. No approval = no messages. This eliminates spam entirely for
your main inbox.

But how does someone contact you for the first time? Through a **contact
request inbox**. Anyone can send you a request, but they must solve a
computational puzzle first (proof-of-work). This is trivial for a real
person — a few seconds of computation — but prohibitively expensive for
a spammer trying to send millions of requests.

---

## Quantum-Safe Security

Chromatin uses **post-quantum cryptography** exclusively. While other messengers
rely on math that quantum computers will eventually break, Chromatin uses
algorithms specifically designed to resist quantum attacks:

- **ML-DSA-87** for digital signatures (proving identity, signing messages)
- **ML-KEM-1024** for encryption (protecting message contents)
- **SHA3-256** for hashing (generating fingerprints and keys)
- **AES-256-GCM** for symmetric encryption (encrypting message data)

These are standardized by NIST (the US National Institute of Standards and
Technology) under FIPS 203 and FIPS 204. They're not experimental — they're
the industry standard for post-quantum security.

This means Chromatin is protected not just against today's threats, but against
the quantum computers of the future.

---

## Multi-Device Support

Messages in Chromatin are stored for **7 days** on the responsible nodes. During
that time, any of your devices — phone, laptop, tablet — can independently
connect and fetch messages. Each device tracks its own sync position, so it
only downloads messages it hasn't seen yet.

The server knows nothing about your devices. It doesn't track how many you
have or which ones are online. It simply stores messages and serves them
when asked. After 7 days, messages are automatically cleaned up.

---

## Censorship Resistance

Chromatin is designed to be **unstoppable**:

- **No company to subpoena** — The network is operated by independent node
  operators worldwide
- **No server to seize** — Shutting down one node doesn't affect the network;
  messages are replicated across 3 nodes
- **No DNS to block** — Nodes discover each other through the network itself,
  not through a central directory
- **No data to read** — Even if a government controls a node, all messages
  are encrypted blobs that can't be decrypted
- **Open protocol** — Anyone can build a client, run a node, or extend the
  network. It's not controlled by any single entity

The only way to stop Chromatin would be to shut down every single node in the
network simultaneously — which is practically impossible when nodes are
spread across different countries, jurisdictions, and operators.

---

## Key Properties

| Property | How Chromatin Achieves It |
|---|---|
| **Privacy** | End-to-end encryption — nodes never see message contents |
| **Quantum safety** | ML-DSA-87 + ML-KEM-1024 — immune to quantum attacks |
| **Decentralization** | No central server — independent nodes cooperate |
| **Censorship resistance** | No single point of failure or control |
| **Self-sovereign identity** | Your DNA is yours — no company can revoke it |
| **Spam prevention** | Allowlist model + proof-of-work for contact requests |
| **Reliability** | 3-node replication — survive node failures |
| **Multi-device** | 7-day message retention, per-device sync |
| **Open protocol** | Anyone can build clients, run nodes, extend the network |

---

## About cpunk.io

Chromatin is built by **cpunk.io** — a project dedicated to building
censorship-resistant, privacy-preserving communication tools. The protocol
is open, the code is open, and the network belongs to everyone who runs it.

We believe that private communication is a fundamental right, not a feature
that can be revoked by a terms-of-service update. Chromatin is our answer to
a world where digital communication is increasingly surveilled, censored,
and controlled.

---

*For technical details, see [PROTOCOL-SPEC.md](PROTOCOL-SPEC.md) and
[PROTOCOL.md](PROTOCOL.md). For a visual walkthrough of message delivery,
see [MESSAGING-FLOW.md](MESSAGING-FLOW.md).*
