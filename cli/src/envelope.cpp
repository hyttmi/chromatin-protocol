#include "cli/src/envelope.h"
#include "cli/src/wire.h"

#include <oqs/oqs.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace chromatindb::cli::envelope {

// =============================================================================
// Constants
// =============================================================================

static constexpr size_t KEM_PK_SIZE       = 1568;
static constexpr size_t KEM_CT_SIZE       = 1568;
static constexpr size_t KEM_SS_SIZE       = 32;
static constexpr size_t KEM_SK_SIZE       = 3168;
static constexpr size_t AEAD_KEY_SIZE     = 32;
static constexpr size_t AEAD_TAG_SIZE     = 16;
static constexpr size_t NONCE_SIZE        = 12;
static constexpr size_t PK_HASH_SIZE      = 32;
static constexpr size_t WRAPPED_DEK_SIZE  = AEAD_KEY_SIZE + AEAD_TAG_SIZE;  // 48
static constexpr size_t STANZA_SIZE       = PK_HASH_SIZE + KEM_CT_SIZE + WRAPPED_DEK_SIZE;  // 1648
static constexpr size_t HEADER_SIZE       = 20;

static constexpr uint8_t MAGIC[4]         = {0x43, 0x45, 0x4E, 0x56};  // "CENV"
static constexpr uint8_t VERSION          = 0x01;
static constexpr uint8_t SUITE            = 0x01;

static constexpr char HKDF_LABEL[] = "chromatindb-envelope-kek-v1";

// =============================================================================
// Internal: SHA3-256 hash
// =============================================================================



// =============================================================================
// Internal: HKDF-SHA256 (extract + expand) via libsodium
// =============================================================================

static std::array<uint8_t, 32> hkdf_derive_kek(
    std::span<const uint8_t> shared_secret) {

    // Extract: PRK = HMAC-SHA256(salt=empty, IKM=shared_secret)
    uint8_t prk[crypto_auth_hmacsha256_BYTES];
    // Empty salt (32 zero bytes for HMAC-SHA256)
    uint8_t salt[crypto_auth_hmacsha256_KEYBYTES] = {};
    crypto_auth_hmacsha256_state extract_state;
    crypto_auth_hmacsha256_init(&extract_state, salt, sizeof(salt));
    crypto_auth_hmacsha256_update(&extract_state, shared_secret.data(), shared_secret.size());
    crypto_auth_hmacsha256_final(&extract_state, prk);

    // Expand: OKM = HMAC-SHA256(PRK, label || 0x01) truncated to 32 bytes
    // For 32 bytes of output, one HMAC round suffices.
    crypto_auth_hmacsha256_state expand_state;
    crypto_auth_hmacsha256_init(&expand_state, prk, sizeof(prk));
    crypto_auth_hmacsha256_update(&expand_state,
        reinterpret_cast<const uint8_t*>(HKDF_LABEL), sizeof(HKDF_LABEL) - 1);
    uint8_t counter = 0x01;
    crypto_auth_hmacsha256_update(&expand_state, &counter, 1);

    std::array<uint8_t, 32> kek{};
    crypto_auth_hmacsha256_final(&expand_state, kek.data());

    sodium_memzero(prk, sizeof(prk));
    return kek;
}

// =============================================================================
// Internal: Stanza data for sorting
// =============================================================================

struct StanzaBuilder {
    std::array<uint8_t, PK_HASH_SIZE> pk_hash;
    std::array<uint8_t, KEM_CT_SIZE> kem_ct;
    std::array<uint8_t, KEM_SS_SIZE> shared_secret;
};

// =============================================================================
// encrypt
// =============================================================================

