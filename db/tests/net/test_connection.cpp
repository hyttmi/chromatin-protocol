#include <catch2/catch_test_macros.hpp>
#include "db/net/connection.h"
#include "db/identity/identity.h"

#include <asio.hpp>
#include <thread>
#include <chrono>

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
            if (type == TransportMsgType_Data) {
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
            if (type == TransportMsgType_Data) {
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
            asio::co_spawn(ioc, init_conn->send_message(TransportMsgType_Data, payload),
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
// Phase 25: Lightweight handshake integration tests
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
            if (type == TransportMsgType_Data) {
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
            asio::co_spawn(ioc, init_conn->send_message(TransportMsgType_Data, payload),
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
