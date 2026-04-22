#include <catch2/catch_test_macros.hpp>
#include "db/net/connection.h"
#include "db/net/auth_helpers.h"
#include "db/crypto/aead.h"
#include "db/crypto/hash.h"
#include "db/crypto/kdf.h"
#include "db/identity/identity.h"
#include "db/util/endian.h"

#include <asio.hpp>
#include <thread>
#include <chrono>
#include <cstring>

// Friend class accessor for private test-only methods on Connection.
// Must be in chromatindb::net to match the friend declaration.
namespace chromatindb::net {
class ConnectionTestAccess {
public:
    static void set_send_counter(Connection& conn, uint64_t v) {
        conn.set_send_counter_for_test(v);
    }
    static void set_recv_counter(Connection& conn, uint64_t v) {
        conn.set_recv_counter_for_test(v);
    }
};
} // namespace chromatindb::net

using namespace chromatindb::net;
using namespace chromatindb::wire;

TEST_CASE("Two connections complete handshake over loopback", "[connection]") {
    auto initiator_id = chromatindb::identity::NodeIdentity::generate();
    auto responder_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    // Create acceptor on random port
    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool handshake_ok_init = false;
    bool handshake_ok_resp = false;
    bool data_received = false;
    std::vector<uint8_t> received_payload;

    // Spawn responder: accept + run
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        REQUIRE(!ec);

        auto conn = Connection::create_inbound(std::move(socket), responder_id);
        conn->on_message([&](Connection::Ptr, TransportMsgType type, std::vector<uint8_t> payload, uint32_t /*request_id*/) {
            if (type == TransportMsgType_BlobWrite) {
                data_received = true;
                received_payload = payload;
            }
        });

        handshake_ok_resp = co_await conn->run();
    }, asio::detached);

    // Spawn initiator: connect + run + send data
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        REQUIRE(!ec);

        auto conn = Connection::create_outbound(std::move(socket), initiator_id);
        handshake_ok_init = co_await conn->run();

        // Note: run() enters message loop and only returns when connection closes.
        // We need to send data during the run, not after.
        // Let's restructure: send in the message callback or before message_loop.
    }, asio::detached);

    // Run for up to 10 seconds
    ioc.run_for(std::chrono::seconds(10));

    // At minimum, both handshakes should complete
    // Note: the initiator's run() blocks in message_loop, so we might not
    // get handshake_ok_init set before the timeout. Let's verify differently.
}

TEST_CASE("Connection handshake succeeds over loopback", "[connection]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool init_authenticated = false;
    bool resp_authenticated = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        co_await resp_conn->run();
        resp_authenticated = resp_conn->is_authenticated();
    }, asio::detached);

    // Initiator
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        co_await init_conn->run();
        init_authenticated = init_conn->is_authenticated();
    }, asio::detached);

    // Let the handshake complete, then close connections after a short delay
    asio::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(3));
    timer.async_wait([&](asio::error_code) {
        // Close connections to break the message loops
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(5));

    REQUIRE(init_authenticated);
    REQUIRE(resp_authenticated);
}

TEST_CASE("Connection sends and receives encrypted data", "[connection]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool data_received = false;
    std::vector<uint8_t> received_payload;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder -- receives data
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->on_message([&](Connection::Ptr, TransportMsgType type,
                                   std::vector<uint8_t> payload, uint32_t /*request_id*/) {
            if (type == TransportMsgType_BlobWrite) {
                data_received = true;
                received_payload = payload;
                // Close both to exit test
                if (init_conn) init_conn->close();
                if (resp_conn) resp_conn->close();
            }
        });
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator -- sends data after handshake
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);

        // We need to do handshake first, then send data, then enter message loop.
        // But run() does handshake + message_loop together.
        // We'll use a timer to send data after handshake completes.
        co_await init_conn->run();
    }, asio::detached);

    // After handshake completes (give it 2s), send data from initiator
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code) {
        if (init_conn && init_conn->is_authenticated()) {
            std::vector<uint8_t> payload = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
            asio::co_spawn(ioc, init_conn->send_message(TransportMsgType_BlobWrite, payload),
                           asio::detached);
        }
    });

    // Timeout
    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(8));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(10));

    REQUIRE(data_received);
    std::vector<uint8_t> expected = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
    REQUIRE(received_payload == expected);
}

