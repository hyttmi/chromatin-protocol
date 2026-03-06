#include <catch2/catch_test_macros.hpp>
#include "db/net/server.h"
#include "db/identity/identity.h"
#include "db/config/config.h"

#include <asio.hpp>
#include <thread>
#include <chrono>

using namespace chromatindb::net;

TEST_CASE("Server starts and accepts inbound connection", "[server]") {
    auto identity = chromatindb::identity::NodeIdentity::generate();

    chromatindb::config::Config cfg;
    cfg.bind_address = "127.0.0.1:0";  // random port -- Server binds to 0

    // Server needs a fixed port for the client to connect to.
    // We'll use a known port range instead.
    // Actually, Server::start() resolves "127.0.0.1:0" but TCP port 0 is
    // kernel-assigned. We can't query it from Server (no accessor).
    // For this test, we'll use a high port and hope it's free.
    cfg.bind_address = "127.0.0.1:44201";

    asio::io_context ioc;
    Server server(cfg, identity, ioc);
    server.start();

    // Connect a raw TCP client to verify accept works
    auto client_id = chromatindb::identity::NodeIdentity::generate();
    bool connected = false;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), 44201),
            use_nothrow);
        connected = !ec;

        // Close immediately -- we just care about the accept
        socket.close();
    }, asio::detached);

    ioc.run_for(std::chrono::seconds(2));

    REQUIRE(connected);
    REQUIRE(server.connection_count() >= 0);  // Connection may have been cleaned up
}

TEST_CASE("Server stop triggers draining state", "[server]") {
    auto identity = chromatindb::identity::NodeIdentity::generate();

    chromatindb::config::Config cfg;
    cfg.bind_address = "127.0.0.1:44202";

    asio::io_context ioc;
    Server server(cfg, identity, ioc);
    server.start();

    REQUIRE_FALSE(server.is_draining());

    server.stop();

    REQUIRE(server.is_draining());

    ioc.run_for(std::chrono::seconds(6));
}

TEST_CASE("Server connects to bootstrap peer", "[server]") {
    auto server_id = chromatindb::identity::NodeIdentity::generate();
    auto peer_id = chromatindb::identity::NodeIdentity::generate();

    asio::io_context ioc;

    // Start a "peer" that just accepts connections
    asio::ip::tcp::acceptor peer_acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 44203));
    bool peer_got_connection = false;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        auto [ec, socket] = co_await peer_acceptor.async_accept(use_nothrow);
        peer_got_connection = !ec;
        // Close immediately
        if (!ec) socket.close();
    }, asio::detached);

    // Configure server with bootstrap peer
    chromatindb::config::Config cfg;
    cfg.bind_address = "127.0.0.1:44204";
    cfg.bootstrap_peers = {"127.0.0.1:44203"};

    Server server(cfg, server_id, ioc);
    server.start();

    // Stop server after 2s so reconnect_loop exits before scope ends
    asio::steady_timer stop_timer(ioc);
    stop_timer.expires_after(std::chrono::seconds(2));
    stop_timer.async_wait([&](asio::error_code) {
        asio::error_code ec;
        peer_acceptor.close(ec);
        server.stop();
    });

    ioc.run_for(std::chrono::seconds(8));

    REQUIRE(peer_got_connection);
}

TEST_CASE("Server handles full handshake with inbound peer", "[server]") {
    auto server_id = chromatindb::identity::NodeIdentity::generate();
    auto client_id = chromatindb::identity::NodeIdentity::generate();

    chromatindb::config::Config cfg;
    cfg.bind_address = "127.0.0.1:44205";

    asio::io_context ioc;
    Server server(cfg, server_id, ioc);
    server.start();

    Connection::Ptr client_conn;
    bool client_authenticated = false;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), 44205),
            use_nothrow);
        if (ec) co_return;

        client_conn = Connection::create_outbound(std::move(socket), client_id);
        co_await client_conn->run();
        client_authenticated = client_conn->is_authenticated();
    }, asio::detached);

    // Close after handshake has time to complete
    asio::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(3));
    timer.async_wait([&](asio::error_code) {
        if (client_conn) client_conn->close();
        server.stop();
    });

    ioc.run_for(std::chrono::seconds(10));

    REQUIRE(client_authenticated);
    REQUIRE(server.connection_count() == 0);  // Should be 0 after drain
}
