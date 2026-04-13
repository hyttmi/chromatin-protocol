#include <catch2/catch_test_macros.hpp>
#include "relay/http/token_store.h"

#include <array>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace chromatindb::relay::http;

static std::vector<uint8_t> make_pubkey(uint8_t fill = 0x42) {
    return std::vector<uint8_t>(2592, fill);
}

static std::array<uint8_t, 32> make_namespace(uint8_t fill = 0xAA) {
    std::array<uint8_t, 32> ns{};
    ns.fill(fill);
    return ns;
}

// ============================================================================
// create_session
// ============================================================================

TEST_CASE("create_session returns 64-char hex token", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace());
    CHECK(token.size() == 64);
    // All chars should be hex
    for (char c : token) {
        bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(is_hex);
    }
}

TEST_CASE("create_session assigns unique session_id starting at 1", "[http][token_store]") {
    TokenStore store;
    auto token1 = store.create_session(make_pubkey(0x01), make_namespace(0x01));
    auto token2 = store.create_session(make_pubkey(0x02), make_namespace(0x02));

    auto* s1 = store.lookup(token1);
    auto* s2 = store.lookup(token2);
    REQUIRE(s1 != nullptr);
    REQUIRE(s2 != nullptr);
    CHECK(s1->session_id == 1);
    CHECK(s2->session_id == 2);
}

TEST_CASE("two create_session calls produce different tokens", "[http][token_store]") {
    TokenStore store;
    auto token1 = store.create_session(make_pubkey(0x01), make_namespace(0x01));
    auto token2 = store.create_session(make_pubkey(0x02), make_namespace(0x02));
    CHECK(token1 != token2);
}

TEST_CASE("create_session stores pubkey and namespace", "[http][token_store]") {
    TokenStore store;
    auto pk = make_pubkey(0x55);
    auto ns = make_namespace(0xBB);
    auto token = store.create_session(pk, ns);

    auto* state = store.lookup(token);
    REQUIRE(state != nullptr);
    CHECK(state->client_pubkey == pk);
    CHECK(state->client_namespace == ns);
}

// ============================================================================
// lookup
// ============================================================================

TEST_CASE("lookup returns state for valid token", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace());
    auto* state = store.lookup(token);
    REQUIRE(state != nullptr);
    CHECK(state->session_id == 1);
}

TEST_CASE("lookup returns nullptr for unknown token", "[http][token_store]") {
    TokenStore store;
    auto* state = store.lookup("0000000000000000000000000000000000000000000000000000000000000000");
    CHECK(state == nullptr);
}

TEST_CASE("lookup updates last_activity", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace());
    auto* state = store.lookup(token);
    REQUIRE(state != nullptr);
    auto first_activity = state->last_activity;

    // Small delay
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto* state2 = store.lookup(token);
    REQUIRE(state2 != nullptr);
    CHECK(state2->last_activity >= first_activity);
}

// ============================================================================
// lookup_by_id
// ============================================================================

TEST_CASE("lookup_by_id returns state for valid ID", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace());
    auto* state = store.lookup_by_id(1);
    REQUIRE(state != nullptr);
    CHECK(state->session_id == 1);
}

TEST_CASE("lookup_by_id returns nullptr for unknown ID", "[http][token_store]") {
    TokenStore store;
    auto* state = store.lookup_by_id(999);
    CHECK(state == nullptr);
}

// ============================================================================
// get_token
// ============================================================================

TEST_CASE("get_token returns token for valid session_id", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace());
    auto* found = store.get_token(1);
    REQUIRE(found != nullptr);
    CHECK(*found == token);
}

TEST_CASE("get_token returns nullptr for unknown ID", "[http][token_store]") {
    TokenStore store;
    auto* found = store.get_token(999);
    CHECK(found == nullptr);
}

// ============================================================================
// remove_session
// ============================================================================

TEST_CASE("remove_session by ID removes token mapping", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace());
    CHECK(store.count() == 1);

    store.remove_session(1);
    CHECK(store.count() == 0);
    CHECK(store.lookup(token) == nullptr);
}

TEST_CASE("remove_session with unknown ID is no-op", "[http][token_store]") {
    TokenStore store;
    store.create_session(make_pubkey(), make_namespace());
    store.remove_session(999);  // Should not crash or remove anything
    CHECK(store.count() == 1);
}

// ============================================================================
// remove_by_token
// ============================================================================