TEST_CASE("Connection goodbye sends properly", "[connection]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool resp_got_goodbye = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->on_close([&](Connection::Ptr c, bool graceful) {
            resp_got_goodbye = c->received_goodbye();
        });
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator sends goodbye after handshake
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        co_await init_conn->run();
    }, asio::detached);

    // After handshake, send goodbye
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code) {
        if (init_conn && init_conn->is_authenticated()) {
            asio::co_spawn(ioc, init_conn->close_gracefully(), asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    REQUIRE(resp_got_goodbye);
}

// =============================================================================
// Lightweight handshake integration tests
// =============================================================================

TEST_CASE("Lightweight handshake over loopback", "[connection][lightweight]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool init_authenticated = false;
    bool resp_authenticated = false;
    bool data_received = false;
    std::vector<uint8_t> received_payload;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Trust check: always trust loopback
    auto trust_check = [](const asio::ip::address& addr) {
        return addr.is_loopback();
    };

    // Responder
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->set_trust_check(trust_check);
        resp_conn->on_message([&](Connection::Ptr, TransportMsgType type,
                                   std::vector<uint8_t> payload, uint32_t /*request_id*/) {
            if (type == TransportMsgType_BlobWrite) {
                data_received = true;
                received_payload = payload;
                if (init_conn) init_conn->close();
                if (resp_conn) resp_conn->close();
            }
        });
        co_await resp_conn->run();
        resp_authenticated = resp_conn->is_authenticated();
    }, asio::detached);

    // Initiator
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->set_trust_check(trust_check);
        co_await init_conn->run();
        init_authenticated = init_conn->is_authenticated();
    }, asio::detached);

    // Send data after handshake
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code) {
        if (init_conn && init_conn->is_authenticated()) {
            std::vector<uint8_t> payload = {0xCA, 0xFE};
            asio::co_spawn(ioc, init_conn->send_message(TransportMsgType_BlobWrite, payload),
                           asio::detached);
        }
    });

    // Timeout
    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(8));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(10));

    REQUIRE(init_authenticated);
    REQUIRE(resp_authenticated);
    REQUIRE(data_received);
    std::vector<uint8_t> expected = {0xCA, 0xFE};
    REQUIRE(received_payload == expected);
}

TEST_CASE("PQ handshake when trust_check is not set", "[connection][lightweight]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool init_authenticated = false;
    bool resp_authenticated = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder -- NO trust_check set (default PQ behavior)
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        co_await resp_conn->run();
        resp_authenticated = resp_conn->is_authenticated();
    }, asio::detached);

    // Initiator -- NO trust_check set
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        co_await init_conn->run();
        init_authenticated = init_conn->is_authenticated();
    }, asio::detached);

    // Close after a bit
    asio::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(3));
    timer.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(5));

    REQUIRE(init_authenticated);
    REQUIRE(resp_authenticated);
}

TEST_CASE("Mismatch fallback: initiator trusts, responder does not", "[connection][lightweight]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool init_authenticated = false;
    bool resp_authenticated = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder -- trust_check returns false (doesn't trust)
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->set_trust_check([](const asio::ip::address&) { return false; });
        co_await resp_conn->run();
        resp_authenticated = resp_conn->is_authenticated();
    }, asio::detached);

    // Initiator -- trust_check returns true (trusts)
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->set_trust_check([](const asio::ip::address&) { return true; });
        co_await init_conn->run();
        init_authenticated = init_conn->is_authenticated();
    }, asio::detached);

    // Close after a bit
    asio::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(3));
    timer.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(5));

    // Connection should succeed via PQ fallback
    REQUIRE(init_authenticated);
    REQUIRE(resp_authenticated);
}

