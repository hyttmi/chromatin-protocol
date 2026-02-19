# How Messaging Works in Chromatin

> A simple guide to how messages travel through the Chromatin network.

---

## The Big Picture

Imagine a network of **post offices** spread around the world. No single company
owns them — anyone can run one. When you send a message, these post offices work
together to deliver it. No one can read your message except the person you sent
it to, and no one can shut down the network because there's no central server
to turn off.

That's Chromatin.

---

## Step 1: Creating Your Identity (DNA)

Before you can message anyone, you need a **DNA** — your digital identity.

```
  You
   │
   ▼
┌─────────────────────────────┐
│  Your device generates:     │
│                             │
│  • A unique fingerprint     │
│    (like a digital ID card) │
│                             │
│  • A secret key             │
│    (only you have this)     │
│                             │
│  • A public key             │
│    (anyone can see this)    │
└─────────────────────────────┘
```

Your fingerprint is **yours forever**. No company controls it. No server can
revoke it. You must also register a human-readable name (like "alice") that
maps to your fingerprint — think of it as a username, but permanent and free.

---

## Step 2: Finding Your Post Office

The network figures out which post offices (nodes) are responsible for holding
your messages. You don't choose — it's determined automatically by math.

```
The network has many nodes:

  [Node A]   [Node B]   [Node C]   [Node D]   [Node E]  ...

Based on your fingerprint, the network assigns 3 nodes to hold your inbox:

  Your inbox lives on:  [Node B]  [Node D]  [Node E]
                            │         │         │
                            └────┬────┘         │
                                 └───────┬──────┘
                                         │
                              These 3 keep copies
                              of your messages
                              (so nothing gets lost)
```

Why 3 copies? If one node goes down, the other two still have your messages.
Redundancy = reliability.

---

## Step 3: Adding a Contact

Before someone can message you, you need to **allow** them. This prevents spam —
only people you approve can send you messages.

```
Alice wants to message Bob, but Bob hasn't approved her yet.

  Alice                          Bob
    │                             │
    │  "Hey Bob, can we chat?"    │
    │  ──── Contact Request ────► │
    │       (with proof-of-work)  │
    │                             │
    │                        Bob sees the request
    │                        and approves Alice
    │                             │
    │  "You're approved!"         │
    │  ◄──── Allowlist Update ─── │
    │                             │
    Now Alice can message Bob ✓
```

The **proof-of-work** is like solving a small puzzle — it takes your device a
few seconds. This makes it costly to send millions of spam requests, but easy
for real people.

---

## Step 4: Sending a Message

Alice sends a message to Bob. Here's what happens behind the scenes:

```
Alice                    Any Node              Bob's Responsible Nodes
  │                         │                    [B]    [D]    [E]
  │                         │                     │      │      │
  │  1. Alice scrambles     │                     │      │      │
  │     her message so      │                     │      │      │
  │     only Bob can        │                     │      │      │
  │     read it             │                     │      │      │
  │                         │                     │      │      │
  │  2. Sends scrambled     │                     │      │      │
  │     message to any      │                     │      │      │
  │     node she's          │                     │      │      │
  │     connected to        │                     │      │      │
  │ ─────────────────────►  │                     │      │      │
  │                         │  3. That node       │      │      │
  │                         │     forwards to     │      │      │
  │                         │     Bob's 3 nodes   │      │      │
  │                         │ ──────────────────► │      │      │
  │                         │ ───────────────────────► │      │
  │                         │ ──────────────────────────────► │
  │                         │                     │      │      │
  │  4. Alice gets a        │                     │      │      │
  │     confirmation:       │                     │      │      │
  │     "Message stored"    │                     │      │      │
  │ ◄─────────────────────  │                     │      │      │
```

The message is **end-to-end encrypted** — the nodes store it, but they can't
read it. They just see scrambled data.

---

## Step 5: Receiving Messages

Bob opens his app and connects to one of his responsible nodes:

```
Bob                       Bob's Node [B]
 │                              │
 │  "Give me messages           │
 │   since last time"           │
 │ ──────────────────────────►  │
 │                              │
 │   Here are 3 new messages:   │
 │   • From Alice (2 min ago)   │
 │   • From Alice (1 min ago)   │
 │   • From Charlie (just now)  │
 │ ◄──────────────────────────  │
 │                              │
 │  Bob's device unscrambles    │
 │  each message with his       │
 │  secret key                  │
 │                              │
 │  💬 Alice: "Hey Bob!"        │
 │  💬 Alice: "You there?"      │
 │  💬 Charlie: "Meeting at 3"  │
```

If Bob is already connected, new messages are **pushed instantly** — no need
to ask.

---

## Step 6: Multiple Devices

Bob has a phone and a laptop. Both can receive messages independently:

```
                    Bob's Node [B]
                         │
              Messages stored for 7 days
                    ┌────┴────┐
                    │         │
                    ▼         ▼
              Bob's Phone   Bob's Laptop
              fetches at    fetches at
              9:00 AM       9:30 AM
                    │         │
              Gets all      Gets all
              messages      messages
              since last    since last
              fetch         fetch
```

Each device tracks its own position — "I last checked at 9:00 AM, give me
everything after that." The server doesn't know or care how many devices Bob
has. Messages stick around for **7 days**, giving all devices time to sync up.

---

## Step 7: Messages Expire

After 7 days, messages are automatically cleaned up:

```
Day 1: Message arrives ──────────────────────── Stored on 3 nodes
Day 2: Bob's phone fetches it ─────────────── Still stored
Day 3: Bob's laptop fetches it ────────────── Still stored
  ...
Day 7: Automatic cleanup ─────────────────── Message deleted from all nodes
```

Bob can also choose to delete messages earlier if all his devices have them.
But even if he doesn't, the 7-day cleanup keeps the network from filling up.

---

## What Makes This Different

| Traditional Messenger | Chromatin |
|---|---|
| One company runs the servers | Anyone can run a node |
| Company can read your messages | No one can — end-to-end encrypted |
| Government can shut it down | No central point to shut down |
| Quantum computers will break it | Uses future-proof encryption |
| Company controls your account | Your identity is yours forever |
| One server = one point of failure | 3 copies on independent nodes |

---

## In One Sentence

**Chromatin is a messenger where your messages are encrypted, stored across
independent nodes that no one controls, and protected by encryption that
even future quantum computers can't break.**