std::vector<uint8_t> encrypt(
    std::span<const uint8_t> plaintext,
    std::vector<std::span<const uint8_t>> recipient_kem_pubkeys) {

    if (recipient_kem_pubkeys.empty()) {
        throw std::runtime_error("envelope::encrypt: no recipients");
    }
    if (recipient_kem_pubkeys.size() > 65535) {
        throw std::runtime_error("envelope::encrypt: too many recipients");
    }

    // Validate all pubkey sizes
    for (auto& pk : recipient_kem_pubkeys) {
        if (pk.size() != KEM_PK_SIZE) {
            throw std::runtime_error("envelope::encrypt: invalid KEM pubkey size");
        }
    }

    // Generate DEK and data nonce
    std::array<uint8_t, AEAD_KEY_SIZE> dek{};
    randombytes_buf(dek.data(), dek.size());

    std::array<uint8_t, NONCE_SIZE> data_nonce{};
    randombytes_buf(data_nonce.data(), data_nonce.size());

    // Build stanza data: encapsulate for each recipient
    OQS_KEM* kem = OQS_KEM_new(OQS_KEM_alg_ml_kem_1024);
    if (!kem) {
        throw std::runtime_error("envelope::encrypt: failed to create ML-KEM-1024");
    }

    std::vector<StanzaBuilder> stanzas(recipient_kem_pubkeys.size());
    for (size_t i = 0; i < recipient_kem_pubkeys.size(); ++i) {
        // Compute pk_hash = SHA3-256(pubkey)
        stanzas[i].pk_hash = sha3_256(recipient_kem_pubkeys[i]);

        // Encapsulate
        if (OQS_KEM_encaps(kem,
                stanzas[i].kem_ct.data(),
                stanzas[i].shared_secret.data(),
                recipient_kem_pubkeys[i].data()) != OQS_SUCCESS) {
            OQS_KEM_free(kem);
            throw std::runtime_error("envelope::encrypt: ML-KEM encapsulation failed");
        }
    }
    OQS_KEM_free(kem);

    // Sort stanzas by pk_hash (lexicographic)
    std::sort(stanzas.begin(), stanzas.end(),
        [](const StanzaBuilder& a, const StanzaBuilder& b) {
            return a.pk_hash < b.pk_hash;
        });

    // Build fixed header (20 bytes)
    std::array<uint8_t, HEADER_SIZE> header{};
    std::memcpy(header.data(), MAGIC, 4);
    header[4] = VERSION;
    header[5] = SUITE;
    store_u16_be(header.data() + 6, static_cast<uint16_t>(stanzas.size()));
    std::memcpy(header.data() + 8, data_nonce.data(), NONCE_SIZE);

    // Wrap DEK for each stanza with incremental AD
    // AD for stanza[i] wrap = header(20) + accumulated (pk_hash + kem_ct) from stanzas 0..i-1
    static constexpr size_t PK_HASH_PLUS_CT = PK_HASH_SIZE + KEM_CT_SIZE;  // 1600

    // Pre-allocate AD buffer: header + up to N * (pk_hash + kem_ct)
    std::vector<uint8_t> ad_buf;
    ad_buf.reserve(HEADER_SIZE + stanzas.size() * PK_HASH_PLUS_CT);
    ad_buf.insert(ad_buf.end(), header.begin(), header.end());

    std::array<uint8_t, NONCE_SIZE> zero_nonce{};  // all zeros for DEK wrapping

    std::vector<std::array<uint8_t, WRAPPED_DEK_SIZE>> wrapped_deks(stanzas.size());

    for (size_t i = 0; i < stanzas.size(); ++i) {
        // Derive KEK from shared_secret via HKDF
        auto kek = hkdf_derive_kek(stanzas[i].shared_secret);

        // Wrap DEK: encrypt(dek, ad=ad_buf, nonce=zero, key=kek) -> 48 bytes
        unsigned long long ct_len = 0;
        if (crypto_aead_chacha20poly1305_ietf_encrypt(
                wrapped_deks[i].data(), &ct_len,
                dek.data(), AEAD_KEY_SIZE,
                ad_buf.data(), ad_buf.size(),
                nullptr, zero_nonce.data(), kek.data()) != 0) {
            sodium_memzero(dek.data(), dek.size());
            throw std::runtime_error("envelope::encrypt: DEK wrapping failed");
        }

        sodium_memzero(kek.data(), kek.size());
        sodium_memzero(stanzas[i].shared_secret.data(), stanzas[i].shared_secret.size());

        // Accumulate this stanza's pk_hash + kem_ct into AD for next iteration
        ad_buf.insert(ad_buf.end(), stanzas[i].pk_hash.begin(), stanzas[i].pk_hash.end());
        ad_buf.insert(ad_buf.end(), stanzas[i].kem_ct.begin(), stanzas[i].kem_ct.end());
    }

    // AD for data encryption = header(20) + all stanzas(N * 1648)
    std::vector<uint8_t> data_ad;
    data_ad.reserve(HEADER_SIZE + stanzas.size() * STANZA_SIZE);
    data_ad.insert(data_ad.end(), header.begin(), header.end());
    for (size_t i = 0; i < stanzas.size(); ++i) {
        data_ad.insert(data_ad.end(), stanzas[i].pk_hash.begin(), stanzas[i].pk_hash.end());
        data_ad.insert(data_ad.end(), stanzas[i].kem_ct.begin(), stanzas[i].kem_ct.end());
        data_ad.insert(data_ad.end(), wrapped_deks[i].begin(), wrapped_deks[i].end());
    }

    // Encrypt plaintext with DEK
    std::vector<uint8_t> ciphertext(plaintext.size() + AEAD_TAG_SIZE);
    unsigned long long ct_len = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            ciphertext.data(), &ct_len,
            plaintext.data(), plaintext.size(),
            data_ad.data(), data_ad.size(),
            nullptr, data_nonce.data(), dek.data()) != 0) {
        sodium_memzero(dek.data(), dek.size());
        throw std::runtime_error("envelope::encrypt: data encryption failed");
    }
    ciphertext.resize(static_cast<size_t>(ct_len));

    sodium_memzero(dek.data(), dek.size());

    // Assemble final envelope
    std::vector<uint8_t> envelope;
    envelope.reserve(HEADER_SIZE + stanzas.size() * STANZA_SIZE + ciphertext.size());

    // Header
    envelope.insert(envelope.end(), header.begin(), header.end());

    // Stanzas
    for (size_t i = 0; i < stanzas.size(); ++i) {
        envelope.insert(envelope.end(), stanzas[i].pk_hash.begin(), stanzas[i].pk_hash.end());
        envelope.insert(envelope.end(), stanzas[i].kem_ct.begin(), stanzas[i].kem_ct.end());
        envelope.insert(envelope.end(), wrapped_deks[i].begin(), wrapped_deks[i].end());
    }

    // Ciphertext + tag
    envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());

    return envelope;
}