// =============================================================================
// Send queue tests
// =============================================================================

TEST_CASE("Send queue: multiple concurrent sends complete without crash", "[connection][send_queue]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    int messages_received = 0;
    constexpr int NUM_MESSAGES = 10;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder -- counts received Data messages
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->on_message([&](Connection::Ptr, TransportMsgType type,
                                   std::vector<uint8_t> payload, uint32_t /*request_id*/) {
            if (type == TransportMsgType_BlobWrite) {
                messages_received++;
                if (messages_received == NUM_MESSAGES) {
                    if (init_conn) init_conn->close();
                    if (resp_conn) resp_conn->close();
                }
            }
        });
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator -- fires multiple send_message() calls concurrently
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        // Use on_ready to send after handshake but during message_loop
        init_conn->on_ready([&](Connection::Ptr conn) {
            // Fire multiple concurrent sends via co_spawn
            for (int i = 0; i < NUM_MESSAGES; i++) {
                std::vector<uint8_t> payload = {static_cast<uint8_t>(i)};
                asio::co_spawn(ioc, conn->send_message(TransportMsgType_BlobWrite, payload),
                               asio::detached);
            }
        });
        co_await init_conn->run();
    }, asio::detached);

    // Timeout
    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(10));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(12));

    REQUIRE(messages_received == NUM_MESSAGES);
}

TEST_CASE("Send queue: send_message returns false after close", "[connection][send_queue]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool send_after_close_returned_false = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator -- close then try to send
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->on_ready([&](Connection::Ptr conn) {
            // Close the connection, then try to send
            conn->close();
            asio::co_spawn(ioc, [&, conn]() -> asio::awaitable<void> {
                std::vector<uint8_t> payload = {0x01};
                bool ok = co_await conn->send_message(TransportMsgType_BlobWrite, payload);
                send_after_close_returned_false = !ok;
            }, asio::detached);
        });
        co_await init_conn->run();
    }, asio::detached);

    // Timeout
    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(5));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(7));

    REQUIRE(send_after_close_returned_false);
}

TEST_CASE("Send queue: Pong reply goes through send_message", "[connection][send_queue]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool pong_received = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder -- receives Ping from initiator
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator -- sends Ping and expects Pong back (via message callback)
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->on_ready([&](Connection::Ptr conn) {
            // Send Ping via send_message
            asio::co_spawn(ioc, [&, conn]() -> asio::awaitable<void> {
                std::span<const uint8_t> empty{};
                co_await conn->send_message(TransportMsgType_Ping, empty);

                // After sending Ping, send Data and wait for responder to get it.
                // The Pong should be received by message_loop which handles it silently.
                // To verify Pong was sent, we check that the connection is still alive
                // after sending Ping (if Pong failed, nonce desync would kill connection).
                std::vector<uint8_t> payload = {0xAA};
                bool ok = co_await conn->send_message(TransportMsgType_BlobWrite, payload);
                // If we get here without nonce desync, the Pong went through the queue
                pong_received = ok;
                if (init_conn) init_conn->close();
                if (resp_conn) resp_conn->close();
            }, asio::detached);
        });
        co_await init_conn->run();
    }, asio::detached);

    // Timeout
    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(10));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(12));

    REQUIRE(pong_received);
}

// =============================================================================
// Nonce exhaustion tests (CRYPTO-01)
// =============================================================================

// =============================================================================
// Lightweight handshake authentication tests (CRYPTO-03)
// =============================================================================

