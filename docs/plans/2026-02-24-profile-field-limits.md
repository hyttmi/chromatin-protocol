# Profile Sub-Field Limits Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add per-field size limits to profile records (bio, avatar, social links) to prevent abuse within the 1 MiB total profile cap.

**Architecture:** Add constants to `config::protocol::`, add bounds checks to `validate_profile()` in kademlia.cpp during the existing field parsing loop, update PROTOCOL-SPEC.md and PROTOCOL.md, add client-side validation in Python `builders.py`, add C++ and Python tests.

**Tech Stack:** C++20, GoogleTest, Python pytest

---

### Task 1: Add protocol constants

**Files:**
- Modify: `src/config/config.h:14-22`

**Step 1: Add the five new constants to `config::protocol::`**

Add these after the existing `MAX_REQUEST_BLOB_SIZE` line (line 20):

```cpp
    constexpr uint32_t MAX_BIO_SIZE              = 2048;              // 2 KiB
    constexpr uint32_t MAX_AVATAR_SIZE           = 256 * 1024;        // 256 KiB
    constexpr uint8_t  MAX_SOCIAL_LINKS          = 16;
    constexpr uint8_t  MAX_SOCIAL_PLATFORM_LENGTH = 64;
    constexpr uint8_t  MAX_SOCIAL_HANDLE_LENGTH  = 128;
```

**Step 2: Verify it compiles**

