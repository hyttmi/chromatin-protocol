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

## Step 1: Creating Your Identity

Before you can message anyone, you need a **Chromatin identity**.

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

## Step 8: Group Messaging

Chromatin supports group conversations with up to 512 members. Groups use a
**shared inbox** model — one copy of each message is stored on the network,
and all members read from the same location.

### Creating a Group

```
Alice creates a group and adds Bob and Charlie:

  Alice
    │
    │  GROUP_CREATE
    │  "Book Club"
    │ ──────────────────────►  Responsible Nodes
    │                            [N1]  [N2]  [N3]
    │                              │     │     │
    │  Group created ✓              Store GROUP_META:
    │ ◄──────────────────────      • group_id (random)
    │                              • owner: Alice
    │                              • members: Alice, Bob, Charlie
    │                              • roles: owner, member, member
```

The group has **three roles**:
- **Owner** — can do everything, including destroying the group
- **Admin** — can add/remove regular members
- **Member** — can send messages and read the group inbox

### Sending a Group Message

```
Alice sends a message to the Book Club:

  Alice                       Responsible Nodes
    │                           [N1]  [N2]  [N3]
    │                             │     │     │
    │  1. Alice encrypts         │     │     │
    │     with the Group         │     │     │
    │     Encryption Key (GEK)   │     │     │
    │                             │     │     │
    │  2. GROUP_SEND              │     │     │
    │ ────────────────────────►   │     │     │
    │                             │     │     │
    │                        3. Node checks:
    │                           Is Alice a member? ✓
    │                           Store the message
    │                             │     │     │
    │  4. "Stored" ✓              │     │     │
    │ ◄────────────────────────   │     │     │
    │                             │     │     │
    │                        5. Push notification
    │                           to Bob and Charlie
    │                           (if they're online)
    │                             │     │     │
    │                             ▼     ▼     ▼
    │                           Bob   Charlie
    │                           gets  gets
    │                           push  push
```

Unlike 1-to-1 messages, group messages are stored **once** at a location
derived from the group ID — not copied to each member's inbox. Every member
reads from the same shared inbox. The node enforces access control: only
members listed in the GROUP_META can read or write.

### Group Encryption Key (GEK)

Messages are encrypted with a **Group Encryption Key** shared among members.
The GEK is distributed to each member inside the GROUP_META, encrypted with
their individual public key (ML-KEM-1024). When membership changes, a new
GEK version is generated — old members lose access to new messages.

```
GROUP_META contains per-member encrypted GEK:

  ┌─────────────────────────────────────────────┐
  │  group_id    │  owner_fp   │  version: 3    │
  ├──────────────┼─────────────┼────────────────┤
  │  Alice (owner)  │  GEK v3 encrypted for Alice  │
  │  Bob (member)   │  GEK v3 encrypted for Bob    │
  │  Charlie (admin) │  GEK v3 encrypted for Charlie│
  └─────────────────┴─────────────────────────────┘
```

### Reading Group Messages

```
Bob checks the Book Club:

  Bob                        Responsible Node [N1]
    │                              │
    │  GROUP_LIST                  │
    │  (after: last_seen_key)      │
    │ ──────────────────────────►  │
    │                              │
    │   Message index:             │
    │   • Alice, 2 min ago         │
    │   • Charlie, 1 min ago       │
    │ ◄──────────────────────────  │
    │                              │
    │  GROUP_GET (specific msg)    │
    │ ──────────────────────────►  │
    │                              │
    │   Encrypted message blob     │
    │ ◄──────────────────────────  │
    │                              │
    │  Bob decrypts with GEK       │
```

### Managing Members

Owners and admins can update the group by publishing a new GROUP_META with
an incremented version number. The node enforces role restrictions:

- **Admins** can add or remove regular members
- **Admins** cannot change roles, remove other admins, or remove owners
- **Owners** can do everything — add, remove, promote, demote
- If all members are removed, the group is automatically destroyed

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
| Groups managed by company servers | Groups managed by the network with shared inboxes |

---

## In One Sentence

**Chromatin is a messenger where your messages are encrypted, stored across
independent nodes that no one controls, and protected by encryption that
even future quantum computers can't break.**
