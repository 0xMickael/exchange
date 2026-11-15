#include "exchange/server/publisher.hpp"

#include <gtest/gtest.h>

#include <set>
#include <utility>
#include <vector>

namespace exchange::server {
namespace {

using Delivery = std::pair<SessionId, protocol::ServerMessage>;

// Publishes events and records who received what.
class PublisherTest : public ::testing::Test {
protected:
    std::vector<Delivery> publish(const Event& event) {
        std::vector<Delivery> deliveries;
        publisher_.publish(event,
                           [&](SessionId session_id, const protocol::ServerMessage& message) {
                               if (dead_sessions_.contains(session_id)) {
                                   return false;
                               }
                               deliveries.emplace_back(session_id, message);
                               return true;
                           });
        return deliveries;
    }

    void subscribe(SessionId session_id) {
        publish(SnapshotBegin{.session_id = session_id});
        publish(SnapshotEnd{.session_id = session_id});
    }

    Publisher publisher_;
    std::set<SessionId> dead_sessions_;
};

const Event a_trade = Trade{.price = 101, .quantity = 5, .aggressor_side = Side::Buy};
const Event a_book_update = BookUpdate{.side = Side::Sell, .price = 101, .total_quantity = 0};

TEST_F(PublisherTest, PrivateEventsGoToTheirSessionWithoutTheSessionId) {
    EXPECT_EQ(
        publish(OrderAccepted{.session_id = 7, .client_order_id = 1, .order_id = 4}),
        (std::vector<Delivery>{{7, protocol::OrderAccepted{.client_order_id = 1, .order_id = 4}}}));
    EXPECT_EQ(publish(OrderRejected{
                  .session_id = 7, .client_order_id = 2, .reason = RejectReason::InvalidPrice}),
              (std::vector<Delivery>{
                  {7, protocol::OrderRejected{.client_order_id = 2,
                                              .reason = RejectReason::InvalidPrice}}}));
    EXPECT_EQ(
        publish(Fill{
            .session_id = 8, .order_id = 4, .price = 101, .quantity = 5, .remaining_quantity = 7}),
        (std::vector<Delivery>{
            {8, protocol::Fill{
                    .order_id = 4, .price = 101, .quantity = 5, .remaining_quantity = 7}}}));
    EXPECT_EQ(
        publish(OrderCancelled{.session_id = 8,
                               .order_id = 4,
                               .cancelled_quantity = 7,
                               .reason = CancelReason::Requested}),
        (std::vector<Delivery>{{8, protocol::OrderCancelled{.order_id = 4,
                                                            .cancelled_quantity = 7,
                                                            .reason = CancelReason::Requested}}}));
    EXPECT_EQ(publish(CancelRejected{
                  .session_id = 8, .order_id = 4, .reason = CancelRejectReason::UnknownOrder}),
              (std::vector<Delivery>{
                  {8, protocol::CancelRejected{.order_id = 4,
                                               .reason = CancelRejectReason::UnknownOrder}}}));
}

TEST_F(PublisherTest, PublicEventsReachNobodyWithoutSubscribers) {
    EXPECT_TRUE(publish(a_trade).empty());
    EXPECT_TRUE(publish(a_book_update).empty());
}

TEST_F(PublisherTest, PublicEventsAreNumberedEvenWhenNobodyListens) {
    publish(a_trade);
    publish(a_book_update);

    EXPECT_EQ(publisher_.last_sequence(), 2U);
}

TEST_F(PublisherTest, SnapshotStatesTheLastSequenceAndSubscribesAtItsEnd) {
    publish(a_trade);       // sequence 1
    publish(a_book_update); // sequence 2

    EXPECT_EQ(publish(SnapshotBegin{.session_id = 7}),
              (std::vector<Delivery>{{7, protocol::SnapshotBegin{.last_sequence = 2}}}));
    EXPECT_EQ(
        publish(
            SnapshotLevel{.session_id = 7, .side = Side::Buy, .price = 100, .total_quantity = 3}),
        (std::vector<Delivery>{
            {7, protocol::SnapshotLevel{.side = Side::Buy, .price = 100, .total_quantity = 3}}}));
    // Not subscribed yet: the snapshot is not finished.
    EXPECT_TRUE(publish(a_trade).empty()); // sequence 3

    EXPECT_EQ(publish(SnapshotEnd{.session_id = 7}),
              (std::vector<Delivery>{{7, protocol::SnapshotEnd{}}}));

    EXPECT_EQ(publish(a_book_update),
              (std::vector<Delivery>{
                  {7, protocol::BookUpdate{
                          .sequence = 4, .side = Side::Sell, .price = 101, .total_quantity = 0}}}));
}

TEST_F(PublisherTest, PublicEventsGoToEverySubscriberWithTheSameSequence) {
    subscribe(9);
    subscribe(3);

    const protocol::ServerMessage expected =
        protocol::Trade{.sequence = 1, .price = 101, .quantity = 5, .aggressor_side = Side::Buy};
    EXPECT_EQ(publish(a_trade), (std::vector<Delivery>{{3, expected}, {9, expected}}));
}

TEST_F(PublisherTest, SubscribingTwiceDoesNotDuplicateMarketData) {
    subscribe(7);
    subscribe(7);

    EXPECT_EQ(publish(a_trade).size(), 1U);
}

TEST_F(PublisherTest, RemovedSessionNoLongerReceivesMarketData) {
    subscribe(7);
    subscribe(8);
    publisher_.remove_session(7);

    const std::vector<Delivery> deliveries = publish(a_trade);

    ASSERT_EQ(deliveries.size(), 1U);
    EXPECT_EQ(deliveries[0].first, 8U);
}

TEST_F(PublisherTest, SubscriberThatCannotReceiveIsDropped) {
    subscribe(7);
    subscribe(8);
    dead_sessions_.insert(7);
    EXPECT_EQ(publish(a_trade).size(), 1U);

    // Even if it could receive again, it stays unsubscribed: it missed an update.
    dead_sessions_.clear();
    const std::vector<Delivery> deliveries = publish(a_trade);

    ASSERT_EQ(deliveries.size(), 1U);
    EXPECT_EQ(deliveries[0].first, 8U);
}

TEST_F(PublisherTest, SessionThatIsGoneIsNotSubscribedByItsSnapshot) {
    dead_sessions_.insert(7);
    subscribe(7);
    dead_sessions_.clear();

    EXPECT_TRUE(publish(a_trade).empty());
}

} // namespace
} // namespace exchange::server
