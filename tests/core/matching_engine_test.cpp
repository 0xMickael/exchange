#include "exchange/core/matching_engine.hpp"

#include "support/events.hpp"

#include <gtest/gtest.h>

#include <map>
#include <vector>

namespace exchange {
namespace {

using test::accepted;
using test::book;
using test::cancel_rejected;
using test::cancelled;
using test::fill;
using test::rejected;
using test::snapshot_begin;
using test::snapshot_end;
using test::snapshot_level;
using test::text;
using test::trade;

using Levels = std::vector<LevelInfo>;

constexpr SessionId a = 1;
constexpr SessionId b = 2;
constexpr SessionId c = 3;

class MatchingEngineTest : public ::testing::Test {
protected:
    // Processes a command and checks the invariants of SPEC section 5.6.
    std::vector<Event> run(const Command& command) {
        std::vector<Event> events;
        engine_.process(command, events);
        EXPECT_EQ(engine_.check_invariants(), "");
        return events;
    }

    std::vector<Event> submit(SessionId session, ClientOrderId client_order_id, Side side,
                              Price price, Quantity quantity) {
        return run(NewOrder{.session_id = session,
                            .client_order_id = client_order_id,
                            .side = side,
                            .price = price,
                            .quantity = quantity});
    }

    // Submits a valid order using the session's next client order id. Used to
    // set up the book: the n-th accepted order of a test has OrderId n.
    void rest(SessionId session, Side side, Price price, Quantity quantity) {
        submit(session, ++last_client_order_id_[session], side, price, quantity);
    }

    std::vector<Event> cancel(SessionId session, OrderId order) {
        return run(CancelOrder{.session_id = session, .order_id = order});
    }

    std::vector<Event> close(SessionId session) {
        return run(SessionClosed{.session_id = session});
    }

    [[nodiscard]] Levels bids() const { return engine_.book().levels(Side::Buy); }
    [[nodiscard]] Levels asks() const { return engine_.book().levels(Side::Sell); }

