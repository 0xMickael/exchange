#include "exchange/core/order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace exchange {
namespace {

Order make_order(OrderId id, Side side, Price price, Quantity quantity, SessionId session = 1) {
    return Order{.id = id,
                 .session_id = session,
                 .side = side,
                 .price = price,
                 .quantity = quantity,
                 .remaining = quantity};
}

using Levels = std::vector<LevelInfo>;

TEST(OrderBook, StartsEmpty) {
    const OrderBook book;

    EXPECT_EQ(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.best(Side::Sell), nullptr);
    EXPECT_EQ(book.find(1), nullptr);
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_TRUE(book.levels(Side::Buy).empty());
    EXPECT_EQ(book.check_invariants(), "");
}

TEST(OrderBook, BestBidIsTheHighestPrice) {
    OrderBook book;
    book.add(make_order(1, Side::Buy, 99, 10));
    book.add(make_order(2, Side::Buy, 100, 10));
    book.add(make_order(3, Side::Buy, 98, 10));

    ASSERT_NE(book.best(Side::Buy), nullptr);
    EXPECT_EQ(book.best(Side::Buy)->id, 2U);
    EXPECT_EQ(book.check_invariants(), "");
}

TEST(OrderBook, BestAskIsTheLowestPrice) {
    OrderBook book;
    book.add(make_order(1, Side::Sell, 102, 10));
    book.add(make_order(2, Side::Sell, 101, 10));
    book.add(make_order(3, Side::Sell, 103, 10));

    ASSERT_NE(book.best(Side::Sell), nullptr);
    EXPECT_EQ(book.best(Side::Sell)->id, 2U);
    EXPECT_EQ(book.check_invariants(), "");
}

TEST(OrderBook, BestIsTheOldestOrderOfTheBestLevel) {
    OrderBook book;
    book.add(make_order(1, Side::Sell, 101, 10));
    book.add(make_order(2, Side::Sell, 101, 10));

    EXPECT_EQ(book.best(Side::Sell)->id, 1U);
}

TEST(OrderBook, LevelsAreListedBestFirstWithTheirTotals) {
    OrderBook book;
    book.add(make_order(1, Side::Buy, 99, 10));
    book.add(make_order(2, Side::Buy, 100, 5));
    book.add(make_order(3, Side::Buy, 100, 7));
    book.add(make_order(4, Side::Sell, 102, 1));
    book.add(make_order(5, Side::Sell, 101, 2));

    EXPECT_EQ(book.levels(Side::Buy), (Levels{{100, 12}, {99, 10}}));
    EXPECT_EQ(book.levels(Side::Sell), (Levels{{101, 2}, {102, 1}}));
    EXPECT_EQ(book.level_quantity(Side::Buy, 100), 12U);
    EXPECT_EQ(book.level_quantity(Side::Buy, 101), 0U);
    EXPECT_EQ(book.order_count(), 5U);
}

TEST(OrderBook, PartialFillKeepsTheOrderAtTheFront) {
    OrderBook book;
    book.add(make_order(1, Side::Sell, 101, 10));
    book.add(make_order(2, Side::Sell, 101, 10));

    book.fill_best(Side::Sell, 4);

    ASSERT_NE(book.best(Side::Sell), nullptr);
    EXPECT_EQ(book.best(Side::Sell)->id, 1U);
    EXPECT_EQ(book.best(Side::Sell)->remaining, 6U);
    EXPECT_EQ(book.best(Side::Sell)->quantity, 10U);
    EXPECT_EQ(book.level_quantity(Side::Sell, 101), 16U);
    EXPECT_EQ(book.check_invariants(), "");
}

TEST(OrderBook, FullFillRemovesTheOrderThenTheLevel) {
    OrderBook book;
    book.add(make_order(1, Side::Sell, 101, 10));
    book.add(make_order(2, Side::Sell, 101, 3));
    book.add(make_order(3, Side::Sell, 102, 8));

    book.fill_best(Side::Sell, 10);
    EXPECT_EQ(book.find(1), nullptr);
    EXPECT_EQ(book.best(Side::Sell)->id, 2U);
    EXPECT_EQ(book.check_invariants(), "");

    book.fill_best(Side::Sell, 3);
    EXPECT_EQ(book.best(Side::Sell)->id, 3U);
    EXPECT_EQ(book.levels(Side::Sell), (Levels{{102, 8}}));
    EXPECT_EQ(book.check_invariants(), "");
}

TEST(OrderBook, RemoveTakesAnOrderOutOfTheMiddleOfALevel) {
    OrderBook book;
    book.add(make_order(1, Side::Buy, 100, 1));
    book.add(make_order(2, Side::Buy, 100, 2));
    book.add(make_order(3, Side::Buy, 100, 4));

    book.remove(2);

    EXPECT_EQ(book.find(2), nullptr);
    EXPECT_EQ(book.level_quantity(Side::Buy, 100), 5U);
    EXPECT_EQ(book.best(Side::Buy)->id, 1U);
    EXPECT_EQ(book.order_count(), 2U);
    EXPECT_EQ(book.check_invariants(), "");
}

TEST(OrderBook, RemovingTheLastOrderRemovesTheLevel) {
    OrderBook book;
    book.add(make_order(1, Side::Buy, 100, 1));

    book.remove(1);

    EXPECT_EQ(book.best(Side::Buy), nullptr);
    EXPECT_TRUE(book.levels(Side::Buy).empty());
    EXPECT_EQ(book.check_invariants(), "");
}

TEST(OrderBook, FindReturnsTheStoredOrder) {
    OrderBook book;
    book.add(make_order(7, Side::Sell, 105, 9, 42));

    const Order* order = book.find(7);

    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->session_id, 42U);
    EXPECT_EQ(order->side, Side::Sell);
    EXPECT_EQ(order->price, 105);
    EXPECT_EQ(order->remaining, 9U);
}

TEST(OrderBook, InvariantCheckReportsACrossedBook) {
    // The book itself does not prevent crossing; that is the engine's job.
    OrderBook book;
    book.add(make_order(1, Side::Buy, 101, 1));
    book.add(make_order(2, Side::Sell, 100, 1));

    EXPECT_NE(book.check_invariants(), "");
}

} // namespace
} // namespace exchange
