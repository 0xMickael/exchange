#include "exchange/server/server.hpp"

#include "support/test_client.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// Integration tests: a real server on a local TCP port, and real client
// connections.

namespace exchange::server {
namespace {

using test::TestClient;

protocol::ClientMessage new_order(ClientOrderId client_order_id, Side side, Price price,
                                  Quantity quantity) {
    return protocol::NewOrder{
        .client_order_id = client_order_id, .side = side, .price = price, .quantity = quantity};
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> bytes;
    for (const unsigned value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

// The server configurations every test runs against. The last one has queues
// so small that they are full almost all the time, which exercises every
// backpressure path with the ordinary tests.
struct Variant {
    std::string name;
    ServerConfig config;
};

void PrintTo(const Variant& variant, std::ostream* os) { // NOLINT(readability-identifier-naming)
    *os << variant.name;
}

ServerConfig local_config(ServerMode mode, std::size_t queue_capacity = 0) {
    ServerConfig config;
    config.port = 0;
    config.mode = mode;
    if (queue_capacity != 0) {
        config.pipeline.command_queue_capacity = queue_capacity;
        config.pipeline.event_queue_capacity = queue_capacity;
    }
    return config;
}

class ServerTest : public ::testing::TestWithParam<Variant> {
public:
    ServerTest(const ServerTest&) = delete;
    ServerTest& operator=(const ServerTest&) = delete;

protected:
    ServerTest() : server_(GetParam().config), thread_([this] { server_.run(); }) {}
    ~ServerTest() override { server_.request_stop(); }

    [[nodiscard]] TestClient connect() const { return TestClient(server_.port()); }

    // A client that has completed the handshake.
    [[nodiscard]] TestClient session() const {
        TestClient client = connect();
        client.hello();
        return client;
    }

    // Checks that the server has nothing else queued for this client: a cancel
    // of an order that cannot exist must be the very next message answered.
    static void expect_nothing_pending(TestClient& client) {
        client.send(protocol::CancelOrder{.order_id = 0});
        EXPECT_EQ(client.expect<protocol::CancelRejected>().order_id, 0U);
    }

    // Expects the given protocol error, then the end of the connection.
    static void expect_protocol_error(TestClient& client, protocol::ProtocolErrorCode code) {
        EXPECT_EQ(client.expect<protocol::ProtocolError>().code, code);
        EXPECT_EQ(client.receive(), std::nullopt) << "the server must close the connection";
    }

    Server server_;
    // Declared after server_, so it is joined before the server is destroyed.
    std::jthread thread_;
};

// --- Handshake (section 7.5) ---

TEST_P(ServerTest, HandshakeGivesEachConnectionItsOwnSession) {
    TestClient first = connect();
    TestClient second = connect();

    first.send(protocol::Hello{.magic = protocol::magic, .version = protocol::version});
    const auto first_ack = first.expect<protocol::HelloAck>();
    const SessionId second_session = second.hello();

    EXPECT_EQ(first_ack.version, protocol::version);
    EXPECT_NE(first_ack.session_id, 0U);
    EXPECT_NE(first_ack.session_id, second_session);
}

// --- Orders ---

TEST_P(ServerTest, OrderIsAcknowledged) {
    TestClient client = session();

    client.send(new_order(1, Side::Buy, 100, 10));

    EXPECT_EQ(client.expect<protocol::OrderAccepted>(),
              (protocol::OrderAccepted{.client_order_id = 1, .order_id = 1}));
    expect_nothing_pending(client);
}

TEST_P(ServerTest, RejectedOrderDoesNotEndTheSession) {
    TestClient client = session();

    client.send(new_order(1, Side::Buy, 0, 10));
    EXPECT_EQ(
        client.expect<protocol::OrderRejected>(),
        (protocol::OrderRejected{.client_order_id = 1, .reason = RejectReason::InvalidPrice}));

    client.send(new_order(2, Side::Buy, 100, 10));
    EXPECT_EQ(client.expect<protocol::OrderAccepted>().client_order_id, 2U);
}

TEST_P(ServerTest, TwoClientsTradeWithEachOther) {
    TestClient seller = session();
    TestClient buyer = session();

    seller.send(new_order(1, Side::Sell, 101, 5));
    EXPECT_EQ(seller.expect<protocol::OrderAccepted>().order_id, 1U);

    buyer.send(new_order(1, Side::Buy, 101, 5));
    EXPECT_EQ(buyer.expect<protocol::OrderAccepted>().order_id, 2U);
    EXPECT_EQ(
        buyer.expect<protocol::Fill>(),
        (protocol::Fill{.order_id = 2, .price = 101, .quantity = 5, .remaining_quantity = 0}));
    EXPECT_EQ(
        seller.expect<protocol::Fill>(),
        (protocol::Fill{.order_id = 1, .price = 101, .quantity = 5, .remaining_quantity = 0}));

    // Neither subscribed to market data, so neither receives the public events.
    expect_nothing_pending(seller);
    expect_nothing_pending(buyer);
}

TEST_P(ServerTest, ClientCancelsItsOwnOrder) {
    TestClient client = session();
    client.send(new_order(1, Side::Buy, 100, 10));
    const OrderId order_id = client.expect<protocol::OrderAccepted>().order_id;

    client.send(protocol::CancelOrder{.order_id = order_id});

    EXPECT_EQ(client.expect<protocol::OrderCancelled>(),
              (protocol::OrderCancelled{.order_id = order_id,
                                        .cancelled_quantity = 10,
                                        .reason = CancelReason::Requested}));
}

TEST_P(ServerTest, ClientCannotCancelAnotherClientsOrder) {
    TestClient owner = session();
    TestClient other = session();
    owner.send(new_order(1, Side::Buy, 100, 10));
    const OrderId order_id = owner.expect<protocol::OrderAccepted>().order_id;

    other.send(protocol::CancelOrder{.order_id = order_id});

    EXPECT_EQ(other.expect<protocol::CancelRejected>(),
              (protocol::CancelRejected{.order_id = order_id,
                                        .reason = CancelRejectReason::UnknownOrder}));
    expect_nothing_pending(owner);
}

// --- Market data (section 6) ---

TEST_P(ServerTest, SubscriptionDeliversASnapshotThenNumberedUpdates) {
    TestClient trader = session();
    TestClient watcher = session();
    trader.send(new_order(1, Side::Buy, 100, 3));
    trader.send(new_order(2, Side::Sell, 105, 2));
    trader.expect<protocol::OrderAccepted>();
    trader.expect<protocol::OrderAccepted>();

    watcher.send(protocol::Subscribe{});

    // Two book updates were published before anyone listened.
    EXPECT_EQ(watcher.expect<protocol::SnapshotBegin>().last_sequence, 2U);
    EXPECT_EQ(watcher.expect<protocol::SnapshotLevel>(),
              (protocol::SnapshotLevel{.side = Side::Buy, .price = 100, .total_quantity = 3}));
    EXPECT_EQ(watcher.expect<protocol::SnapshotLevel>(),
              (protocol::SnapshotLevel{.side = Side::Sell, .price = 105, .total_quantity = 2}));
    watcher.expect<protocol::SnapshotEnd>();

    trader.send(new_order(3, Side::Buy, 100, 1));
    EXPECT_EQ(watcher.expect<protocol::BookUpdate>(),
              (protocol::BookUpdate{
                  .sequence = 3, .side = Side::Buy, .price = 100, .total_quantity = 4}));

    // A sell that takes the whole bid level, in two trades.
    trader.send(new_order(4, Side::Sell, 100, 4));
    EXPECT_EQ(watcher.expect<protocol::Trade>(),
              (protocol::Trade{
                  .sequence = 4, .price = 100, .quantity = 3, .aggressor_side = Side::Sell}));
    EXPECT_EQ(watcher.expect<protocol::Trade>(),
              (protocol::Trade{
                  .sequence = 5, .price = 100, .quantity = 1, .aggressor_side = Side::Sell}));
    EXPECT_EQ(watcher.expect<protocol::BookUpdate>(),
              (protocol::BookUpdate{
                  .sequence = 6, .side = Side::Buy, .price = 100, .total_quantity = 0}));
    expect_nothing_pending(watcher);
}

TEST_P(ServerTest, SubscriberReceivesPrivateAndPublicEventsInEngineOrder) {
    TestClient client = session();
    client.send(protocol::Subscribe{});
    EXPECT_EQ(client.expect<protocol::SnapshotBegin>().last_sequence, 0U);
    client.expect<protocol::SnapshotEnd>();

    client.send(new_order(1, Side::Buy, 100, 3));

    client.expect<protocol::OrderAccepted>();
    EXPECT_EQ(client.expect<protocol::BookUpdate>().sequence, 1U);
}

TEST_P(ServerTest, SubscribingAgainDeliversANewSnapshot) {
    TestClient client = session();
    client.send(protocol::Subscribe{});
    client.expect<protocol::SnapshotBegin>();
    client.expect<protocol::SnapshotEnd>();
    client.send(new_order(1, Side::Buy, 100, 3));
    client.expect<protocol::OrderAccepted>();
    client.expect<protocol::BookUpdate>();

    client.send(protocol::Subscribe{});

    EXPECT_EQ(client.expect<protocol::SnapshotBegin>().last_sequence, 1U);
    EXPECT_EQ(client.expect<protocol::SnapshotLevel>().total_quantity, 3U);
    client.expect<protocol::SnapshotEnd>();
}

// --- Disconnection (sections 4.3 and 8) ---

TEST_P(ServerTest, DisconnectingCancelsTheClientsOrders) {
    TestClient leaving = session();
    TestClient watcher = session();
    leaving.send(new_order(1, Side::Buy, 100, 3));
    leaving.expect<protocol::OrderAccepted>();
    watcher.send(protocol::Subscribe{});
    watcher.expect<protocol::SnapshotBegin>();
    watcher.expect<protocol::SnapshotLevel>();
    watcher.expect<protocol::SnapshotEnd>();

    leaving.close();

    EXPECT_EQ(watcher.expect<protocol::BookUpdate>(),
              (protocol::BookUpdate{
                  .sequence = 2, .side = Side::Buy, .price = 100, .total_quantity = 0}));
}

TEST_P(ServerTest, ClientLeavingInTheMiddleOfAMessageDoesNotDisturbOthers) {
    TestClient leaving = session();
    std::vector<std::byte> order;
    protocol::encode(new_order(1, Side::Buy, 100, 3), order);
    leaving.send_bytes(std::span(order).first(order.size() / 2));
    leaving.close();

    TestClient other = session();
    other.send(new_order(1, Side::Buy, 100, 3));

    EXPECT_EQ(other.expect<protocol::OrderAccepted>().order_id, 1U)
        << "the half-received order must not have been processed";
}

// --- Framing over TCP (section 7.1) ---

TEST_P(ServerTest, MessageArrivingOneByteAtATimeIsReassembled) {
    TestClient client = connect();
    std::vector<std::byte> bytes;
    protocol::encode(protocol::Hello{.magic = protocol::magic, .version = protocol::version},
                     bytes);
    protocol::encode(new_order(1, Side::Buy, 100, 3), bytes);

    for (const std::byte& byte : bytes) {
        client.send_bytes(std::span(&byte, 1));
    }

    client.expect<protocol::HelloAck>();
    EXPECT_EQ(client.expect<protocol::OrderAccepted>().client_order_id, 1U);
}

TEST_P(ServerTest, SeveralMessagesInOneWriteAreAllProcessedInOrder) {
    TestClient client = connect();
    std::vector<std::byte> bytes;
    protocol::encode(protocol::Hello{.magic = protocol::magic, .version = protocol::version},
                     bytes);
    for (ClientOrderId id = 1; id <= 3; ++id) {
        protocol::encode(new_order(id, Side::Buy, 100, 1), bytes);
    }

    client.send_bytes(bytes);

    client.expect<protocol::HelloAck>();
    for (ClientOrderId id = 1; id <= 3; ++id) {
        EXPECT_EQ(client.expect<protocol::OrderAccepted>(),
                  (protocol::OrderAccepted{.client_order_id = id, .order_id = id}));
    }
}

// --- Protocol errors (section 7.6) ---

TEST_P(ServerTest, FirstMessageMustBeHello) {
    TestClient client = connect();

    client.send(new_order(1, Side::Buy, 100, 3));

    expect_protocol_error(client, protocol::ProtocolErrorCode::HelloRequired);
}

TEST_P(ServerTest, HelloCannotBeSentTwice) {
    TestClient client = session();

    client.send(protocol::Hello{.magic = protocol::magic, .version = protocol::version});

    expect_protocol_error(client, protocol::ProtocolErrorCode::HelloRequired);
}

TEST_P(ServerTest, WrongMagicIsRefused) {
    TestClient client = connect();

    client.send(protocol::Hello{.magic = 0x12345678, .version = protocol::version});

    expect_protocol_error(client, protocol::ProtocolErrorCode::BadMagic);
}

TEST_P(ServerTest, UnsupportedVersionIsRefused) {
    TestClient client = connect();

    client.send(protocol::Hello{.magic = protocol::magic, .version = 2});

    expect_protocol_error(client, protocol::ProtocolErrorCode::UnsupportedVersion);
}

TEST_P(ServerTest, UnknownMessageTypeEndsTheSession) {
    TestClient client = session();

    client.send_bytes(bytes_of({0x00, 0x00, 0x7F}));

    expect_protocol_error(client, protocol::ProtocolErrorCode::UnknownType);
}

TEST_P(ServerTest, BadLengthEndsTheSession) {
    TestClient client = session();

    client.send_bytes(bytes_of({0xFF, 0xFF, 0x02}));

    expect_protocol_error(client, protocol::ProtocolErrorCode::BadLength);
}

TEST_P(ServerTest, UndefinedFieldValueEndsTheSession) {
    TestClient client = session();
    std::vector<std::byte> bytes;
    protocol::encode(new_order(1, Side::Buy, 100, 3), bytes);
    bytes[protocol::header_size + 8] = std::byte{9}; // the side field

    client.send_bytes(bytes);

    expect_protocol_error(client, protocol::ProtocolErrorCode::BadField);
}

TEST_P(ServerTest, ProtocolErrorIsTheLastMessageOfTheSession) {
    TestClient client = session();
    std::vector<std::byte> bytes;
    protocol::encode(new_order(1, Side::Buy, 100, 3), bytes);
    const std::vector<std::byte> garbage = bytes_of({0x00, 0x00, 0x7F});
    bytes.insert(bytes.end(), garbage.begin(), garbage.end());

    client.send_bytes(bytes);

    // The answer to the order may or may not arrive before the session ends
    // (SPEC section 7.6), but nothing may follow the error.
    std::optional<protocol::ServerMessage> message = client.receive();
    if (message.has_value() && std::holds_alternative<protocol::OrderAccepted>(*message)) {
        message = client.receive();
    }
    EXPECT_EQ(message, std::optional<protocol::ServerMessage>(protocol::ProtocolError{
                           .code = protocol::ProtocolErrorCode::UnknownType}));
    EXPECT_EQ(client.receive(), std::nullopt);
}

TEST_P(ServerTest, ProtocolErrorCancelsTheClientsOrders) {
    TestClient faulty = session();
    TestClient watcher = session();
    faulty.send(new_order(1, Side::Sell, 105, 2));
    faulty.expect<protocol::OrderAccepted>();
    watcher.send(protocol::Subscribe{});
    watcher.expect<protocol::SnapshotBegin>();
    watcher.expect<protocol::SnapshotLevel>();
    watcher.expect<protocol::SnapshotEnd>();

    faulty.send_bytes(bytes_of({0x00, 0x00, 0x7F}));

    EXPECT_EQ(watcher.expect<protocol::BookUpdate>(),
              (protocol::BookUpdate{
                  .sequence = 2, .side = Side::Sell, .price = 105, .total_quantity = 0}));
}

// --- Several clients, start and stop ---

TEST_P(ServerTest, ManyClientsAreServedAtTheSameTime) {
    constexpr std::size_t client_count = 50;
    std::vector<TestClient> clients;
    clients.reserve(client_count);
    for (std::size_t i = 0; i < client_count; ++i) {
        clients.push_back(connect());
    }
    // All handshakes and orders are sent before any answer is read.
    for (TestClient& client : clients) {
        client.send(protocol::Hello{.magic = protocol::magic, .version = protocol::version});
        client.send(new_order(1, Side::Buy, 100, 1));
    }

    std::set<SessionId> sessions;
    std::set<OrderId> orders;
    for (TestClient& client : clients) {
        sessions.insert(client.expect<protocol::HelloAck>().session_id);
        orders.insert(client.expect<protocol::OrderAccepted>().order_id);
    }

    EXPECT_EQ(sessions.size(), client_count);
    EXPECT_EQ(orders.size(), client_count);
}

// --- Saturation ---

TEST_P(ServerTest, BurstLargerThanTheQueuesIsFullyProcessedInOrder) {
    constexpr ClientOrderId order_count = 5000;
    TestClient client = session();
    std::vector<std::byte> bytes;
    for (ClientOrderId id = 1; id <= order_count; ++id) {
        protocol::encode(
            new_order(id, id % 2 == 0 ? Side::Buy : Side::Sell, id % 2 == 0 ? 100 : 200, 1), bytes);
    }

    // Written from another thread: the client must read the answers while it
    // writes, or both sides would end up waiting for each other.
    std::jthread writer([&client, &bytes] { client.send_bytes(bytes); });

    for (ClientOrderId id = 1; id <= order_count; ++id) {
        const auto accepted = client.expect<protocol::OrderAccepted>();
        ASSERT_EQ(accepted.client_order_id, id);
        ASSERT_EQ(accepted.order_id, id);
    }
    writer.join();
    expect_nothing_pending(client);
}

TEST_P(ServerTest, SnapshotLargerThanTheEventQueueIsDeliveredWhole) {
    constexpr Price level_count = 300;
    TestClient trader = session();
    for (Price i = 0; i < level_count; ++i) {
        trader.send(new_order(static_cast<ClientOrderId>(i + 1), Side::Sell, 1000 + i, 1));
        trader.expect<protocol::OrderAccepted>();
    }

    TestClient watcher = session();
    watcher.send(protocol::Subscribe{});

    EXPECT_EQ(watcher.expect<protocol::SnapshotBegin>().last_sequence,
              static_cast<std::uint64_t>(level_count));
    for (Price i = 0; i < level_count; ++i) {
        ASSERT_EQ(watcher.expect<protocol::SnapshotLevel>().price, 1000 + i);
    }
    watcher.expect<protocol::SnapshotEnd>();
}

TEST_P(ServerTest, ClientsSendingAtTheSameTimeAreAllServed) {
    constexpr std::size_t client_count = 8;
    constexpr ClientOrderId orders_per_client = 500;

    std::vector<std::jthread> threads;
    threads.reserve(client_count);
    std::array<ClientOrderId, client_count> accepted_counts{};
    for (std::size_t i = 0; i < client_count; ++i) {
        threads.emplace_back([this, i, &accepted_counts] {
            TestClient client = session();
            for (ClientOrderId id = 1; id <= orders_per_client; ++id) {
                // Prices that never cross, so every order rests.
                client.send(new_order(id, Side::Buy, 100, 1));
                if (client.expect<protocol::OrderAccepted>().client_order_id == id) {
                    ++accepted_counts.at(i);
                }
            }
        });
    }
    threads.clear(); // Joins every thread.

    for (const ClientOrderId count : accepted_counts) {
        EXPECT_EQ(count, orders_per_client);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Modes, ServerTest,
    ::testing::Values(Variant{.name = "Inline", .config = local_config(ServerMode::Inline)},
                      Variant{.name = "Pipeline", .config = local_config(ServerMode::Pipeline)},
                      Variant{.name = "PipelineTinyQueues",
                              .config = local_config(ServerMode::Pipeline, 2)}),
    [](const ::testing::TestParamInfo<Variant>& variant) { return variant.param.name; });

TEST(ServerLifecycle, RunReturnsAfterRequestStop) {
    Server server(local_config(ServerMode::Pipeline));
    std::jthread thread([&server] { server.run(); });

    server.request_stop();
    thread.join();

    SUCCEED();
}

TEST(ServerLifecycle, StopRequestedBeforeRunIsHonoured) {
    Server server(local_config(ServerMode::Pipeline));
    server.request_stop();

    server.run();

    SUCCEED();
}

TEST(ServerLifecycle, PortAlreadyInUseIsReported) {
    const Server first(local_config(ServerMode::Inline));

    EXPECT_THROW(Server(ServerConfig{.port = first.port()}), std::system_error);
}

TEST(ServerLifecycle, InvalidAddressIsReported) {
    EXPECT_THROW(Server(ServerConfig{.address = "not-an-address", .port = 0}), std::system_error);
}

} // namespace
} // namespace exchange::server