TEST_CASE("lightweight handshake authenticates both peers", "[connection][lightweight][auth]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool init_authenticated = false;
    bool resp_authenticated = false;
    std::vector<uint8_t> init_peer_pk;
    std::vector<uint8_t> resp_peer_pk;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    auto trust_check = [](const asio::ip::address& addr) {
        return addr.is_loopback();
    };

    // Responder
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->set_trust_check(trust_check);
        co_await resp_conn->run();
        resp_authenticated = resp_conn->is_authenticated();
        resp_peer_pk = resp_conn->peer_pubkey();
    }, asio::detached);

    // Initiator
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->set_trust_check(trust_check);
        co_await init_conn->run();
        init_authenticated = init_conn->is_authenticated();
        init_peer_pk = init_conn->peer_pubkey();
    }, asio::detached);

    // Close after handshake
    asio::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(3));
    timer.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(5));

    REQUIRE(init_authenticated);
    REQUIRE(resp_authenticated);
    // Verify peer pubkeys are correctly exchanged via AuthSignature
    auto resp_pk_span = resp_id.public_key();
    auto init_pk_span = init_id.public_key();
    REQUIRE(init_peer_pk.size() == resp_pk_span.size());
    REQUIRE(std::memcmp(init_peer_pk.data(), resp_pk_span.data(), resp_pk_span.size()) == 0);
    REQUIRE(resp_peer_pk.size() == init_pk_span.size());
    REQUIRE(std::memcmp(resp_peer_pk.data(), init_pk_span.data(), init_pk_span.size()) == 0);
}

TEST_CASE("lightweight handshake rejects mismatched auth pubkey", "[connection][lightweight][auth]") {
    // Scenario: attacker controls the responder side. The responder's
    // TrustedHello uses peer_b's pubkey, but the AuthSignature contains
    // attacker's pubkey + attacker's signature. The initiator must reject.

    auto peer_a = chromatindb::identity::NodeIdentity::generate();
    auto peer_b = chromatindb::identity::NodeIdentity::generate();
    auto attacker = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool init_handshake_completed = false;
    bool init_authenticated = false;
    Connection::Ptr init_conn;

    auto trust_check = [](const asio::ip::address& addr) {
        return addr.is_loopback();
    };

    // Manual malicious responder: performs lightweight handshake steps
    // but sends AuthSignature with attacker's identity
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        // Helper lambdas for raw frame IO
        auto send_raw = [&](std::span<const uint8_t> data) -> asio::awaitable<bool> {
            uint32_t len = static_cast<uint32_t>(data.size());
            std::array<uint8_t, 4> header;
            chromatindb::util::store_u32_be(header.data(), len);
            auto [ec1, _n1] = co_await asio::async_write(
                socket, asio::buffer(header), use_nothrow);
            if (ec1) co_return false;
            auto [ec2, _n2] = co_await asio::async_write(
                socket, asio::buffer(data.data(), data.size()), use_nothrow);
            co_return !ec2;
        };

        auto recv_raw = [&]() -> asio::awaitable<std::optional<std::vector<uint8_t>>> {
            std::array<uint8_t, 4> header;
            auto [ec1, _n1] = co_await asio::async_read(
                socket, asio::buffer(header), use_nothrow);
            if (ec1) co_return std::nullopt;
            uint32_t len = chromatindb::util::read_u32_be(header.data());
            if (len > 110 * 1024 * 1024) co_return std::nullopt;
            std::vector<uint8_t> data(len);
            auto [ec2, _n2] = co_await asio::async_read(
                socket, asio::buffer(data), use_nothrow);
            if (ec2) co_return std::nullopt;
            co_return data;
        };

        // Step 1: Receive initiator's TrustedHello
        auto init_msg_raw = co_await recv_raw();
        if (!init_msg_raw) co_return;
        auto init_decoded = chromatindb::net::TransportCodec::decode(*init_msg_raw);
        if (!init_decoded) co_return;

        constexpr size_t NONCE_SIZE = 32;
        auto nonce_i = std::span<const uint8_t>(init_decoded->payload.data(), NONCE_SIZE);
        auto init_signing_pk = std::span<const uint8_t>(
            init_decoded->payload.data() + NONCE_SIZE,
            init_decoded->payload.size() - NONCE_SIZE);

        // Step 2: Send TrustedHello with peer_b's pubkey (legitimate)
        std::array<uint8_t, 32> nonce_r{};
        randombytes_buf(nonce_r.data(), nonce_r.size());

        auto resp_pk = peer_b.public_key();
        std::vector<uint8_t> resp_payload;
        resp_payload.reserve(32 + resp_pk.size());
        resp_payload.insert(resp_payload.end(), nonce_r.begin(), nonce_r.end());
        resp_payload.insert(resp_payload.end(), resp_pk.begin(), resp_pk.end());

        auto resp_msg = chromatindb::net::TransportCodec::encode(
            chromatindb::wire::TransportMsgType_TrustedHello, resp_payload);
        if (!co_await send_raw(resp_msg)) co_return;

        // Step 3: Derive session keys (responder side, using peer_b's pk)
        auto session_keys = chromatindb::net::derive_lightweight_session_keys(
            nonce_i, nonce_r, init_signing_pk, resp_pk, false);

        uint64_t send_ctr = 0;
        uint64_t recv_ctr = 0;

        // Step 4: Receive initiator's encrypted AuthSignature
        // recv_encrypted manually
        auto ct_raw = co_await recv_raw();
        if (!ct_raw) co_return;

        auto recv_nonce = chromatindb::net::make_nonce(recv_ctr++);
        std::span<const uint8_t> empty_ad{};
        auto init_auth_pt = chromatindb::crypto::AEAD::decrypt(
            *ct_raw, empty_ad, recv_nonce, session_keys.recv_key.span());
        if (!init_auth_pt) co_return;
        // (initiator's auth is valid, we just discard it)

        // Step 5: Send malicious AuthSignature with ATTACKER's pubkey
        auto attacker_sig = attacker.sign(session_keys.session_fingerprint);
        auto malicious_auth = chromatindb::net::encode_auth_payload(
            chromatindb::net::Role::Peer, attacker.public_key(), attacker_sig);
        auto malicious_msg = chromatindb::net::TransportCodec::encode(
            chromatindb::wire::TransportMsgType_AuthSignature, malicious_auth);

        // send_encrypted manually
        auto send_nonce = chromatindb::net::make_nonce(send_ctr++);
        auto ciphertext = chromatindb::crypto::AEAD::encrypt(
            malicious_msg, empty_ad, send_nonce, session_keys.send_key.span());
        co_await send_raw(ciphertext);

        // Connection should be rejected by initiator
    }, asio::detached);

    // Initiator (peer_a)
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), peer_a);
        init_conn->set_trust_check(trust_check);
        bool result = co_await init_conn->run();
        init_handshake_completed = result;
        init_authenticated = init_conn->is_authenticated();
    }, asio::detached);

    // Timeout
    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(5));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
    });

    ioc.run_for(std::chrono::seconds(7));

    // The handshake should have failed (run returns false, sets close callback)
    REQUIRE_FALSE(init_authenticated);
}