// =============================================================================
// decrypt
// =============================================================================

std::optional<std::vector<uint8_t>> decrypt(
    std::span<const uint8_t> envelope_data,
    std::span<const uint8_t> our_kem_seckey,
    std::span<const uint8_t> our_kem_pubkey) {

    // Validate minimum size: header(20)
    if (envelope_data.size() < HEADER_SIZE) {
        return std::nullopt;
    }

    // Validate magic
    if (std::memcmp(envelope_data.data(), MAGIC, 4) != 0) {
        return std::nullopt;
    }

    // Validate version and suite
    if (envelope_data[4] != VERSION || envelope_data[5] != SUITE) {
        return std::nullopt;
    }

    // Validate key sizes
    if (our_kem_seckey.size() != KEM_SK_SIZE || our_kem_pubkey.size() != KEM_PK_SIZE) {
        return std::nullopt;
    }

    // Parse header
    uint16_t recipient_count = load_u16_be(envelope_data.data() + 6);
    if (recipient_count == 0) {
        return std::nullopt;
    }

    // Validate total size: header + stanzas + at least the tag
    size_t stanza_total = static_cast<size_t>(recipient_count) * STANZA_SIZE;
    size_t min_size = HEADER_SIZE + stanza_total + AEAD_TAG_SIZE;
    if (envelope_data.size() < min_size) {
        return std::nullopt;
    }

    // data_nonce is at offset 8, 12 bytes
    const uint8_t* data_nonce = envelope_data.data() + 8;

    // Compute our pk_hash
    auto our_pk_hash = sha3_256(our_kem_pubkey);

    // Scan stanzas to find our match
    const uint8_t* stanzas_start = envelope_data.data() + HEADER_SIZE;
    int matched_index = -1;

    for (uint16_t i = 0; i < recipient_count; ++i) {
        const uint8_t* stanza = stanzas_start + static_cast<size_t>(i) * STANZA_SIZE;
        if (std::memcmp(stanza, our_pk_hash.data(), PK_HASH_SIZE) == 0) {
            matched_index = static_cast<int>(i);
            break;
        }
    }

    if (matched_index < 0) {
        return std::nullopt;  // Not a recipient
    }

    // Get our stanza
    const uint8_t* our_stanza = stanzas_start + static_cast<size_t>(matched_index) * STANZA_SIZE;
    const uint8_t* kem_ct = our_stanza + PK_HASH_SIZE;
    const uint8_t* wrapped_dek = kem_ct + KEM_CT_SIZE;

    // Decapsulate
    std::array<uint8_t, KEM_SS_SIZE> shared_secret{};
    OQS_KEM* kem = OQS_KEM_new(OQS_KEM_alg_ml_kem_1024);
    if (!kem) {
        return std::nullopt;
    }

    if (OQS_KEM_decaps(kem, shared_secret.data(), kem_ct, our_kem_seckey.data()) != OQS_SUCCESS) {
        OQS_KEM_free(kem);
        return std::nullopt;
    }
    OQS_KEM_free(kem);

    // Derive KEK
    auto kek = hkdf_derive_kek(shared_secret);
    sodium_memzero(shared_secret.data(), shared_secret.size());

    // Rebuild AD for this stanza's position:
    // header(20) + accumulated (pk_hash + kem_ct) from stanzas 0..matched_index-1
    static constexpr size_t PK_HASH_PLUS_CT = PK_HASH_SIZE + KEM_CT_SIZE;
    std::vector<uint8_t> wrap_ad;
    wrap_ad.reserve(HEADER_SIZE + static_cast<size_t>(matched_index) * PK_HASH_PLUS_CT);
    wrap_ad.insert(wrap_ad.end(), envelope_data.data(), envelope_data.data() + HEADER_SIZE);

    for (int i = 0; i < matched_index; ++i) {
        const uint8_t* s = stanzas_start + static_cast<size_t>(i) * STANZA_SIZE;
        // pk_hash(32) + kem_ct(1568) = 1600 bytes
        wrap_ad.insert(wrap_ad.end(), s, s + PK_HASH_PLUS_CT);
    }

    // Unwrap DEK
    std::array<uint8_t, AEAD_KEY_SIZE> dek{};
    unsigned long long dek_len = 0;
    std::array<uint8_t, NONCE_SIZE> zero_nonce{};

    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            dek.data(), &dek_len,
            nullptr,
            wrapped_dek, WRAPPED_DEK_SIZE,
            wrap_ad.data(), wrap_ad.size(),
            zero_nonce.data(), kek.data()) != 0) {
        sodium_memzero(kek.data(), kek.size());
        return std::nullopt;
    }
    sodium_memzero(kek.data(), kek.size());

    // Build AD for data decryption: header(20) + all stanzas(N * 1648)
    std::vector<uint8_t> data_ad;
    data_ad.reserve(HEADER_SIZE + stanza_total);
    data_ad.insert(data_ad.end(), envelope_data.data(), envelope_data.data() + HEADER_SIZE);
    data_ad.insert(data_ad.end(), stanzas_start, stanzas_start + stanza_total);

    // Decrypt ciphertext
    const uint8_t* ciphertext = stanzas_start + stanza_total;
    size_t ciphertext_len = envelope_data.size() - HEADER_SIZE - stanza_total;

    if (ciphertext_len < AEAD_TAG_SIZE) {
        sodium_memzero(dek.data(), dek.size());
        return std::nullopt;
    }

    std::vector<uint8_t> decrypted(ciphertext_len - AEAD_TAG_SIZE);
    unsigned long long pt_len = 0;

    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            decrypted.data(), &pt_len,
            nullptr,
            ciphertext, ciphertext_len,
            data_ad.data(), data_ad.size(),
            data_nonce, dek.data()) != 0) {
        sodium_memzero(dek.data(), dek.size());
        return std::nullopt;
    }
    sodium_memzero(dek.data(), dek.size());

    decrypted.resize(static_cast<size_t>(pt_len));
    return decrypted;
}

// =============================================================================
// is_envelope
// =============================================================================

bool is_envelope(std::span<const uint8_t> data) {
    if (data.size() < 4) {
        return false;
    }
    return std::memcmp(data.data(), MAGIC, 4) == 0;
}

} // namespace chromatindb::cli::envelope