    MatchingEngine engine_;
    std::map<SessionId, ClientOrderId> last_client_order_id_;
};

// --- Resting and crossing (M1, M7) ---

TEST_F(MatchingEngineTest, OrderRestsWhenTheBookIsEmpty) {
    EXPECT_EQ(text(submit(a, 1, Side::Buy, 100, 10)),
              text({accepted(a, 1, 1), book(Side::Buy, 100, 10)}));
    EXPECT_EQ(bids(), (Levels{{100, 10}}));
}

TEST_F(MatchingEngineTest, OrdersRestWhenPricesDoNotCross) {
    rest(a, Side::Sell, 101, 10);

    EXPECT_EQ(text(submit(b, 1, Side::Buy, 100, 10)),
              text({accepted(b, 1, 2), book(Side::Buy, 100, 10)}));
    EXPECT_EQ(bids(), (Levels{{100, 10}}));
    EXPECT_EQ(asks(), (Levels{{101, 10}}));
}

TEST_F(MatchingEngineTest, BuyMatchesAnAskAtTheSamePrice) {
    rest(a, Side::Sell, 101, 10);

    EXPECT_EQ(text(submit(b, 1, Side::Buy, 101, 10)),
              text({accepted(b, 1, 2), fill(a, 1, 101, 10, 0), fill(b, 2, 101, 10, 0),
                    trade(101, 10, Side::Buy), book(Side::Sell, 101, 0)}));
    EXPECT_TRUE(asks().empty());
    EXPECT_TRUE(bids().empty());
}

TEST_F(MatchingEngineTest, SellMatchesABidAtTheSamePrice) {
    rest(a, Side::Buy, 100, 10);

    EXPECT_EQ(text(submit(b, 1, Side::Sell, 100, 10)),
              text({accepted(b, 1, 2), fill(a, 1, 100, 10, 0), fill(b, 2, 100, 10, 0),
                    trade(100, 10, Side::Sell), book(Side::Buy, 100, 0)}));
}

// --- Trade price (M4) ---

TEST_F(MatchingEngineTest, BuyAboveTheAskTradesAtTheAskPrice) {
    rest(a, Side::Sell, 101, 10);

    EXPECT_EQ(text(submit(b, 1, Side::Buy, 105, 10)),
              text({accepted(b, 1, 2), fill(a, 1, 101, 10, 0), fill(b, 2, 101, 10, 0),
                    trade(101, 10, Side::Buy), book(Side::Sell, 101, 0)}));
}

TEST_F(MatchingEngineTest, SellBelowTheBidTradesAtTheBidPrice) {
    rest(a, Side::Buy, 100, 10);

    EXPECT_EQ(text(submit(b, 1, Side::Sell, 95, 10)),
              text({accepted(b, 1, 2), fill(a, 1, 100, 10, 0), fill(b, 2, 100, 10, 0),
                    trade(100, 10, Side::Sell), book(Side::Buy, 100, 0)}));
}

// --- Priority (M2, M3) ---

TEST_F(MatchingEngineTest, BuyTakesTheLowestAskFirst) {
    rest(a, Side::Sell, 102, 5); // order 1
    rest(a, Side::Sell, 101, 5); // order 2

    EXPECT_EQ(text(submit(b, 1, Side::Buy, 102, 5)),
              text({accepted(b, 1, 3), fill(a, 2, 101, 5, 0), fill(b, 3, 101, 5, 0),
                    trade(101, 5, Side::Buy), book(Side::Sell, 101, 0)}));
    EXPECT_EQ(asks(), (Levels{{102, 5}}));
}

TEST_F(MatchingEngineTest, SellTakesTheHighestBidFirst) {
    rest(a, Side::Buy, 99, 5);  // order 1
    rest(a, Side::Buy, 100, 5); // order 2

    EXPECT_EQ(text(submit(b, 1, Side::Sell, 99, 5)),
              text({accepted(b, 1, 3), fill(a, 2, 100, 5, 0), fill(b, 3, 100, 5, 0),
                    trade(100, 5, Side::Sell), book(Side::Buy, 100, 0)}));
    EXPECT_EQ(bids(), (Levels{{99, 5}}));
}

TEST_F(MatchingEngineTest, OlderOrderTradesFirstAtTheSamePrice) {
    rest(a, Side::Sell, 101, 5); // order 1
    rest(b, Side::Sell, 101, 5); // order 2

    EXPECT_EQ(text(submit(c, 1, Side::Buy, 101, 5)),
              text({accepted(c, 1, 3), fill(a, 1, 101, 5, 0), fill(c, 3, 101, 5, 0),
                    trade(101, 5, Side::Buy), book(Side::Sell, 101, 5)}));
}

TEST_F(MatchingEngineTest, PartiallyFilledOrderKeepsItsPriority) {
    rest(a, Side::Sell, 101, 10); // order 1
    rest(b, Side::Sell, 101, 10); // order 2
    rest(c, Side::Buy, 101, 4);   // order 3, fills 4 of order 1

    EXPECT_EQ(text(submit(c, 2, Side::Buy, 101, 8)),
              text({accepted(c, 2, 4), fill(a, 1, 101, 6, 0), fill(c, 4, 101, 6, 2),
                    trade(101, 6, Side::Buy), fill(b, 2, 101, 2, 8), fill(c, 4, 101, 2, 0),
                    trade(101, 2, Side::Buy), book(Side::Sell, 101, 8)}));
}

// --- Partial fills and multiple levels (M5, M6, M7) ---

TEST_F(MatchingEngineTest, IncomingOrderSmallerThanTheRestingOrder) {
    rest(a, Side::Sell, 101, 10);

    EXPECT_EQ(text(submit(b, 1, Side::Buy, 101, 3)),
              text({accepted(b, 1, 2), fill(a, 1, 101, 3, 7), fill(b, 2, 101, 3, 0),
                    trade(101, 3, Side::Buy), book(Side::Sell, 101, 7)}));
    EXPECT_TRUE(bids().empty());
}

TEST_F(MatchingEngineTest, MatchingStopsAtTheLimitPrice) {
    rest(a, Side::Sell, 101, 5);
    rest(a, Side::Sell, 103, 5);

    EXPECT_EQ(text(submit(b, 1, Side::Buy, 102, 8)),
              text({accepted(b, 1, 3), fill(a, 1, 101, 5, 0), fill(b, 3, 101, 5, 3),
                    trade(101, 5, Side::Buy), book(Side::Sell, 101, 0), book(Side::Buy, 102, 3)}));
    EXPECT_EQ(asks(), (Levels{{103, 5}}));
    EXPECT_EQ(bids(), (Levels{{102, 3}}));
}

TEST_F(MatchingEngineTest, SpecExampleA) {
    rest(a, Side::Sell, 101, 5);  // order 1
    rest(b, Side::Sell, 101, 3);  // order 2
    rest(a, Side::Sell, 102, 10); // order 3

    EXPECT_EQ(text(submit(c, 1, Side::Buy, 102, 12)),
              text({accepted(c, 1, 4), fill(a, 1, 101, 5, 0), fill(c, 4, 101, 5, 7),
                    trade(101, 5, Side::Buy), fill(b, 2, 101, 3, 0), fill(c, 4, 101, 3, 4),
                    trade(101, 3, Side::Buy), book(Side::Sell, 101, 0), fill(a, 3, 102, 4, 6),
                    fill(c, 4, 102, 4, 0), trade(102, 4, Side::Buy), book(Side::Sell, 102, 6)}));
    EXPECT_EQ(asks(), (Levels{{102, 6}}));
    EXPECT_TRUE(bids().empty());
}

TEST_F(MatchingEngineTest, SpecExampleB) {
    rest(a, Side::Sell, 101, 5); // order 1

    EXPECT_EQ(text(submit(c, 1, Side::Buy, 101, 8)),
              text({accepted(c, 1, 2), fill(a, 1, 101, 5, 0), fill(c, 2, 101, 5, 3),
                    trade(101, 5, Side::Buy), book(Side::Sell, 101, 0), book(Side::Buy, 101, 3)}));
    EXPECT_EQ(text(cancel(c, 2)), text({cancelled(c, 2, 3), book(Side::Buy, 101, 0)}));
    EXPECT_EQ(text(cancel(c, 2)), text({cancel_rejected(c, 2)}));
}

TEST_F(MatchingEngineTest, SellSweepsSeveralBidLevels) {
    rest(a, Side::Buy, 100, 2); // order 1
    rest(a, Side::Buy, 99, 2);  // order 2
    rest(a, Side::Buy, 98, 2);  // order 3

    EXPECT_EQ(text(submit(b, 1, Side::Sell, 99, 10)),
              text({accepted(b, 1, 4), fill(a, 1, 100, 2, 0), fill(b, 4, 100, 2, 8),
                    trade(100, 2, Side::Sell), book(Side::Buy, 100, 0), fill(a, 2, 99, 2, 0),
                    fill(b, 4, 99, 2, 6), trade(99, 2, Side::Sell), book(Side::Buy, 99, 0),
                    book(Side::Sell, 99, 6)}));
    EXPECT_EQ(bids(), (Levels{{98, 2}}));
    EXPECT_EQ(asks(), (Levels{{99, 6}}));
}

TEST_F(MatchingEngineTest, SessionCanTradeWithItself) {
    rest(a, Side::Sell, 101, 5);

    EXPECT_EQ(text(submit(a, 2, Side::Buy, 101, 5)),
              text({accepted(a, 2, 2), fill(a, 1, 101, 5, 0), fill(a, 2, 101, 5, 0),
                    trade(101, 5, Side::Buy), book(Side::Sell, 101, 0)}));
}

// --- Validation (section 4.1) ---

TEST_F(MatchingEngineTest, ClientOrderIdMustIncrease) {
    EXPECT_EQ(text(submit(a, 0, Side::Buy, 100, 1)),
              text({rejected(a, 0, RejectReason::InvalidClientOrderId)}));

    rest(a, Side::Buy, 100, 1); // client order id 1
    submit(a, 5, Side::Buy, 100, 1);

    EXPECT_EQ(text(submit(a, 5, Side::Buy, 100, 1)),
              text({rejected(a, 5, RejectReason::InvalidClientOrderId)}));
    EXPECT_EQ(text(submit(a, 3, Side::Buy, 100, 1)),
              text({rejected(a, 3, RejectReason::InvalidClientOrderId)}));
    EXPECT_EQ(text(submit(a, 6, Side::Buy, 100, 1)),
              text({accepted(a, 6, 3), book(Side::Buy, 100, 3)}));
}

TEST_F(MatchingEngineTest, ClientOrderIdsAreIndependentBetweenSessions) {
    submit(a, 10, Side::Buy, 100, 1);

    EXPECT_EQ(text(submit(b, 1, Side::Buy, 100, 1)),
              text({accepted(b, 1, 2), book(Side::Buy, 100, 2)}));
}

TEST_F(MatchingEngineTest, PriceMustBeInRange) {
    EXPECT_EQ(text(submit(a, 1, Side::Buy, 0, 1)),
              text({rejected(a, 1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(text(submit(a, 2, Side::Buy, -5, 1)),
              text({rejected(a, 2, RejectReason::InvalidPrice)}));
    EXPECT_EQ(text(submit(a, 3, Side::Buy, max_price + 1, 1)),
              text({rejected(a, 3, RejectReason::InvalidPrice)}));
    EXPECT_TRUE(bids().empty());

    EXPECT_EQ(text(submit(a, 4, Side::Buy, min_price, 1)),
              text({accepted(a, 4, 1), book(Side::Buy, min_price, 1)}));
    EXPECT_EQ(text(submit(a, 5, Side::Sell, max_price, 1)),
              text({accepted(a, 5, 2), book(Side::Sell, max_price, 1)}));
}

TEST_F(MatchingEngineTest, QuantityMustBeInRange) {
    EXPECT_EQ(text(submit(a, 1, Side::Buy, 100, 0)),
              text({rejected(a, 1, RejectReason::InvalidQuantity)}));
    EXPECT_EQ(text(submit(a, 2, Side::Buy, 100, max_quantity + 1)),
              text({rejected(a, 2, RejectReason::InvalidQuantity)}));
    EXPECT_TRUE(bids().empty());

    EXPECT_EQ(text(submit(a, 3, Side::Buy, 100, max_quantity)),
              text({accepted(a, 3, 1), book(Side::Buy, 100, max_quantity)}));
}

TEST_F(MatchingEngineTest, FirstFailingCheckDecidesTheRejectReason) {
    submit(a, 1, Side::Buy, 100, 1);

    // Bad client order id and bad price: the client order id is checked first.
    EXPECT_EQ(text(submit(a, 1, Side::Buy, 0, 0)),
              text({rejected(a, 1, RejectReason::InvalidClientOrderId)}));
    // Bad price and bad quantity: the price is checked first.
    EXPECT_EQ(text(submit(a, 2, Side::Buy, 0, 0)),
              text({rejected(a, 2, RejectReason::InvalidPrice)}));
}

TEST_F(MatchingEngineTest, ClientOrderIdIsConsumedByALaterRejection) {
    EXPECT_EQ(text(submit(a, 1, Side::Buy, 0, 1)),
              text({rejected(a, 1, RejectReason::InvalidPrice)}));

    EXPECT_EQ(text(submit(a, 1, Side::Buy, 100, 1)),
              text({rejected(a, 1, RejectReason::InvalidClientOrderId)}));
}

TEST_F(MatchingEngineTest, RejectedOrdersDoNotConsumeOrderIds) {
    submit(a, 1, Side::Buy, 100, 1); // order 1
    submit(a, 2, Side::Buy, 0, 1);   // rejected

    EXPECT_EQ(text(submit(a, 3, Side::Buy, 100, 1)),
              text({accepted(a, 3, 2), book(Side::Buy, 100, 2)}));
}

// --- Cancel (section 4.2) ---

TEST_F(MatchingEngineTest, CancelRemovesARestingOrder) {
    rest(a, Side::Buy, 100, 10); // order 1
    rest(b, Side::Buy, 100, 5);  // order 2

    EXPECT_EQ(text(cancel(a, 1)), text({cancelled(a, 1, 10), book(Side::Buy, 100, 5)}));
    EXPECT_EQ(bids(), (Levels{{100, 5}}));
}

TEST_F(MatchingEngineTest, CancelAfterAPartialFillCancelsTheRemainder) {
    rest(a, Side::Sell, 101, 10); // order 1
    rest(b, Side::Buy, 101, 4);   // order 2, fully filled

    EXPECT_EQ(text(cancel(a, 1)), text({cancelled(a, 1, 6), book(Side::Sell, 101, 0)}));
    EXPECT_TRUE(asks().empty());
}

TEST_F(MatchingEngineTest, CancelAfterAFullFillIsRejected) {
    rest(a, Side::Sell, 101, 10); // order 1
    rest(b, Side::Buy, 101, 10);  // order 2

    EXPECT_EQ(text(cancel(a, 1)), text({cancel_rejected(a, 1)}));
    EXPECT_EQ(text(cancel(b, 2)), text({cancel_rejected(b, 2)}));
}

TEST_F(MatchingEngineTest, CancelOfAnotherSessionsOrderIsRejected) {
    rest(a, Side::Buy, 100, 10);

    EXPECT_EQ(text(cancel(b, 1)), text({cancel_rejected(b, 1)}));
    EXPECT_EQ(bids(), (Levels{{100, 10}}));
}

TEST_F(MatchingEngineTest, CancelOfAnUnassignedOrderIdIsRejected) {
    EXPECT_EQ(text(cancel(a, 1)), text({cancel_rejected(a, 1)}));
    EXPECT_EQ(text(cancel(a, 0)), text({cancel_rejected(a, 0)}));
}

TEST_F(MatchingEngineTest, CancelledOrderNoLongerTrades) {
    rest(a, Side::Sell, 101, 5); // order 1
    rest(b, Side::Sell, 101, 5); // order 2
    cancel(a, 1);

    EXPECT_EQ(text(submit(c, 1, Side::Buy, 101, 5)),
              text({accepted(c, 1, 3), fill(b, 2, 101, 5, 0), fill(c, 3, 101, 5, 0),
                    trade(101, 5, Side::Buy), book(Side::Sell, 101, 0)}));
}

// --- Session closed (section 4.3) ---

TEST_F(MatchingEngineTest, ClosingASessionCancelsItsOrdersInOrderIdOrder) {
    rest(a, Side::Sell, 103, 1); // order 1
    rest(b, Side::Buy, 100, 2);  // order 2
    rest(a, Side::Buy, 100, 3);  // order 3
    rest(a, Side::Sell, 102, 4); // order 4

    EXPECT_EQ(text(close(a)),
              text({cancelled(a, 1, 1, CancelReason::SessionClosed), book(Side::Sell, 103, 0),
                    cancelled(a, 3, 3, CancelReason::SessionClosed), book(Side::Buy, 100, 2),
                    cancelled(a, 4, 4, CancelReason::SessionClosed), book(Side::Sell, 102, 0)}));
    EXPECT_EQ(bids(), (Levels{{100, 2}}));
    EXPECT_TRUE(asks().empty());
}

TEST_F(MatchingEngineTest, ClosingASessionSkipsOrdersThatAreNoLongerActive) {
    rest(a, Side::Sell, 101, 5); // order 1, will be filled
    rest(a, Side::Sell, 102, 5); // order 2, will be cancelled
    rest(a, Side::Sell, 103, 5); // order 3
    rest(b, Side::Buy, 101, 5);  // order 4
    cancel(a, 2);

    EXPECT_EQ(text(close(a)),
              text({cancelled(a, 3, 5, CancelReason::SessionClosed), book(Side::Sell, 103, 0)}));
}

TEST_F(MatchingEngineTest, ClosingASessionWithoutActiveOrdersEmitsNothing) {
    EXPECT_TRUE(close(a).empty());

    rest(a, Side::Buy, 100, 1);
    cancel(a, 1);
    EXPECT_TRUE(close(a).empty());
}

TEST_F(MatchingEngineTest, ClosingASessionDiscardsItsState) {
    submit(a, 7, Side::Buy, 100, 1);
    close(a);

    // The client order id sequence starts again from scratch.
    EXPECT_EQ(text(submit(a, 1, Side::Buy, 100, 1)),
              text({accepted(a, 1, 2), book(Side::Buy, 100, 1)}));
}

// --- Snapshot (sections 5.3 and 6.4) ---

TEST_F(MatchingEngineTest, SnapshotOfAnEmptyBook) {
    EXPECT_EQ(text(run(SnapshotRequest{.session_id = a})),
              text({snapshot_begin(a), snapshot_end(a)}));
}

TEST_F(MatchingEngineTest, SnapshotListsBidsThenAsksBestPriceFirst) {
    rest(a, Side::Buy, 99, 5);
    rest(b, Side::Buy, 100, 2);
    rest(a, Side::Buy, 100, 3);
    rest(a, Side::Sell, 103, 7);
    rest(b, Side::Sell, 102, 1);

    EXPECT_EQ(text(run(SnapshotRequest{.session_id = c})),
              text({snapshot_begin(c), snapshot_level(c, Side::Buy, 100, 5),
                    snapshot_level(c, Side::Buy, 99, 5), snapshot_level(c, Side::Sell, 102, 1),
                    snapshot_level(c, Side::Sell, 103, 7), snapshot_end(c)}));
}

TEST_F(MatchingEngineTest, SnapshotChangesNothing) {
    rest(a, Side::Buy, 100, 5);
    run(SnapshotRequest{.session_id = b});

    EXPECT_EQ(bids(), (Levels{{100, 5}}));
    // The requesting session has no state: its first client order id is still free.
    EXPECT_EQ(text(submit(b, 1, Side::Buy, 100, 1)),
              text({accepted(b, 1, 2), book(Side::Buy, 100, 6)}));
}

// --- Determinism (section 5.5) ---

TEST(MatchingEngineDeterminism, SameCommandsProduceTheSameEvents) {
    const std::vector<Command> commands = {
        NewOrder{
            .session_id = a, .client_order_id = 1, .side = Side::Sell, .price = 101, .quantity = 5},
        NewOrder{
            .session_id = b, .client_order_id = 1, .side = Side::Sell, .price = 102, .quantity = 5},
        NewOrder{
            .session_id = c, .client_order_id = 1, .side = Side::Buy, .price = 102, .quantity = 7},
        CancelOrder{.session_id = b, .order_id = 2},
        NewOrder{
            .session_id = c, .client_order_id = 2, .side = Side::Buy, .price = 100, .quantity = 1},
        SessionClosed{.session_id = c},
    };

    const auto replay = [&commands] {
        MatchingEngine engine;
        std::vector<Event> events;
        for (const Command& command : commands) {
            engine.process(command, events);
        }
        return events;
    };

    const std::vector<Event> first = replay();
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, replay());
}

} // namespace
} // namespace exchange