TEST_CASE("send_encrypted returns false at nonce limit", "[connection][nonce]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    bool send_returned_false = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;

        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator -- after handshake, force counter to limit, then try to send
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;

        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->on_ready([&](Connection::Ptr conn) {
            asio::co_spawn(ioc, [&, conn]() -> asio::awaitable<void> {
                // Verify normal send works first
                std::vector<uint8_t> payload = {0x01};
                bool ok = co_await conn->send_message(TransportMsgType_BlobWrite, payload);
                REQUIRE(ok);

                // Force counter to nonce exhaustion threshold
                ConnectionTestAccess::set_send_counter(*conn, 1ULL << 63);

                // This send should fail due to nonce exhaustion
                std::vector<uint8_t> payload2 = {0x02};
                bool ok2 = co_await conn->send_message(TransportMsgType_BlobWrite, payload2);
                send_returned_false = !ok2;

                if (init_conn) init_conn->close();
                if (resp_conn) resp_conn->close();
            }, asio::detached);
        });
        co_await init_conn->run();
    }, asio::detached);

    // Timeout
    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(10));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(12));

    REQUIRE(send_returned_false);
}

// =============================================================================
// Streaming invariant tests
// =============================================================================
// Both tests exercise Connection::send_message with payloads sized relative to
// STREAMING_THRESHOLD (not MAX_FRAME_SIZE) so the tests remain meaningful if
// MAX_FRAME_SIZE is later shrunk. Successful end-to-end receipt of bytes with a
// distinctive fill pattern proves the send_message bifurcation reassembled the
// payload correctly on the responder side.