TEST_CASE("remove_by_token removes the session", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace());
    CHECK(store.count() == 1);

    store.remove_by_token(token);
    CHECK(store.count() == 0);
    CHECK(store.lookup(token) == nullptr);
    CHECK(store.lookup_by_id(1) == nullptr);
}

TEST_CASE("remove_by_token with unknown token is no-op", "[http][token_store]") {
    TokenStore store;
    store.create_session(make_pubkey(), make_namespace());
    store.remove_by_token("deadbeef");
    CHECK(store.count() == 1);
}

// ============================================================================
// reap_idle
// ============================================================================

TEST_CASE("reap_idle with 0s timeout removes all sessions", "[http][token_store]") {
    TokenStore store;
    store.create_session(make_pubkey(0x01), make_namespace(0x01));
    store.create_session(make_pubkey(0x02), make_namespace(0x02));
    store.create_session(make_pubkey(0x03), make_namespace(0x03));
    CHECK(store.count() == 3);

    auto reaped = store.reap_idle(std::chrono::seconds(0));
    CHECK(reaped == 3);
    CHECK(store.count() == 0);
}

TEST_CASE("reap_idle with 1h timeout removes nothing for fresh sessions", "[http][token_store]") {
    TokenStore store;
    store.create_session(make_pubkey(0x01), make_namespace(0x01));
    store.create_session(make_pubkey(0x02), make_namespace(0x02));

    auto reaped = store.reap_idle(std::chrono::seconds(3600));
    CHECK(reaped == 0);
    CHECK(store.count() == 2);
}

// ============================================================================
// count
// ============================================================================

TEST_CASE("count returns correct active session count", "[http][token_store]") {
    TokenStore store;
    CHECK(store.count() == 0);

    store.create_session(make_pubkey(0x01), make_namespace(0x01));
    CHECK(store.count() == 1);

    auto token2 = store.create_session(make_pubkey(0x02), make_namespace(0x02));
    CHECK(store.count() == 2);

    store.remove_by_token(token2);
    CHECK(store.count() == 1);
}

// ============================================================================
// for_each / for_each_mut
// ============================================================================

TEST_CASE("for_each iterates all sessions", "[http][token_store]") {
    TokenStore store;
    store.create_session(make_pubkey(0x01), make_namespace(0x01));
    store.create_session(make_pubkey(0x02), make_namespace(0x02));

    std::set<uint64_t> ids;
    store.for_each([&](const HttpSessionState& state) {
        ids.insert(state.session_id);
    });
    CHECK(ids.size() == 2);
    CHECK(ids.count(1) == 1);
    CHECK(ids.count(2) == 1);
}

TEST_CASE("for_each_mut can modify sessions", "[http][token_store]") {
    TokenStore store;
    store.create_session(make_pubkey(0x01), make_namespace(0x01));

    store.for_each_mut([&](HttpSessionState& state) {
        state.client_pubkey[0] = 0xFF;
    });

    auto* s = store.lookup_by_id(1);
    REQUIRE(s != nullptr);
    CHECK(s->client_pubkey[0] == 0xFF);
}

// ============================================================================
// rate_limiter integration
// ============================================================================

TEST_CASE("create_session with rate_limit sets rate limiter", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace(), 100);

    auto* state = store.lookup(token);
    REQUIRE(state != nullptr);
    CHECK(state->rate_limiter.current_rate() == 100);
}

TEST_CASE("create_session with zero rate_limit disables limiter", "[http][token_store]") {
    TokenStore store;
    auto token = store.create_session(make_pubkey(), make_namespace(), 0);

    auto* state = store.lookup(token);
    REQUIRE(state != nullptr);
    CHECK(state->rate_limiter.current_rate() == 0);
    CHECK(state->rate_limiter.try_consume() == true);  // Disabled = always passes
}

// ============================================================================
// Multiple sessions stress
// ============================================================================

TEST_CASE("many sessions created and looked up correctly", "[http][token_store]") {
    TokenStore store;
    std::vector<std::string> tokens;
    for (int i = 0; i < 50; ++i) {
        tokens.push_back(store.create_session(
            make_pubkey(static_cast<uint8_t>(i)),
            make_namespace(static_cast<uint8_t>(i))));
    }
    CHECK(store.count() == 50);

    // All tokens unique
    std::set<std::string> unique_tokens(tokens.begin(), tokens.end());
    CHECK(unique_tokens.size() == 50);

    // All lookups work
    for (size_t i = 0; i < tokens.size(); ++i) {
        auto* state = store.lookup(tokens[i]);
        REQUIRE(state != nullptr);
        CHECK(state->session_id == i + 1);
    }
}
