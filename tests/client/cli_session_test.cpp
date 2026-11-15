#include "exchange/client/book_view.hpp"
#include "exchange/client/cli_session.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace exchange::client {
namespace {

using Lines = std::vector<std::string>;
using Levels = std::vector<LevelInfo>;
using Messages = std::vector<protocol::ClientMessage>;

// --- BookView ---

TEST(BookView, IsNotReadyBeforeASnapshot) {
    const BookView book;

    EXPECT_FALSE(book.ready());
    EXPECT_EQ(book.render(10), Lines{"the book is empty"});
}

TEST(BookView, SnapshotThenUpdatesGiveTheCurrentBook) {
    BookView book;
    book.begin_snapshot();
    book.set_level(Side::Buy, 100, 5);
    book.set_level(Side::Buy, 99, 2);
    book.set_level(Side::Sell, 101, 7);
    book.end_snapshot();
    EXPECT_TRUE(book.ready());

    book.set_level(Side::Buy, 100, 9);  // changed
    book.set_level(Side::Buy, 99, 0);   // removed
    book.set_level(Side::Sell, 103, 1); // added

    EXPECT_EQ(book.levels(Side::Buy), (Levels{{100, 9}}));
    EXPECT_EQ(book.levels(Side::Sell), (Levels{{101, 7}, {103, 1}}));
}

TEST(BookView, ANewSnapshotReplacesTheOldContent) {
    BookView book;
    book.begin_snapshot();
    book.set_level(Side::Buy, 100, 5);
    book.end_snapshot();

    book.begin_snapshot();
    EXPECT_FALSE(book.ready());
    book.set_level(Side::Sell, 101, 7);
    book.end_snapshot();

    EXPECT_TRUE(book.levels(Side::Buy).empty());
    EXPECT_EQ(book.levels(Side::Sell), (Levels{{101, 7}}));
}

TEST(BookView, RendersBestLevelsFirstAndCountsHiddenOnes) {
    BookView book;
    book.begin_snapshot();
    book.set_level(Side::Buy, 100, 5);
    book.set_level(Side::Buy, 99, 2);
    book.set_level(Side::Buy, 98, 1);
    book.set_level(Side::Sell, 101, 7);
    book.end_snapshot();

    EXPECT_EQ(book.render(2), (Lines{
                                  "                BIDS |       ASKS",
                                  "  quantity     price |      price  quantity",
                                  "         5       100 |        101         7",
                                  "         2        99 |",
                                  "(not shown: 1 bid levels, 0 ask levels)",
                              }));
}

// --- CliSession ---

// A session that has completed the handshake and received an empty snapshot.
CliSession ready_session() {
    CliSession session;
    static_cast<void>(session.on_message(protocol::HelloAck{.version = 1, .session_id = 3}));
    static_cast<void>(session.on_message(protocol::SnapshotBegin{.last_sequence = 0}));
    static_cast<void>(session.on_message(protocol::SnapshotEnd{}));
    return session;
}

TEST(CliSession, StartsWithHello) {
    EXPECT_EQ(CliSession::start().to_send,
              (Messages{protocol::Hello{.magic = protocol::magic, .version = protocol::version}}));
}

TEST(CliSession, SubscribesAfterTheHandshakeAndBecomesReadyAfterTheSnapshot) {
    CliSession session;
    EXPECT_FALSE(session.ready());

    const CliOutput output = session.on_message(protocol::HelloAck{.version = 1, .session_id = 3});
    EXPECT_EQ(output.lines, Lines{"session 3 opened"});
    EXPECT_EQ(output.to_send, Messages{protocol::Subscribe{}});
    EXPECT_FALSE(session.ready());

    static_cast<void>(session.on_message(protocol::SnapshotBegin{.last_sequence = 0}));
    static_cast<void>(session.on_message(protocol::SnapshotEnd{}));
    EXPECT_TRUE(session.ready());
}

TEST(CliSession, BuyAndSellSendOrdersWithIncreasingClientOrderIds) {
    CliSession session = ready_session();

    EXPECT_EQ(session.on_line("buy 100 10").to_send,
              (Messages{protocol::NewOrder{
                  .client_order_id = 1, .side = Side::Buy, .price = 100, .quantity = 10}}));
    EXPECT_EQ(session.on_line("  sell   101\t5 ").to_send,
              (Messages{protocol::NewOrder{
                  .client_order_id = 2, .side = Side::Sell, .price = 101, .quantity = 5}}));
}

TEST(CliSession, MalformedCommandsSendNothing) {
    CliSession session = ready_session();

    for (const std::string line :
         {"buy", "buy 100", "buy x 10", "buy 100 10 extra", "sell 100 -1", "cancel", "cancel x",
          "cancel 0", "book x", "book 1 2", "frobnicate"}) {
        const CliOutput output = session.on_line(line);
        EXPECT_TRUE(output.to_send.empty()) << line;
        EXPECT_EQ(output.lines.size(), 1U) << line;
        EXPECT_FALSE(output.quit) << line;
    }
    EXPECT_TRUE(session.on_line("").lines.empty());
    EXPECT_TRUE(session.on_line("   ").lines.empty());
}

TEST(CliSession, OutOfRangeValuesAreLeftToTheServer) {
    CliSession session = ready_session();

    EXPECT_EQ(session.on_line("buy 0 1").to_send.size(), 1U);

    EXPECT_EQ(session
                  .on_message(protocol::OrderRejected{.client_order_id = 1,
                                                      .reason = RejectReason::InvalidPrice})
                  .lines,
              Lines{"order rejected (invalid price): buy 1 @ 0"});
}

TEST(CliSession, ReportsTheLifeOfAnOrder) {
    CliSession session = ready_session();
    static_cast<void>(session.on_line("buy 100 10"));

    EXPECT_EQ(
        session.on_message(protocol::OrderAccepted{.client_order_id = 1, .order_id = 7}).lines,
        Lines{"order 7 accepted: buy 10 @ 100"});
    EXPECT_EQ(session.on_line("orders").lines, Lines{"order 7: buy 10 @ 100 remaining"});

    EXPECT_EQ(session
                  .on_message(protocol::Fill{
                      .order_id = 7, .price = 100, .quantity = 4, .remaining_quantity = 6})
                  .lines,
              Lines{"order 7 filled: 4 @ 100, 6 remaining"});
    EXPECT_EQ(session.on_line("orders").lines, Lines{"order 7: buy 6 @ 100 remaining"});

    EXPECT_EQ(session
                  .on_message(protocol::OrderCancelled{
                      .order_id = 7, .cancelled_quantity = 6, .reason = CancelReason::Requested})
                  .lines,
              Lines{"order 7 cancelled: 6 not filled"});
    EXPECT_EQ(session.on_line("orders").lines, Lines{"you have no active orders"});
}

TEST(CliSession, FullyFilledOrderIsNoLongerActive) {
    CliSession session = ready_session();
    static_cast<void>(session.on_line("sell 101 5"));
    static_cast<void>(
        session.on_message(protocol::OrderAccepted{.client_order_id = 1, .order_id = 2}));

    EXPECT_EQ(session
                  .on_message(protocol::Fill{
                      .order_id = 2, .price = 101, .quantity = 5, .remaining_quantity = 0})
                  .lines,
              Lines{"order 2 filled: 5 @ 101, complete"});
    EXPECT_EQ(session.on_line("orders").lines, Lines{"you have no active orders"});
}

TEST(CliSession, CancelSendsACancelAndReportsARejection) {
    CliSession session = ready_session();

    EXPECT_EQ(session.on_line("cancel 77").to_send,
              Messages{protocol::CancelOrder{.order_id = 77}});
    EXPECT_EQ(session
                  .on_message(protocol::CancelRejected{.order_id = 77,
                                                       .reason = CancelRejectReason::UnknownOrder})
                  .lines,
              Lines{"cancel rejected: order 77 is not one of your active orders"});
}

TEST(CliSession, SyncRequestIsAnsweredSilently) {
    CliSession session = ready_session();
    EXPECT_FALSE(session.sync_pending());

    EXPECT_EQ(session.begin_sync(), protocol::ClientMessage{protocol::CancelOrder{.order_id = 0}});
    EXPECT_TRUE(session.sync_pending());

    const CliOutput output = session.on_message(
        protocol::CancelRejected{.order_id = 0, .reason = CancelRejectReason::UnknownOrder});
    EXPECT_TRUE(output.lines.empty());
    EXPECT_FALSE(session.sync_pending());
}

TEST(CliSession, BookFollowsMarketData) {
    CliSession session;
    static_cast<void>(session.on_message(protocol::HelloAck{.version = 1, .session_id = 3}));
    static_cast<void>(session.on_message(protocol::SnapshotBegin{.last_sequence = 5}));
    static_cast<void>(session.on_message(
        protocol::SnapshotLevel{.side = Side::Buy, .price = 100, .total_quantity = 5}));
    static_cast<void>(session.on_message(protocol::SnapshotEnd{}));

    static_cast<void>(session.on_message(protocol::BookUpdate{
        .sequence = 6, .side = Side::Sell, .price = 101, .total_quantity = 7}));
    EXPECT_EQ(session
                  .on_message(protocol::Trade{
                      .sequence = 7, .price = 101, .quantity = 2, .aggressor_side = Side::Buy})
                  .lines,
              Lines{"market trade: 2 @ 101, buy side aggressor"});
    static_cast<void>(session.on_message(protocol::BookUpdate{
        .sequence = 8, .side = Side::Sell, .price = 101, .total_quantity = 5}));

    EXPECT_EQ(session.on_line("book").lines, (Lines{
                                                 "                BIDS |       ASKS",
                                                 "  quantity     price |      price  quantity",
                                                 "         5       100 |        101         5",
                                             }));
}

TEST(CliSession, SequenceGapTriggersANewSubscription) {
    CliSession session = ready_session();

    const CliOutput gap = session.on_message(
        protocol::BookUpdate{.sequence = 2, .side = Side::Buy, .price = 100, .total_quantity = 5});
    EXPECT_EQ(gap.to_send, Messages{protocol::Subscribe{}});
    EXPECT_EQ(gap.lines.size(), 1U);
    EXPECT_FALSE(session.ready());

    // Updates that arrive before the new snapshot are already included in it.
    const CliOutput stale = session.on_message(
        protocol::BookUpdate{.sequence = 3, .side = Side::Buy, .price = 100, .total_quantity = 9});
    EXPECT_TRUE(stale.to_send.empty());

    static_cast<void>(session.on_message(protocol::SnapshotBegin{.last_sequence = 3}));
    static_cast<void>(session.on_message(
        protocol::SnapshotLevel{.side = Side::Buy, .price = 100, .total_quantity = 9}));
    static_cast<void>(session.on_message(protocol::SnapshotEnd{}));
    EXPECT_TRUE(session.ready());

    const CliOutput next = session.on_message(
        protocol::BookUpdate{.sequence = 4, .side = Side::Buy, .price = 100, .total_quantity = 1});
    EXPECT_TRUE(next.to_send.empty());
    EXPECT_TRUE(next.lines.empty());
}

TEST(CliSession, QuitAndHelp) {
    CliSession session = ready_session();

    EXPECT_TRUE(session.on_line("quit").quit);
    EXPECT_TRUE(session.on_line("exit").quit);
    EXPECT_GT(session.on_line("help").lines.size(), 5U);
}

TEST(CliSession, ReportsAProtocolError) {
    CliSession session = ready_session();

    EXPECT_EQ(
        session.on_message(protocol::ProtocolError{.code = protocol::ProtocolErrorCode::BadField})
            .lines,
        Lines{"protocol error reported by the server: bad field"});
}

} // namespace
} // namespace exchange::client