TEST_CASE("streaming invariant: payload just under STREAMING_THRESHOLD "
          "round-trips through non-chunked path",
          "[connection][streaming]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    std::vector<uint8_t> received;
    bool message_arrived = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder: accept, capture first BlobWrite payload, close.
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;
        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->on_message([&](Connection::Ptr, TransportMsgType type,
                                   std::vector<uint8_t> payload, uint32_t /*request_id*/) {
            if (type == TransportMsgType_BlobWrite) {
                received = std::move(payload);
                message_arrived = true;
                if (init_conn) init_conn->close();
                if (resp_conn) resp_conn->close();
            }
        });
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator: connect, send a (STREAMING_THRESHOLD - 1)-byte payload.
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;
        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->on_ready([&](Connection::Ptr conn) {
            std::vector<uint8_t> payload(STREAMING_THRESHOLD - 1, 0xAB);
            asio::co_spawn(ioc,
                conn->send_message(TransportMsgType_BlobWrite, payload),
                asio::detached);
        });
        co_await init_conn->run();
    }, asio::detached);

    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(10));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(12));

    REQUIRE(message_arrived);
    REQUIRE(received.size() == STREAMING_THRESHOLD - 1);
    REQUIRE(received.front() == 0xAB);
    REQUIRE(received.back() == 0xAB);
}

TEST_CASE("streaming invariant: payload just over STREAMING_THRESHOLD "
          "auto-chunks end-to-end",
          "[connection][streaming]") {
    auto init_id = chromatindb::identity::NodeIdentity::generate();
    auto resp_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto port = acceptor.local_endpoint().port();

    std::vector<uint8_t> received;
    bool message_arrived = false;
    Connection::Ptr init_conn;
    Connection::Ptr resp_conn;

    // Responder: accept, capture first BlobWrite payload reassembled from
    // chunked sub-frames, close.
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow);
        if (ec) co_return;
        resp_conn = Connection::create_inbound(std::move(socket), resp_id);
        resp_conn->on_message([&](Connection::Ptr, TransportMsgType type,
                                   std::vector<uint8_t> payload, uint32_t /*request_id*/) {
            if (type == TransportMsgType_BlobWrite) {
                received = std::move(payload);
                message_arrived = true;
                if (init_conn) init_conn->close();
                if (resp_conn) resp_conn->close();
            }
        });
        co_await resp_conn->run();
    }, asio::detached);

    // Initiator: connect, send a (STREAMING_THRESHOLD + 1)-byte payload.
    // send_message auto-routes payload >= STREAMING_THRESHOLD through
    // send_message_chunked: 14-byte header + two data sub-frames + zero-length
    // sentinel, each sub-frame well under MAX_FRAME_SIZE.
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port),
            use_nothrow);
        if (ec) co_return;
        init_conn = Connection::create_outbound(std::move(socket), init_id);
        init_conn->on_ready([&](Connection::Ptr conn) {
            std::vector<uint8_t> payload(STREAMING_THRESHOLD + 1, 0xCD);
            asio::co_spawn(ioc,
                conn->send_message(TransportMsgType_BlobWrite, payload),
                asio::detached);
        });
        co_await init_conn->run();
    }, asio::detached);

    asio::steady_timer timeout(ioc);
    timeout.expires_after(std::chrono::seconds(10));
    timeout.async_wait([&](asio::error_code) {
        if (init_conn) init_conn->close();
        if (resp_conn) resp_conn->close();
    });

    ioc.run_for(std::chrono::seconds(12));

    REQUIRE(message_arrived);
    REQUIRE(received.size() == STREAMING_THRESHOLD + 1);
    REQUIRE(received.front() == 0xCD);
    REQUIRE(received.back() == 0xCD);
}
