#include "exchange/server/server.hpp"

#include "support/test_client.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
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

// A server configuration that every test runs against; variants() lists them.
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

// The same configuration with a network thread that never sleeps.
ServerConfig spinning(ServerConfig config) {
    config.network_wait = NetworkWait::Spin;
    return config;
}

// "PipelineTinyQueues" has queues so small that they are full almost all the
// time, which exercises every backpressure path with the ordinary tests.
std::vector<Variant> variants() {
    return {
        Variant{.name = "Inline", .config = local_config(ServerMode::Inline)},
        Variant{.name = "Pipeline", .config = local_config(ServerMode::Pipeline)},
        Variant{.name = "PipelineTinyQueues", .config = local_config(ServerMode::Pipeline, 2)},
        Variant{.name = "InlineSpin", .config = spinning(local_config(ServerMode::Inline))},
        Variant{.name = "PipelineSpin", .config = spinning(local_config(ServerMode::Pipeline))},
    };
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

// --- Limits and shutdown: tests that need their own server settings ---

class ServerLimitsTest : public ::testing::TestWithParam<Variant> {
public:
    ServerLimitsTest(const ServerLimitsTest&) = delete;
    ServerLimitsTest& operator=(const ServerLimitsTest&) = delete;

protected:
    ServerLimitsTest() = default;
    ~ServerLimitsTest() override { stop(); }

    void start(const ServerConfig& config) {
        server_ = std::make_unique<Server>(config);
        thread_ = std::jthread([this] { server_->run(); });
    }

    // Stops the server and waits until it has finished shutting down.
    void stop() {
        if (thread_.joinable()) {
            server_->request_stop();
            thread_.join();
        }
    }

    [[nodiscard]] TestClient session() const {
        TestClient client(server_->port());
        client.hello();
        return client;
    }

    // A session that subscribes to market data and then never reads again.
    [[nodiscard]] TestClient idle_subscriber() const {
        TestClient client = session();
        client.set_receive_buffer(4096);
        client.send(new_order(1, Side::Buy, 50, 1));
        client.expect<protocol::OrderAccepted>();
        client.send(protocol::Subscribe{});
        client.expect<protocol::SnapshotBegin>();
        client.expect<protocol::SnapshotLevel>();
        client.expect<protocol::SnapshotEnd>();
        return client;
    }

    // Sends a batch of resting sell orders and reads their acknowledgements.
    // Each order produces one market data update for every subscriber.
    // Returns true if a market data update for the bid level at `watched_price`
    // going to zero was seen among the answers.
    static bool send_batch(TestClient& client, ClientOrderId& next_client_order_id,
                           Price watched_price) {
        constexpr int batch_size = 200;
        std::vector<std::byte> bytes;
        for (int i = 0; i < batch_size; ++i) {
            protocol::encode(new_order(next_client_order_id++, Side::Sell, 200, 1), bytes);
        }
        client.send_bytes(bytes);

        bool level_removed = false;
        for (int acknowledged = 0; acknowledged < batch_size;) {
            const std::optional<protocol::ServerMessage> message = client.receive();
            if (!message.has_value()) {
                ADD_FAILURE() << "the active client was disconnected";
                return false;
            }
            if (std::holds_alternative<protocol::OrderAccepted>(*message)) {
                ++acknowledged;
            } else if (const auto* update = std::get_if<protocol::BookUpdate>(&*message)) {
                level_removed = level_removed ||
                                (update->price == watched_price && update->total_quantity == 0);
            }
        }
        return level_removed;
    }

    std::unique_ptr<Server> server_;
    std::jthread thread_;
};

TEST_P(ServerLimitsTest, LargeSnapshotIsNotMistakenForASlowClient) {
    ServerConfig config = GetParam().config;
    config.max_output_bytes = 4096;
    start(config);

    // 1000 levels make a snapshot of about 20 kB, five times the limit.
    constexpr Price level_count = 1000;
    TestClient trader = session();
    for (Price i = 0; i < level_count; ++i) {
        trader.send(new_order(static_cast<ClientOrderId>(i + 1), Side::Sell, 1000 + i, 1));
        trader.expect<protocol::OrderAccepted>();
    }

    TestClient watcher = session();
    watcher.send(protocol::Subscribe{});

    watcher.expect<protocol::SnapshotBegin>();
    for (Price i = 0; i < level_count; ++i) {
        ASSERT_EQ(watcher.expect<protocol::SnapshotLevel>().price, 1000 + i);
    }
    watcher.expect<protocol::SnapshotEnd>();
}

TEST_P(ServerLimitsTest, ClientThatDoesNotReadIsDisconnectedAndItsOrdersCancelled) {
    ServerConfig config = GetParam().config;
    config.max_output_bytes = std::size_t{16} * 1024;
    config.socket_send_buffer_bytes = 4096;
    start(config);

    TestClient slow = idle_subscriber(); // Has a buy order resting at 50.
    TestClient active = session();
    active.send(protocol::Subscribe{});
    active.expect<protocol::SnapshotBegin>();
    active.expect<protocol::SnapshotLevel>();
    active.expect<protocol::SnapshotEnd>();

    // Market data piles up for the slow client until the server gives up on
    // it. The active client, which reads, sees the slow client's order go.
    bool slow_order_cancelled = false;
    ClientOrderId next_client_order_id = 1;
    int batches = 0;
    while (!slow_order_cancelled && batches < 2000) {
        slow_order_cancelled = send_batch(active, next_client_order_id, 50);
        ++batches;
    }

    EXPECT_TRUE(slow_order_cancelled) << "after " << batches << " batches";
    EXPECT_TRUE(slow.read_until_closed());

    // The active client was never affected.
    active.send(protocol::CancelOrder{.order_id = 0});
    bool answered = false;
    while (!answered) {
        const std::optional<protocol::ServerMessage> message = active.receive();
        if (!message.has_value()) {
            FAIL() << "the active client was disconnected";
        }
        answered = std::holds_alternative<protocol::CancelRejected>(*message);
    }
}

TEST_P(ServerLimitsTest, StoppingDoesNotLoseAnswersInTheMiddleOfAStream) {
    start(GetParam().config);
    constexpr ClientOrderId order_count = 3000;
    TestClient client = session();
    std::vector<std::byte> bytes;
    for (ClientOrderId id = 1; id <= order_count; ++id) {
        protocol::encode(new_order(id, Side::Buy, 100, 1), bytes);
    }
    // The server may close the connection while this thread is still writing.
    std::jthread writer([&client, &bytes] {
        try {
            client.send_bytes(bytes);
        } catch (const std::runtime_error&) { // NOLINT(bugprone-empty-catch)
        }
    });

    // Wait until the server is in the middle of the stream, then stop it.
    EXPECT_EQ(client.expect<protocol::OrderAccepted>().client_order_id, 1U);
    server_->request_stop();

    // Whatever the server answered must be the beginning of the stream, with
    // no answer missing before the last one received.
    ClientOrderId last = 1;
    while (true) {
        const std::optional<protocol::ServerMessage> message = client.receive();
        if (!message.has_value()) {
            break;
        }
        const auto* accepted = std::get_if<protocol::OrderAccepted>(&*message);
        ASSERT_NE(accepted, nullptr);
        ASSERT_EQ(accepted->client_order_id, last + 1);
        ASSERT_EQ(accepted->order_id, last + 1);
        last = accepted->client_order_id;
    }
    writer.join();
    EXPECT_GE(last, 1U);
}

TEST_P(ServerLimitsTest, StoppingWaitsForSlowClientsOnlyDuringTheGracePeriod) {
    ServerConfig config = GetParam().config;
    config.max_output_bytes = std::size_t{64} * 1024 * 1024; // Never reached here.
    config.socket_send_buffer_bytes = 4096;
    config.shutdown_grace = std::chrono::milliseconds(300);
    start(config);

    // Kept open, and never read from, until the end of the test.
    const TestClient slow = idle_subscriber();
    TestClient active = session();
    ClientOrderId next_client_order_id = 1;
    for (int batch = 0; batch < 50; ++batch) {
        send_batch(active, next_client_order_id, 50);
    }

    // About 280 kB are waiting for a client that does not read.
    const auto before = std::chrono::steady_clock::now();
    stop();
    const auto elapsed = std::chrono::steady_clock::now() - before;

    EXPECT_GE(elapsed, std::chrono::milliseconds(200)) << "the grace period was not used";
    EXPECT_LT(elapsed, std::chrono::seconds(10)) << "the server waited for the slow client";
}

TEST_P(ServerLimitsTest, StoppingSendsWhatClientsHaveNotReadYet) {
    start(GetParam().config);
    TestClient client = session();
    constexpr ClientOrderId order_count = 100;
    for (ClientOrderId id = 1; id <= order_count; ++id) {
        client.send(new_order(id, Side::Buy, 100, 1));
    }
    // Reading the first answer shows that the server has started on them.
    client.expect<protocol::OrderAccepted>();

    stop();

    // After the server is gone, the answers it produced are still delivered.
    ClientOrderId received = 1;
    while (client.receive().has_value()) {
        ++received;
    }
    EXPECT_GE(received, 1U);
    EXPECT_LE(received, order_count);
}

INSTANTIATE_TEST_SUITE_P(Modes, ServerLimitsTest, ::testing::ValuesIn(variants()),
                         [](const ::testing::TestParamInfo<Variant>& variant) {
                             return variant.param.name;
                         });

INSTANTIATE_TEST_SUITE_P(Modes, ServerTest, ::testing::ValuesIn(variants()),
                         [](const ::testing::TestParamInfo<Variant>& variant) {
                             return variant.param.name;
                         });

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
