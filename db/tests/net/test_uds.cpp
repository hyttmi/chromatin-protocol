#include <catch2/catch_test_macros.hpp>
#include "db/net/uds_acceptor.h"
#include "db/net/connection.h"
#include "db/identity/identity.h"

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace chromatindb;
using namespace chromatindb::net;

namespace {

/// Generate a unique temp socket path using PID and a counter.
std::string temp_socket_path(const std::string& suffix = "") {
    return (std::filesystem::temp_directory_path() /
            ("chromatindb_test_uds_" + std::to_string(::getpid()) + suffix + ".sock")).string();
}

} // namespace

TEST_CASE("UdsAcceptor binds and accepts connection", "[uds]") {
    asio::io_context ioc;
    auto server_id = identity::NodeIdentity::generate();
    auto client_id = identity::NodeIdentity::generate();
    auto path = temp_socket_path("_accept");

    // Clean up any leftover from previous run
    std::filesystem::remove(path);

    UdsAcceptor acceptor(path, server_id, ioc);

    bool connected = false;
    Connection::Ptr server_conn;
    acceptor.set_on_connected([&](Connection::Ptr c) {
        connected = true;
        server_conn = c;
    });
    acceptor.start();

    // Client connects
    asio::local::stream_protocol::socket client_sock(ioc);
    client_sock.connect(asio::local::stream_protocol::endpoint(path));
    auto client_conn = Connection::create_uds_outbound(std::move(client_sock), client_id);

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        co_await client_conn->run();
    }, asio::detached);

    ioc.run_for(std::chrono::seconds(5));

    REQUIRE(connected);
    REQUIRE(client_conn->is_authenticated());
    REQUIRE(client_conn->is_uds());

    // Cleanup
    acceptor.stop();
    std::filesystem::remove(path);
}

TEST_CASE("UdsAcceptor unlinks stale socket", "[uds]") {
    auto path = temp_socket_path("_stale");

    // Create a stale file at the socket path
    {
        std::ofstream f(path);
        f << "stale";
    }
    REQUIRE(std::filesystem::exists(path));

    asio::io_context ioc;
    auto id = identity::NodeIdentity::generate();

    UdsAcceptor acceptor(path, id, ioc);
    // start() should unlink the stale file and bind successfully
    REQUIRE_NOTHROW(acceptor.start());
    REQUIRE(acceptor.is_running());

    // Socket file should exist (re-created by bind)
    REQUIRE(std::filesystem::exists(path));

    acceptor.stop();
    std::filesystem::remove(path);
}

TEST_CASE("UdsAcceptor cleans up socket on stop", "[uds]") {
    auto path = temp_socket_path("_cleanup");
    std::filesystem::remove(path);

    asio::io_context ioc;
    auto id = identity::NodeIdentity::generate();

    UdsAcceptor acceptor(path, id, ioc);
    acceptor.start();

    // Socket file should exist after start
    REQUIRE(std::filesystem::exists(path));

    acceptor.stop();

    // Socket file should be removed after stop
    REQUIRE_FALSE(std::filesystem::exists(path));
}