Run: `cmake --build build --target chromatin-node 2>&1 | tail -5`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/config/config.h
git commit -m "feat(config): add profile sub-field size constants"
```

---

### Task 2: Add C++ tests for profile field limit rejection

**Files:**
- Modify: `tests/test_kademlia.cpp`

These tests reuse the existing `build_profile` helper at line 1876. However, that helper only builds minimal profiles (empty bio/avatar/social). We need an extended helper that can set arbitrary field content.

**Step 1: Add an extended profile builder helper**

Add this below the existing `build_profile` helper (after line ~1900):

```cpp
// Helper: build a valid profile with custom bio, avatar, and social links
static std::vector<uint8_t> build_profile_with_fields(
    const KeyPair& user_kp,
    uint64_t sequence,
    const std::vector<uint8_t>& bio,
    const std::vector<uint8_t>& avatar,
    const std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>& social_links
) {
    Hash user_fp = sha3_256(user_kp.public_key);
    std::vector<uint8_t> profile;
    // fingerprint(32)
    profile.insert(profile.end(), user_fp.begin(), user_fp.end());
    // pubkey_len(2 BE) + pubkey
    uint16_t pk_len = static_cast<uint16_t>(user_kp.public_key.size());
    profile.push_back(static_cast<uint8_t>((pk_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(pk_len & 0xFF));
    profile.insert(profile.end(), user_kp.public_key.begin(), user_kp.public_key.end());
    // kem_pubkey_len = 0
    profile.push_back(0x00); profile.push_back(0x00);
    // bio_len(2 BE) + bio
    uint16_t bio_len = static_cast<uint16_t>(bio.size());
    profile.push_back(static_cast<uint8_t>((bio_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(bio_len & 0xFF));
    profile.insert(profile.end(), bio.begin(), bio.end());
    // avatar_len(4 BE) + avatar
    uint32_t avatar_len = static_cast<uint32_t>(avatar.size());
    profile.push_back(static_cast<uint8_t>((avatar_len >> 24) & 0xFF));
    profile.push_back(static_cast<uint8_t>((avatar_len >> 16) & 0xFF));
    profile.push_back(static_cast<uint8_t>((avatar_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(avatar_len & 0xFF));
    profile.insert(profile.end(), avatar.begin(), avatar.end());
    // social_links_count(1) + links
    profile.push_back(static_cast<uint8_t>(social_links.size()));
    for (const auto& [platform, handle] : social_links) {
        profile.push_back(static_cast<uint8_t>(platform.size()));
        profile.insert(profile.end(), platform.begin(), platform.end());
        profile.push_back(static_cast<uint8_t>(handle.size()));
        profile.insert(profile.end(), handle.begin(), handle.end());
    }
    // sequence(8 BE)
    for (int i = 7; i >= 0; --i) {
        profile.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
    }
    // sign it
    auto sig = sign(std::span<const uint8_t>(profile.data(), profile.size()), user_kp.secret_key);
    uint16_t sig_len = static_cast<uint16_t>(sig.size());
    profile.push_back(static_cast<uint8_t>((sig_len >> 8) & 0xFF));
    profile.push_back(static_cast<uint8_t>(sig_len & 0xFF));
    profile.insert(profile.end(), sig.begin(), sig.end());
    return profile;
}
```

**Step 2: Add test for bio exceeding limit**

```cpp
TEST_F(KademliaTest, ProfileBioExceedsLimit) {
    start_all();
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash profile_key = sha3_256_prefixed("profile:", user_fp);

    // Bio at exactly MAX_BIO_SIZE + 1 bytes
    std::vector<uint8_t> big_bio(config::protocol::MAX_BIO_SIZE + 1, 'A');
    auto profile = build_profile_with_fields(user_kp, 1, big_bio, {}, {});

    bool stored = n1.kad->store(profile_key, 0x00, profile);
    EXPECT_FALSE(stored) << "Profile with oversized bio should be rejected";
}
```

**Step 3: Add test for avatar exceeding limit**

```cpp
TEST_F(KademliaTest, ProfileAvatarExceedsLimit) {
    start_all();
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash profile_key = sha3_256_prefixed("profile:", user_fp);

    // Avatar at exactly MAX_AVATAR_SIZE + 1 bytes
    std::vector<uint8_t> big_avatar(config::protocol::MAX_AVATAR_SIZE + 1, 0xFF);
    auto profile = build_profile_with_fields(user_kp, 1, {}, big_avatar, {});

    bool stored = n1.kad->store(profile_key, 0x00, profile);
    EXPECT_FALSE(stored) << "Profile with oversized avatar should be rejected";
}
```

**Step 4: Add test for too many social links**

```cpp
TEST_F(KademliaTest, ProfileTooManySocialLinks) {
    start_all();
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash profile_key = sha3_256_prefixed("profile:", user_fp);

    // MAX_SOCIAL_LINKS + 1 links, each with short platform/handle
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> links;
    for (int i = 0; i < config::protocol::MAX_SOCIAL_LINKS + 1; ++i) {
        links.push_back({{'x'}, {'y'}});
    }
    auto profile = build_profile_with_fields(user_kp, 1, {}, {}, links);

    bool stored = n1.kad->store(profile_key, 0x00, profile);
    EXPECT_FALSE(stored) << "Profile with too many social links should be rejected";
}
```

**Step 5: Add test for oversized platform string**

```cpp
TEST_F(KademliaTest, ProfileSocialPlatformTooLong) {
    start_all();
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash profile_key = sha3_256_prefixed("profile:", user_fp);

    // One link with platform > MAX_SOCIAL_PLATFORM_LENGTH
    std::vector<uint8_t> long_platform(config::protocol::MAX_SOCIAL_PLATFORM_LENGTH + 1, 'p');
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> links;
    links.push_back({long_platform, {'h'}});
    auto profile = build_profile_with_fields(user_kp, 1, {}, {}, links);

    bool stored = n1.kad->store(profile_key, 0x00, profile);
    EXPECT_FALSE(stored) << "Profile with oversized platform string should be rejected";
}
```

**Step 6: Add test for oversized handle string**

```cpp
TEST_F(KademliaTest, ProfileSocialHandleTooLong) {
    start_all();
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash profile_key = sha3_256_prefixed("profile:", user_fp);

    // One link with handle > MAX_SOCIAL_HANDLE_LENGTH
    std::vector<uint8_t> long_handle(config::protocol::MAX_SOCIAL_HANDLE_LENGTH + 1, 'h');
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> links;
    links.push_back({{'p'}, long_handle});
    auto profile = build_profile_with_fields(user_kp, 1, {}, {}, links);

    bool stored = n1.kad->store(profile_key, 0x00, profile);
    EXPECT_FALSE(stored) << "Profile with oversized handle string should be rejected";
}
```

**Step 7: Add test that valid profile at limits is accepted**

```cpp
TEST_F(KademliaTest, ProfileAtFieldLimitsAccepted) {
    start_all();
    KeyPair user_kp = generate_keypair();
    Hash user_fp = sha3_256(user_kp.public_key);
    Hash profile_key = sha3_256_prefixed("profile:", user_fp);

    // Exactly at each limit
    std::vector<uint8_t> max_bio(config::protocol::MAX_BIO_SIZE, 'B');
    std::vector<uint8_t> max_platform(config::protocol::MAX_SOCIAL_PLATFORM_LENGTH, 'p');
    std::vector<uint8_t> max_handle(config::protocol::MAX_SOCIAL_HANDLE_LENGTH, 'h');
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> links;
    links.push_back({max_platform, max_handle});
    // Skip max avatar here since it would exceed total 1 MiB with pubkey
    auto profile = build_profile_with_fields(user_kp, 1, max_bio, {}, links);

    bool stored = n1.kad->store(profile_key, 0x00, profile);
    EXPECT_TRUE(stored) << "Profile at field limits should be accepted";
}
```

**Step 8: Run tests to verify they fail (validation not yet implemented)**

Run: `cmake --build build --target chromatin_tests 2>&1 | tail -3 && ./build/tests/chromatin_tests --gtest_filter="KademliaTest.Profile*Exceeds*:KademliaTest.Profile*TooMany*:KademliaTest.Profile*TooLong*:KademliaTest.ProfileAtFieldLimitsAccepted" 2>&1 | tail -20`

Expected: Tests compile. The "exceeds/too many/too long" tests FAIL (profiles accepted when they shouldn't be). The "at limits accepted" test PASSES.

**Step 9: Commit the failing tests**

```bash
git add tests/test_kademlia.cpp
git commit -m "test(kad): add failing tests for profile sub-field limits"
```

---

### Task 3: Implement profile field validation in kademlia.cpp

**Files:**
- Modify: `src/kademlia/kademlia.cpp:1931-1961` (inside `validate_profile()`)

**Step 1: Add bio size check after parsing bio_len (line ~1935)**

Change the current "Skip bio" block:

```cpp
    // Skip bio
    if (offset + 2 > value.size()) return false;
    uint16_t bio_len = (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1];
    offset += 2;
    if (offset + bio_len > value.size()) return false;
    offset += bio_len;
```

To:

```cpp
    // bio
    if (offset + 2 > value.size()) return false;
    uint16_t bio_len = (static_cast<uint16_t>(value[offset]) << 8) | value[offset + 1];
    offset += 2;
    if (bio_len > config::protocol::MAX_BIO_SIZE) {
        spdlog::warn("Profile validation: bio size {} exceeds {} byte limit", bio_len, config::protocol::MAX_BIO_SIZE);
        return false;
    }
    if (offset + bio_len > value.size()) return false;
    offset += bio_len;
```

**Step 2: Add avatar size check after parsing avatar_len (line ~1945)**

Change the current "Skip avatar" block:

```cpp
    // Skip avatar
    if (offset + 4 > value.size()) return false;
    uint32_t avatar_len = (static_cast<uint32_t>(value[offset]) << 24)
                        | (static_cast<uint32_t>(value[offset + 1]) << 16)
                        | (static_cast<uint32_t>(value[offset + 2]) << 8)
                        | static_cast<uint32_t>(value[offset + 3]);
    offset += 4;
    if (offset + avatar_len > value.size()) return false;
    offset += avatar_len;
```

To:

```cpp
    // avatar
    if (offset + 4 > value.size()) return false;
    uint32_t avatar_len = (static_cast<uint32_t>(value[offset]) << 24)
                        | (static_cast<uint32_t>(value[offset + 1]) << 16)
                        | (static_cast<uint32_t>(value[offset + 2]) << 8)
                        | static_cast<uint32_t>(value[offset + 3]);
    offset += 4;
    if (avatar_len > config::protocol::MAX_AVATAR_SIZE) {
        spdlog::warn("Profile validation: avatar size {} exceeds {} byte limit", avatar_len, config::protocol::MAX_AVATAR_SIZE);
        return false;
    }
    if (offset + avatar_len > value.size()) return false;
    offset += avatar_len;
```

**Step 3: Add social links count check and per-link field checks (line ~1950)**

Change the current "Skip social links" block:

```cpp
    // Skip social links
    if (offset + 1 > value.size()) return false;
    uint8_t social_count = value[offset];
    offset += 1;
    for (uint8_t i = 0; i < social_count; ++i) {
        if (offset + 1 > value.size()) return false;
        uint8_t platform_len = value[offset]; offset += 1;
        if (offset + platform_len > value.size()) return false;
        offset += platform_len;
        if (offset + 1 > value.size()) return false;
        uint8_t handle_len = value[offset]; offset += 1;
        if (offset + handle_len > value.size()) return false;
        offset += handle_len;
    }
```

To:

```cpp
    // social links
    if (offset + 1 > value.size()) return false;
    uint8_t social_count = value[offset];
    offset += 1;
    if (social_count > config::protocol::MAX_SOCIAL_LINKS) {
        spdlog::warn("Profile validation: social_links count {} exceeds {} limit", social_count, config::protocol::MAX_SOCIAL_LINKS);
        return false;
    }
    for (uint8_t i = 0; i < social_count; ++i) {
        if (offset + 1 > value.size()) return false;
        uint8_t platform_len = value[offset]; offset += 1;
        if (platform_len > config::protocol::MAX_SOCIAL_PLATFORM_LENGTH) {
            spdlog::warn("Profile validation: platform string length {} exceeds {} limit", platform_len, config::protocol::MAX_SOCIAL_PLATFORM_LENGTH);
            return false;
        }
        if (offset + platform_len > value.size()) return false;
        offset += platform_len;
        if (offset + 1 > value.size()) return false;
        uint8_t handle_len = value[offset]; offset += 1;
        if (handle_len > config::protocol::MAX_SOCIAL_HANDLE_LENGTH) {
            spdlog::warn("Profile validation: handle string length {} exceeds {} limit", handle_len, config::protocol::MAX_SOCIAL_HANDLE_LENGTH);
            return false;
        }
        if (offset + handle_len > value.size()) return false;
        offset += handle_len;
    }
```

**Step 4: Run the tests**

Run: `cmake --build build --target chromatin_tests 2>&1 | tail -3 && ./build/tests/chromatin_tests --gtest_filter="KademliaTest.Profile*Exceeds*:KademliaTest.Profile*TooMany*:KademliaTest.Profile*TooLong*:KademliaTest.ProfileAtFieldLimitsAccepted" 2>&1 | tail -20`

Expected: All 7 tests PASS

**Step 5: Run the full test suite to check for regressions**

Run: `./build/tests/chromatin_tests 2>&1 | tail -5`

Expected: All tests pass (existing profiles use empty fields, well within limits)

**Step 6: Commit**

```bash
git add src/kademlia/kademlia.cpp
git commit -m "feat(kad): enforce per-field size limits in profile validation"
```

---

### Task 4: Add Python client-side validation in builders.py

**Files:**
- Modify: `tools/client/builders.py:8-55`
- Modify: `tools/client/tests/test_builders.py`

**Step 1: Add constants and validation to `build_profile_record()`**

Add constants at the top of `builders.py` (after the imports, before the function):

```python
# Profile sub-field limits (must match config::protocol:: in C++)
MAX_BIO_SIZE = 2048
MAX_AVATAR_SIZE = 256 * 1024
MAX_SOCIAL_LINKS = 16
MAX_SOCIAL_PLATFORM_LENGTH = 64
MAX_SOCIAL_HANDLE_LENGTH = 128
```

Add validation at the start of `build_profile_record()`, after `bio_bytes = bio.encode("utf-8")`:

```python
    if len(bio_bytes) > MAX_BIO_SIZE:
        raise ValueError(f"bio exceeds {MAX_BIO_SIZE} byte limit ({len(bio_bytes)} bytes)")
    if len(avatar) > MAX_AVATAR_SIZE:
        raise ValueError(f"avatar exceeds {MAX_AVATAR_SIZE} byte limit ({len(avatar)} bytes)")
    if len(social_links) > MAX_SOCIAL_LINKS:
        raise ValueError(f"social_links count exceeds {MAX_SOCIAL_LINKS} limit ({len(social_links)})")
    for platform, handle in social_links:
        p = platform.encode("utf-8")
        h = handle.encode("utf-8")
        if len(p) > MAX_SOCIAL_PLATFORM_LENGTH:
            raise ValueError(f"platform string exceeds {MAX_SOCIAL_PLATFORM_LENGTH} byte limit ({len(p)} bytes)")
        if len(h) > MAX_SOCIAL_HANDLE_LENGTH:
            raise ValueError(f"handle string exceeds {MAX_SOCIAL_HANDLE_LENGTH} byte limit ({len(h)} bytes)")
```

**Step 2: Add Python tests for validation**

Add to `tools/client/tests/test_builders.py`:

```python
import pytest
from builders import (
    build_profile_record,
    MAX_BIO_SIZE,
    MAX_AVATAR_SIZE,
    MAX_SOCIAL_LINKS,
    MAX_SOCIAL_PLATFORM_LENGTH,
    MAX_SOCIAL_HANDLE_LENGTH,
)


def test_profile_bio_exceeds_limit():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    with pytest.raises(ValueError, match="bio exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="A" * (MAX_BIO_SIZE + 1), avatar=b"", social_links=[], sequence=1,
        )


def test_profile_avatar_exceeds_limit():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    with pytest.raises(ValueError, match="avatar exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"\xff" * (MAX_AVATAR_SIZE + 1), social_links=[], sequence=1,
        )


def test_profile_too_many_social_links():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    links = [("x", "y")] * (MAX_SOCIAL_LINKS + 1)
    with pytest.raises(ValueError, match="social_links count exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"", social_links=links, sequence=1,
        )


def test_profile_platform_too_long():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    links = [("p" * (MAX_SOCIAL_PLATFORM_LENGTH + 1), "h")]
    with pytest.raises(ValueError, match="platform string exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"", social_links=links, sequence=1,
        )


def test_profile_handle_too_long():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    links = [("p", "h" * (MAX_SOCIAL_HANDLE_LENGTH + 1))]
    with pytest.raises(ValueError, match="handle string exceeds"):
        build_profile_record(
            seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
            bio="", avatar=b"", social_links=links, sequence=1,
        )


def test_profile_at_field_limits():
    pubkey, seckey = generate_keypair()
    fp = fingerprint_of(pubkey)
    # Should NOT raise
    record = build_profile_record(
        seckey=seckey, fingerprint=fp, pubkey=pubkey, kem_pubkey=b"",
        bio="B" * MAX_BIO_SIZE, avatar=b"",
        social_links=[("p" * MAX_SOCIAL_PLATFORM_LENGTH, "h" * MAX_SOCIAL_HANDLE_LENGTH)],
        sequence=1,
    )
    assert record[:32] == fp
```

**Step 3: Run Python tests**

Run: `cd tools/client && source venv/bin/activate && python -m pytest tests/ -v 2>&1 | tail -20`

Expected: All tests pass (including 6 new ones)

**Step 4: Commit**

```bash
git add tools/client/builders.py tools/client/tests/test_builders.py
git commit -m "feat(client): add profile sub-field limit validation to Python builder"
```

---

### Task 5: Update PROTOCOL-SPEC.md

**Files:**
- Modify: `PROTOCOL-SPEC.md`

**Step 1: Add per-field limits to the profile format section**

Find the profile format (around line 514-531). After the format block, add a constraints note:

```markdown
**Profile sub-field limits:**

| Field | Max size |
|-------|----------|
| bio | 2,048 bytes |
| avatar | 262,144 bytes (256 KiB) |
| social_links_count | 16 |
| platform (per link) | 64 bytes |
| handle (per link) | 128 bytes |

These limits are enforced independently of the 1 MiB total profile size cap.
```

**Step 2: Update the Profile STORE Validation section (around line 1405-1409)**

Add after the existing validation points:

```markdown
5. Per-field limits:
   - `bio_length` <= 2,048 bytes
   - `avatar_length` <= 262,144 bytes (256 KiB)
   - `social_links_count` <= 16
   - Each `platform_length` <= 64 bytes
   - Each `handle_length` <= 128 bytes
```

**Step 3: Update the Protocol Constants table (around line 1635)**

Add after the `Max profile size` row:

```markdown
| Max bio size                | 2,048 bytes                             |
| Max avatar size             | 256 KiB (262,144 bytes)                 |
| Max social links            | 16                                      |
| Max social platform length  | 64 bytes                                |
| Max social handle length    | 128 bytes                               |
```

**Step 4: Commit**

```bash
git add PROTOCOL-SPEC.md
git commit -m "docs(spec): document profile sub-field size limits"
```

---

### Task 6: Update PROTOCOL.md

**Files:**
- Modify: `PROTOCOL.md`

**Step 1: Find the profile section and add a note about per-field limits**

Add a brief mention of the per-field limits (bio 2 KB, avatar 256 KB, 16 social links max) to the profile section. Keep it concise — the spec has the full details.

**Step 2: Commit**

```bash
git add PROTOCOL.md
git commit -m "docs(protocol): mention profile sub-field limits"
```

---

### Task 7: Update live test for profile with long bio

**Files:**
- Modify: `tools/client/test_live.py`

**Step 1: Find the "profile with long bio" test**

Search for `profile with long bio` in test_live.py. This test currently sends a 10,000-byte bio. Update it to use the new limit:

- Change the bio to `MAX_BIO_SIZE` (2048 bytes) exactly — this should still pass
- Optionally add a test that `MAX_BIO_SIZE + 1` is rejected with a 400 error

**Step 2: Import the constant**

Add to the imports at the top:

```python
from builders import build_profile_record, build_name_record, build_group_meta, MAX_BIO_SIZE
```

**Step 3: Commit**

```bash
git add tools/client/test_live.py
git commit -m "test(live): update long bio test to respect new 2KB limit"
```
