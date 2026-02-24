# Profile Sub-Field Limits Design

**Goal:** Add per-field size limits to profile records to prevent abuse within the 1 MiB total profile cap.

## Limits

| Field | Current max | New limit | Constant |
|-------|-----------|-----------|----------|
| bio | 65,535 bytes | 2,048 bytes | `MAX_BIO_SIZE` |
| avatar | 4 GB (4-byte prefix) | 262,144 bytes (256 KB) | `MAX_AVATAR_SIZE` |
| social_links count | 255 | 16 | `MAX_SOCIAL_LINKS` |
| platform string | 255 bytes | 64 bytes | `MAX_SOCIAL_PLATFORM_LENGTH` |
| handle string | 255 bytes | 128 bytes | `MAX_SOCIAL_HANDLE_LENGTH` |

No display name field — registered name is the only identity. One key, one username, no impersonation.

## Enforcement

- **`config::protocol::`** — new constants
- **`kademlia.cpp` `validate_profile()`** — reject profiles exceeding any field limit during parsing (covers both client SET_PROFILE and Kademlia STORE replication)
- **PROTOCOL-SPEC.md** — document limits in profile format section and constraints section
- **PROTOCOL.md** — mention in profile section
- **Python client `builders.py`** — client-side validation for early feedback
- **C++ tests** — test each limit rejection
- **Python tests** — test builder validation

## Wire format

No changes to binary encoding. Length prefixes stay the same (2-byte bio, 4-byte avatar, 1-byte social). Upper-bound validation only. Forward-compatible if limits are relaxed later.

## Error handling

- `validate_profile()` returns false with spdlog warning
- WS server returns `400 "profile field exceeds limit"`
- STORE_ACK returns status 0x01 reason 0x02 (validation failed)
