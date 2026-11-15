#include "exchange/client/cli.hpp"

#include "exchange/server/server.hpp"
#include "support/test_client.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <array>
#include <sstream>
#include <string>
#include <thread>

// Runs the real client loop against a real server. The client's "keyboard" is
// a pipe that the test writes a script into.

namespace exchange::client {
namespace {

// A file descriptor from which the given text, then the end of input, is read.
int input_containing(const std::string& text) {
    std::array<int, 2> ends{};
    EXPECT_EQ(::pipe(ends.data()), 0);
    EXPECT_EQ(::write(ends[1], text.data(), text.size()), static_cast<ssize_t>(text.size()));
    ::close(ends[1]);
    return ends[0];
}

class CliTest : public ::testing::Test {
public:
    CliTest(const CliTest&) = delete;
    CliTest& operator=(const CliTest&) = delete;

protected:
    CliTest() : server_(server::ServerConfig{.port = 0}), thread_([this] { server_.run(); }) {}
    ~CliTest() override { server_.request_stop(); }

    // Runs the client on a script and returns everything it printed.
    std::string run_script(const std::string& script, int expected_status = 0) const {
        const int input = input_containing(script);
        std::ostringstream output;
        EXPECT_EQ(run_cli(CliOptions{.port = server_.port()}, input, output), expected_status);
        ::close(input);
        return output.str();
    }

    server::Server server_;
    std::jthread thread_;
};

TEST_F(CliTest, ScriptProducesAFullTranscript) {
    // Another participant, with two sell orders resting in the book.
    test::TestClient maker(server_.port());
    maker.hello();
    maker.send(
        protocol::NewOrder{.client_order_id = 1, .side = Side::Sell, .price = 101, .quantity = 5});
    maker.send(
        protocol::NewOrder{.client_order_id = 2, .side = Side::Sell, .price = 102, .quantity = 10});
    maker.expect<protocol::OrderAccepted>();
    maker.expect<protocol::OrderAccepted>();

    const std::string transcript = run_script("book\n"
                                              "buy 102 8\n"
                                              "buy 99 4\n"
                                              "book\n"
                                              "orders\n"
                                              "cancel 4\n"
                                              "cancel 4\n"
                                              "buy 0 1\n");

    EXPECT_EQ(transcript, "session 2 opened\n"
                          "> book\n"
                          "                BIDS |       ASKS\n"
                          "  quantity     price |      price  quantity\n"
                          "                     |        101         5\n"
                          "                     |        102        10\n"
                          "> buy 102 8\n"
                          "order 3 accepted: buy 8 @ 102\n"
                          "order 3 filled: 5 @ 101, 3 remaining\n"
                          "market trade: 5 @ 101, buy side aggressor\n"
                          "order 3 filled: 3 @ 102, complete\n"
                          "market trade: 3 @ 102, buy side aggressor\n"
                          "> buy 99 4\n"
                          "order 4 accepted: buy 4 @ 99\n"
                          "> book\n"
                          "                BIDS |       ASKS\n"
                          "  quantity     price |      price  quantity\n"
                          "         4        99 |        102         7\n"
                          "> orders\n"
                          "order 4: buy 4 @ 99 remaining\n"
                          "> cancel 4\n"
                          "order 4 cancelled: 4 not filled\n"
                          "> cancel 4\n"
                          "cancel rejected: order 4 is not one of your active orders\n"
                          "> buy 0 1\n"
                          "order rejected (invalid price): buy 1 @ 0\n");
}

TEST_F(CliTest, QuitStopsBeforeTheEndOfTheScript) {
    EXPECT_EQ(run_script("buy 100 1\nquit\nbuy 100 2\n"), "session 1 opened\n"
                                                          "> buy 100 1\n"
                                                          "order 1 accepted: buy 1 @ 100\n"
                                                          "> quit\n");
}

TEST_F(CliTest, LastLineWithoutALineBreakIsStillExecuted) {
    EXPECT_EQ(run_script("buy 100 1"), "session 1 opened\n"
                                       "> buy 100 1\n"
                                       "order 1 accepted: buy 1 @ 100\n");
}

TEST_F(CliTest, EmptyInputOnlyOpensASession) {
    EXPECT_EQ(run_script(""), "session 1 opened\n");
}

TEST_F(CliTest, LeavingCancelsTheClientsOrders) {
    test::TestClient watcher(server_.port());
    watcher.hello();
    watcher.send(protocol::Subscribe{});
    watcher.expect<protocol::SnapshotBegin>();
    watcher.expect<protocol::SnapshotEnd>();

    run_script("buy 100 3\n");

    EXPECT_EQ(watcher.expect<protocol::BookUpdate>().total_quantity, 3U);
    EXPECT_EQ(watcher.expect<protocol::BookUpdate>().total_quantity, 0U);
}

TEST(CliConnection, ReportsAServerThatCannotBeReached) {
    // Find a port with nothing listening on it: open a server, note its port, close it.
    std::uint16_t free_port = 0;
    {
        const server::Server server(server::ServerConfig{.port = 0});
        free_port = server.port();
    }
    const int input = input_containing("book\n");
    std::ostringstream output;

    EXPECT_EQ(run_cli(CliOptions{.port = free_port}, input, output), 1);
    ::close(input);

    EXPECT_NE(output.str().find("cannot connect"), std::string::npos);
}

TEST(CliConnection, ReportsAServerThatGoesAway) {
    auto server = std::make_unique<server::Server>(server::ServerConfig{.port = 0});
    const std::uint16_t port = server->port();
    std::jthread server_thread([&server] { server->run(); });

    // A pipe that stays open: the client is waiting for a command when the
    // server stops.
    std::array<int, 2> ends{};
    ASSERT_EQ(::pipe(ends.data()), 0);
    std::ostringstream output;
    int status = -1;
    std::jthread client_thread(
        [&] { status = run_cli(CliOptions{.port = port}, ends[0], output); });

    // Wait until the client has a session, then take the server away.
    test::TestClient probe(port);
    probe.hello();
    server->request_stop();
    server_thread.join();
    server.reset();
    client_thread.join();
    ::close(ends[0]);
    ::close(ends[1]);

    EXPECT_EQ(status, 1);
    EXPECT_NE(output.str().find("the server closed the connection"), std::string::npos);
}

} // namespace
} // namespace exchange::client
